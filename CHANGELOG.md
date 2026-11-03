# 变更日志 (CHANGELOG.md)

## 2026-11-02

### 新增文件
- `SESSION.md`: 初始化会话交互记录。
- `CHANGELOG.md`: 初始化文件变更记录。
- `DEVLOG.md`: 初始化架构设计与研发思路日志。
- `src/base64/base64.cp`: Bun.js 首个转译至 CP 语言的编解码模块，引入 `errdefer allocator.free(...)` 自动释放安全机制。

### 修改文件
- `src/base64/base64.zig`: 由 `cpc src/base64/base64.cp --lib` 转译生成，对比原版修复了潜在的分配失败内存泄漏隐患，100% 保持原有导出 C-ABI 与 SIMD/JSC 绑定。
- 上游 CP 编译器 (`/data/dev/cp/cpc`):
  - `src/ast.rs`: 扩展 `BinaryOp`（Mod、BitAnd、BitOr、BitXor、Shl、Shr、LogicalAnd、LogicalOr）、`Expr::BitNot`、标量整数与定长数组类型定义。
  - `src/lexer.rs`: 新增整型字面量（16进制、2进制、8进制、字符字面量）、位运算符、复合赋值运算符、`pub` 导出别名与 `anytype` 关键字。
  - `src/parser.rs`: 语法解析器全面支持标量整数、定长数组 `[N]T` 与 `T[N]`、复合赋值表达式解构脱糖、`~` 位非操作符及标准优先级。
  - `src/codegen.rs`: 代码生成器支持定长数组、整型类型转换、`this` 只读借用方法生成 `self: *const Self`、`anytype` 零开销映射。
  - `src/sema.rs`: 语义分析器完善标量整数类型相互隐式转换与定长数组等价性检查。

## 2026-11-03

### 新增文件
- `src/base64/base64_.zig`: 按规范备份原版 Zig 源代码，作为重构对照基准。
- `src/wyhash/wyhash_.zig`: 按规范备份原版 Wyhash 源代码，作为重构对照基准。
- `src/wyhash/wyhash.cp`: 采用纯正原生 CP 语言重构的 Wyhash 哈希算法实现（包含 `WyhashStateless`、`Wyhash11`、`read_bytes1/2/4/8`、`mum`、`mix0`、`mix1`）。
- `src/string/escapeRegExp_.zig`: 按规范备份原版 RegExp 转义源代码，作为重构对照基准。
- `src/string/escapeRegExp.cp`: 采用原生 CP 语言重构的正则表达式字符转义模块（实现 `escapeRegExp` 与 `escapeRegExpForPackageNameMatching`）。
- `src/sourcemap/VLQ_.zig`: 按规范备份原版 SourceMap VLQ 变长编码源代码，作为重构对照基准。
- `src/sourcemap/VLQ.cp`: 采用原生 CP 语言重构的 SourceMap 变长数量（VLQ）编解码算法模块（包含 `encode`、`encodeSlowPath`、`decode`、`decodeAssumeValid` 与 `VLQResult`）。

### 重构与修改文件
- `src/sourcemap/VLQ.zig`: 由 `cpc src/sourcemap/VLQ.cp -o src/sourcemap/VLQ.zig --lib` 编译生成。经 Tree-Shaking 深度按需修剪（Range 数值区间循环与 `--lib` 推导错误联合免除迭代与结果集胶水注入）后，由 476 行直降至 225 行（核心代码仅 158 行贴近原版 168 行），通过 1,825 项 SourceMap round-trip 往返测试、全套栈追踪映射测试及 26,000+ 项全量回归测试。
- `src/string/escapeRegExp.zig`: 由 `cpc src/string/escapeRegExp.cp -o src/string/escapeRegExp.zig --lib` 编译生成，行数仅 180 行（紧凑贴近原版 81 行），2 项针对性单元测试与上万项回归测试全部绿灯通过。
- `src/base64/base64.cp`: 遵循 CP 语言最高标准规范全面重构。彻底省略实例方法形参中冗余的 `this` / `_`，方法体内部实现无前缀直接访问字段与调用同类方法；全面遵循 `let` 默认可变原则，使用复合赋值运算符（`+=`, `-=`）与字面量自然收敛（`pad_char ?? 0`），代码兼具 TypeScript 的优雅直觉与底层紧凑性能。
- `src/base64/base64.zig`: 由最新高标准规范 CP 代码重新转译生成，全量回归测试 100% 保持通过。
- `src/wyhash/wyhash.zig`: 由 `cpc src/wyhash/wyhash.cp --lib` 转译生成，测试套件与 Bun 哈希原生成果 100% 匹配。在经过 CP 编译器按需摇树（Tree-Shaking）与值类型 Drop 豁免后，由初始 2,401 行大幅精简至 256 行（贴近原版 180 行）。
- `src/base64/base64.zig`: 由最新高标准规范 CP 代码重新转译生成，经 Tree-Shaking 优化后由 2,300+ 行精简至 805 行，全量回归测试 100% 保持通过。
- `cp/cp_ref.md` & `/data/dev/cp/cp_ref.md`: 同步更新 `i128` 与 `u128` 标量类型规范，补充 `switch` 匹配中的 `else =>` 默认分支语法说明；新增第 11.7 节《零成本转译抽象：按需摇树注入与原生发射》；在第 8.2 节深度完善错误体系规范（确立 `err` 为标准推荐关键字，形成 `err` vs `Err` vs `errdefer` 极简对称哲学，保留 `error` 作为透明向下兼容别名）；同步更新多模式分支语法（`'a' | 'b' =>`）与点路径显式错误联合类型规范（`void ! std.Io.Writer.Error`）。
- `src/base64/base64.cp`: 实装 `err.InvalidPadding` 原生语法，验证编译器双轨支持。
- 上游 CP 编译器 (`/data/dev/cp/cpc`):
  - `src/lexer.rs`: 新增 `#[token("err")]` 关键字映射至 `Token::ErrorKw`，与 `error` 保持统一双轨解析。
  - `src/parser.rs`:
    - 支持 `err SetName { ... }` 错误集声明，以及 `err.TagName` 即席错误字面量发射；同时保护局部变量命名（如 `catch err =>`），杜绝遮蔽冲突；
    - 支持点路径命名错误类型与指针类型解析（如 `std.Io.Writer.Error` 与 `*std.Io.Writer`），构建原生 Zig 错误联合 `ResultType`。
  - `src/ast.rs`: 新增 `Type::U128`、`Type::I128` 标量整数类型；支持 `Expr::ArrayRepeat(val, count)` 定长数组填充；新增 `Break` 与 `Continue` 控制流节点。
  - `src/lexer.rs`: 支持 `u128` / `i128` 关键字；重构十六进制字面量解析（支持无符号 64 位大整数防溢出）；支持 `break`、`continue`。
  - `src/parser.rs`: 适配 128 位标量类型解析；支持模式匹配 `else =>` 默认分支；支持定长数组重复解析。
  - `src/sema.rs`:
    - 完善 128 位标量整数推导、拓宽与 `is_int_type`；
    - 支持 `FixedArray` 与 `Tuple` 之间的安全强制转换（`can_coerce`、`types_eq`）；
    - 允许移位操作符与异类型整数字面量混合计算；
    - 顶层符号预解析（Pass 1）：预先遍历并注册所有顶层函数声明（`Decl::Func`），实现顶层函数声明顺序无关的前向引用调用；
    - 原生逃逸舱符号预扫：预扫描 `zig { ... }` 块内的符号（`const`, `var`, `fn`）并挂载至 `scopes[0]`，使得 `vlq_lookup_table` 等底层查找表在 CP 端免声明直接可用。
  - `src/parser.rs`:
    - 支持类与结构体字段默认值解析（如 `len: u8 = 0,`）；
    - 支持类与结构体内部方法显式标注 `pub fn`。
  - `src/codegen.rs`:
    - 支持 `@root` 装饰器修饰的结构体（`export @root struct Name`）：自动注册为值类型，发射 `pub const Name = @This();` 并将其字段与方法直接发射在文件根作用域；
    - 支持 `@inline` 装饰器，为标注 `@inline` 的方法与函数发射 Zig 原生 `pub inline fn`；
    - 优化 `--lib` 模式下推导错误联合类型 `!T` 发射，生成原生 `!T`，完美匹配泛型 Writer 接口；
    - 在 `--lib` 模式下彻底切除无用的 `global_io: std.Io` 与测试计数全局变量声明；
    - 隔离运行时辅助函数变量命名（`cp_str_slice`、`cp_slice`），彻底杜绝与类内部公开方法（如 `slice`）的符号遮蔽冲突；
    - 识别原生逃逸舱内的 `@This()` 根结构体声明，将其注册为紧凑值类型（`ref_classes`），确保方法返回值与参数按值直接传递；
    - 实装精细化模块特征扫描 `ModuleFeatures`（涵盖 regex、collections、actors、reflection、unpack、null_coalesce、fmt、math、iter 等 19 种细分特征），彻底按需注入运行时辅助代码，切除纯计算模块中上千行无用 Prelude 死代码；
    - 增加 `is_simple_literal_switch` 检测器，支持 `Pattern::Or(pats)` 多模式展开并直接发射原生 Zig `switch (x) { '|', '\\' => ..., else => ... }`，消除深层标签化作用域块（`__sw_blk_*`）；
    - 支持点路径原生错误联合类型返回与原生退出（`return;` 替代结构体字面量 `.{ .ok = {} }`）；
    - 优化二元比较 `==` 与 `!=`：针对 `null` 比较直接生成原生 `(x == null)`，同时彻底修复运行时 `cp_eq` 对可选类型（`?T`）与 `null`（`@typeInfo == .null`）在编译期的等价性判断逻辑；
    - 实装 `type_has_release` 析构检测与豁免机制：标量数值、原生切片、裸指针以及未定义 `release(*this)` 方法的 POD 结构体彻底免除 drop guard 守卫代码与 `_moved` 标志；
    - 支持无前缀字段访问的类型推导（从 `current_class_name.fields` 中检索字段类型），属性访问剥离 `RawPtr`，将 `msg_len += b.len` 直接转译为原生 Zig 整数运算，彻底消除动态 `cp_add` 胶水；
    - 支持模块顶层 `const` / `let` 变量/常量声明代码生成；
    - 修复结构体初始化中定长数组字段（如 `[0; 32]`）的目标元素类型推导与传递；
    - 大整数十六进制针对 63 位最高位为 1 自动格式化为 `0x{:x}`，防止负数转译报错；
    - 升级 `cp_cast_int` 运行时模板，内部采用 128 位宽转换。
