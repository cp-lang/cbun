# 2026-11-12 开发日志 (DEVLOG)

## 1. runtime/socket 全套接字子系统攻坚与架构提炼

### 1.1 所有权移动跟踪跨分支污染与消解方案
- **问题机理**：
  在 CPC 编译器的代码生成借用分析中（`codegen/stmt.rs`），当赋值表达式右侧为裸标识符且是指针类型时（`target = id;`），编译器会将 `id.sym` 加入 `moved_vars` 集合。由于 `moved_vars` 的生命周期是函数级而非块作用域级，在多分支结构中（例如多个连续的 `if (cond) |x|` 分支），即使变量位于完全互斥的分支，只要变量名重复或再次读取同一变量，就会触发 `error[E0203]: use of moved value`。
- **消解手段**：
  1. **分支捕获变量名隔离**：不同条件分支中不复用相同的捕获变量名（如 `prev_tls`, `prev_tcp`, `prev_sock` 分离）；
  2. **指针安全转换规避**：对跨引用指针转移使用 `bun.cast(TargetType, var_ptr)` 进行包装。转换表达式在 AST 中是函数调用节点而非裸标识符，不会被编译器标记为值移动，从根本上消除了虚假的所有权转移和 `__var_moved` 标志注入。

### 1.2 类字段遮蔽与方法只读性推导
- **问题机理**：
  在 `codegen/stmt.rs` 中，赋值语句检查左值标识符时首先通过 `is_class_field` 探测是否属于当前类的字段。若实例方法或静态方法内部的局部变量与类的某个字段同名（如 `let port = ...; port = ...;`），编译器会将其判定为对类字段的写操作。若当前方法未声明为 `*this`（可变借用），编译器即报错 `error[E0201]: Cannot mutate field in read-only method`。
- **消解手段**：
  类成员方法内部的局部变量严禁与类字段同名，统一采用语义化前缀或修饰词（如 `parsed_port`, `parsed_flags`, `packet_port`）。

### 1.3 C-ABI 回调语法契约
- **规范**：
  - 顶层函数采用 `@c fn name(...) : RetType { ... }`；
  - 结构体/类内的方法指针或结构体内方法，采用 `callconv(.c)` 标注（例如 `fn onPipeClosed(pipe: *uv.Pipe) callconv(.c) void { ... }`）。

### 1.4 泛型复合类型构造
- **规范**：
  接收编译期参数并动态生成类型的顶层泛型函数（例如 `pub fn NewSocket(comptime ssl: bool): type`），内部定义结构体时必须使用 `return struct { ... };`，而不能使用 `return class { ... };`。

---

## 2. uws_sys 网络层全子系统攻坚与技术总结

### 2.1 静态结构体工厂与方法模式
- **问题机理**：
  在原始 Zig 代码中，经常使用 `pub const connectGroup = struct { pub fn call(...) ... }.call;` 等匿名结构体闭包。但在 CPC 中，类内部的 `pub const ... = struct { ... };` 属于 Zig 原生常量捕获，其内部代码直接直通转译，导致内部声明的 `let` 无法被 CPC 状态机和语句转译器识别为 `var`，并在 AST-Check 中引发语法报错。
- **消解手段**：
  将此类匿名结构体调用重构为纯正的 CP 静态类方法：`pub static fn connectGroup(...): !Self { ... }`。CPC 正确识别方法作用域，将 `let stack: [256]u8 = undefined;` 正确降级为 Zig 的 `var stack: [256]u8 = undefined;`。

### 2.2 泛型函数中的类型表达式规避
- **问题机理**：
  在函数签名或参数传递中，`@sizeOf(?*This)` 或 `raw.ext(?*This)` 中直接出现 `?*This` 时，由于 `?` 作为前缀在 CP 表达式解析器中被视为非法 Token，导致解析中断报 `unexpected token 'Semicolon'`。
- **消解手段**：
  在方法体内将复合类型显式提升为类型别名：`const ThisPtr = ?*This;`，然后传递标识符 `@sizeOf(ThisPtr)` 和 `raw.ext(ThisPtr)`，完美桥接类型位置与值位置的语义界限。

---

## 3. runtime/webcore/blob 核心存储子系统攻坚与技术总结

### 3.1 CP 借用检查可变方法接收器（`*this: *Type`）
- **问题机理**：
  在 CP 语言定义与编译实现中（`cpc/src/parser.rs:1357` & `cpc/src/codegen/actor.rs:8`），方法的首个参数若为 `this: *Type`，不带有 `*` 前缀，其 `is_mut` 为 `false`，接收器模式被判定为只读 `ReceiverMode::ReadOnly`。方法体内任何对 `this.field` 的赋值或可变借用都会触发 `E0201: cannot perform mutable operation in a read-only method`。
- **消解手段**：
  所有需要修改实例字段的方法，接收器统一声明为 `*this: *ClassName`（或 `*this`），使编译期将方法标记为 `ReceiverMode::Mut`，精准匹配单写多读借用检查契约。

### 3.2 类内 C 回调静态化声明（`@c static fn`）
- **问题机理**：
  类内部用于事件驱动或 libuv/POSIX 的回调函数（如 `@c fn onRead(req: *libuv.fs_t): void`），若未带有 `static` 修饰符，CPC 默认将其视作类实例方法并赋予只读接收器。当回调内部通过 `@fieldParentPtr` 还原出父指针并对其字段赋值时，由于字段名与当前类字段重名，编译器借用检查触发虚假报警。
- **消解手段**：
  所有非接收器成员回调函数统一加上 `@c static fn`，将其编译期上下文从实例方法中完全剥离，消除误报。

### 3.3 顶层类型别名与函数指针调用（`export type Name = ...`）
- **问题机理**：
  若使用 `const Name = *const fn (...) void;` 定义函数指针类型，该标识符被注册为运行时变量而非类型定义。在调用其实例变量（如 `cb(ctx, data)`）时，语义检查因无法推断函数签名而报 `E0216: expression is not a callable function`。
- **消解手段**：
  在顶层使用纯正 CP 类型别名语法：`export type Name = *const fn (...) void;`。CPC 将其正确注册进 `type_aliases` 符号表，同时生成 Zig 标准 `pub const Name = ...;`，完美解决回调变量的语义推断与调用。

---

## 4. Milestone 4 攻坚总结：`src/runtime/webcore/s3/`（8个模块全部达成）

### 4.1 字符串切片推导与静态类型转换机制
- **问题机理**：
  在 XML 解析过程中，通过 `bytes[start .. end]` 截取字符串，由于 `const bytes = list.items` 未带类型标注，其在语义分析时被推断为 `Array(Auto)`，导致赋值给 `string` 或 `?string` 时触发 `E0207: type mismatch: expected 'String', found 'Array(Auto)'`。
- **解决手段**：
  为源切片显式声明类型标注 `const bytes: string = list.items;`。经此标注后，切片表达式推导出 `Array(U8)`，进而命中 `sema.rs:4650` 中允许将 `Array(U8)` 强制转换为 `String` 的安全类型规则。

### 4.2 函数参数中匿名结构体逗号解析歧义
- **问题机理**：
  在 CP 解析器中，直接在参数位置书写多行匿名结构体 `fn_call(.{ .a = 1, .b = 2 }, arg2)` 时，结构体闭合大括号后的逗号易被解析器判为词法异常。
- **解决手段**：
  提取前置常量声明 `const opts = .{ .a = 1, .b = 2 }; fn_call(opts, arg2)`，既提升代码可读性与复用性，又彻底根除了解析器的上下文歧义。

### 4.3 静态回调中的借用检查隔离
- **问题机理**：
  在类内部定义被 C-ABI 捕获的回调函数时，如果定义为普通方法，CPC 默认赋予只读接收器。当通过 `@ptrCast` 获取实例指针并修改内部字段时，借用检查器会将此类修改视作针对局部只读接收器的非法写操作。
- **解决手段**：
  强制将所有非成员 C-ABI 回调声明为 `@c static fn`，并通过 `let self: *T = @ptrCast(@alignCast(ctx));` 明确可变借用权限，确保在生命周期内合规解构与写入。

---

## 5. webcore 核心层攻坚技术剖析（新增 13 模块达成双重通过）

### 5.1 元组解构与匿名结构体类型不兼容（`E0207`）根因与方案
- **问题机理**：
  在 CPC 语法推导中，`let [a, b] = expr;` 仅对 `Type::Tuple` 与 `Type::Array` 生效。当表达式为匿名结构体字面量（如 `break :blk .{ x, y };`）时，编译器将其判定为 `Type::Object`，触发 `E0207: expected 'Tuple([])', found 'Object([])'`。
- **解决手段**：
  解耦数组模式解构，采用统一的安全属性解构范式：
  `const pair = expr; const a = pair.@"0"; const b = pair.@"1";`
  此范式完全规避了元组与对象在 AST 层次上的不匹配，同时生成紧凑合规的 Zig 代码。

### 5.2 复杂对齐指针与 SIMD 内存类型的类型隔离
- **问题机理**：
  在 `encoding.cp` 等底层编码模块中，针对未对齐 UTF-16 内存缓冲区的处理使用了 Zig 的 `[*]align(1) u16` 与 `[]align(1) u16`。CP 语法解析器在类型表达式位置未挂载 `align(N)` 属性修饰符，导致直接书写时触发 `E0111: expected Comma, found U16`。
- **解决手段**：
  利用 CP 对结构体（`struct { ... }`）内直通代码的透明支持，声明类型桥接结构体：
  `pub const Types = struct { pub const U16Slice = []align(1) u16; pub const U16Ptr = [*]align(1) u16; };`
  在业务代码中直接使用 `Types.U16Slice` 与 `Types.U16Ptr`，消除了语法解析缺陷，同时保留了 SIMD 与 SIMDUTF 极致性能。

### 5.3 跨模块单例与内部结构体静态变量消解
- **问题机理**：
  在 `ObjectURLRegistry.cp` 和 `FileSink.cp` 中，原生代码在局部方法或内部结构体内定义了 `var registry: ... = undefined;` 和 `pub var live_count = ...`。由于 CP 规范严格禁止 `var`，且类内直接声明非 const 静态变量会产生字段属性混淆。
- **解决手段**：
  将静态单例与原子状态提升至顶层模块作用域，声明为 `let singleton_registry: ObjectURLRegistry = undefined;`，并配套 `let singleton_once = std.once(...);`。类方法内部直接安全借用指针，完全遵循纯 CP const/let 规范。

---

## 6. 最终决战攻坚规划：全仓最后的 9 个模块（集中攻坚与冲刺）

1. **最后 9 个模块清单**：
   - 存储与高层客户端：`FileReader.cp` (655 行), `S3File.cp` (658 行), `Blob.cp` (228 行)
   - 核心流管道：`ReadableStream.cp` (753 行), `streams.cp` (604 行)
   - 核心 HTTP 客户端与实体：`Response.cp` (484 行), `Request.cp` (575 行), `Body.cp` (410 行), `fetch.cp` (385 行)
2. **冲刺目标**：
   - 攻克剩余 9 个模块，冲刺全仓 **258 / 258 (100%)** 纯正 CP 语言双重编译通过！


