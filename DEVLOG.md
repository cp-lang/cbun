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

## 2. 下一阶段攻坚规划：`src/uws_sys/`（9个模块）与 `src/runtime/webcore/`（27个模块）

1. **`src/uws_sys/` 重点攻坚清单**：
   - `BodyReaderMixin.cp`
   - `Client.cp`
   - `HttpResponse.cp`
   - `Loop.cp`
   - `MultipartParser.cp`
   - `PerMessageDeflate.cp`
   - `socket.cp`
   - `TopicTree.cp`
   - `WebSocket.cp`
2. **方法论**：
   - 先对 9 个文件进行编译诊断扫描，提取具体报错特征；
   - 坚持 100% 纯 CP 语言（0 处 `zig {}`，0 处 `var`）；
   - 双重验证（`cpc --lib` + `vendor/zig/zig ast-check`）确保绝对正确；
   - 膨胀率严守 $\le +20\%$；
   - 阶段闭环后更新四线文档并原子 git 提交。
