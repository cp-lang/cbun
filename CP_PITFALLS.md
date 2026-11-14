# CP 迁移踩坑速查表（子智能体唯一必读文档，约 3KB；发现新坑请追加一行）

> 与 GEMINI.md 第七节同源。**不要**全文读取 `/data/dev/cp/cp_agent_ref.md`。

## A. 新版 cpc 10 条规则（cp agy 2026-11-14 下发）
1. 接收者：禁 `self`/`this`（E0121）。体内用 `_`。只读省略首参（写 `(_)` 报 E0123）；修改 `_.field`（含 `init`）写 `*_`；转移所有权 `move _`。
2. 形参：只读 `v: T`；可变 `v: *T`；`move v: T` / `own v: T`。禁 `*v: T`、`mut v: T`、`let v: T`（E0104/E0125）。
3. 局部只用 `const`/`let`，禁 `var`/`mut`。忽略返回值写 `* = expr;`，禁 `_ = expr;`（E0120）。
4. 有返回值必须 `): T`（漏冒号 E0116）；void 一律省略 `: void`（E0122，`*void` 指针保留）。
5. 可选后缀 `T?`（禁 `?T`，E0115）；错误联合 `T!` / `T!E`；禁 `any`（E0119），泛型推导用 `auto`。
6. 禁 Zig `|capture|`（E0118）：`if (let v = opt)`、`while (let v = it.next())`、`for (const v of arr)`；`while (..) : (i += 1)` 合法。
   - ⚠ 当前已安装的 cpc 若不认 `let v = e`（报 E0100 unexpected 'Let'），改用 `if (v := e)` / `while (v := e)`；`scripts/zig2cp_prep.py` 会自动探测。
7. C-FFI：`@c { fn foo(a: i32): i32; struct bar {..} enum baz: c_int {..} type X = ..; const K: c_int = 1; }`；导出 `pub @c {..}`；块内不写 `pub`/`extern`。
8. struct 字段/方法、enum 方法默认 `pub`，删内部多余 `pub`；静态方法写 `static fn`；顶层导出用 `pub`。空切片 `buf: []T = [];`（禁 `&.{}`）。
9. 删除 `const std = @import("std");`，直接用 `std.*`、`builtin`；**严禁**为调 std 使用 `zig {}`。
10. 编译宏 `@is_nest` 替代 `@is_beam`。

## B. 已知 lowering / 语法坑
- `pub const X = struct {..};` 内部不会被 lower → 改 `pub struct X {..}`；二级嵌套 struct/复杂常量提升到模块顶层。
- `err` 是关键字：禁作变量/捕获名（用 `e`）；错误集 `err X { A, B }`（非 `error{..}`）。
- CP 关键字做标识符需转义：`in as let import arena from impl meta macro err type` → `@"arena"`。局部变量改名（如 `arena_`）更稳（`defer @"arena".x()` 会解析失败）。
- 文件级根结构体（顶层字段 + `const X = @This();`）→ `@root struct X { ... }`，import 放文件顶部。
- 切片字面量/元组：`t[0]`、`let [a, b] = t`，禁 `.@"0"`；枚举 tag 名为关键字（如 `static`）转 `@"static"`。
- `return` 是语句非表达式：`.tag => return,` 会破坏括号栈 → `.tag => {},`。
- `x orelse { block }`、`catch { block }` 的块形式 → 改写成语句级 `let x = opt; if (x == null) { ... }`。
- 表达式位置不要用 `:=`；先 `let x = ..; if (v := opt) { x = ..; }`。
- switch 赋给无符号整型时字面量分支写 `@as(u32, 0)`（避免 E0207）。
- `try a ?? b` 优先级歧义 → `(try a) ?? b` 写成 `(a?) ?? b` 或语句级。
- 单参 `obj.slice(arg)` 会被当数组切片 → `const f = Type.slice; f(obj, arg)`。
- 数组长度表达式含 `*`（`[std.fs.max_path_bytes * 2]u8`）当前不支持 → 先 `const N = std.fs.max_path_bytes * 2;` 再 `[N]u8`。
- Zig 多行字符串 `\\...` 不支持 → 拼接普通字符串 + `\n`。
- 文件尾部的 `extern fn` 声明，若调用点前未识别（E0206）→ 提到文件顶部。
- 形参名与同文件顶层函数同名会遮蔽（如 `ptr`）→ 改名。

## C. 工作流（节省 Token）
1. `python3 scripts/zig2cp_prep.py <a.zig> <b.zig>`（备份 + 机械转换）。
2. `python3 scripts/cpc_brief.py -n 8 <a.cp> <b.cp>`（编译 + ast-check + 4 门禁，输出按错误码聚合，**不要**直接看 cpc 原始输出）。
3. 按 `edit_file` 精修；同一类错误一次修完整个文件再重编；单文件 ≤ 50 步，超限汇报 BLOCKED。
