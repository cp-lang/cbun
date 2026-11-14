# Bun.js 1.3.14 (Zig) → 100% 纯 CP 语言重构开发指令与工作流指南（精炼常驻版）

> **历史完整背景归档**：长篇背景说明、早期教程与历史模板已归档至 `GEMINI_H.md` 与 `AGENTS_H.md`。本文件作为主智能体与子智能体每步自动加载的唯一常驻核心准则，保持高度精炼以节省 Token。

---

## 一、 核心使命与角色定位

- **核心目标**：将 **Bun.js 1.3.14 (`src/**/*.zig`)** 100% 重构为 **纯 CP 语言 (`.cp`)**，利用 CP 语言的借用检查（Borrow Checker）、单写多读排他安全（`*this`/`this`）、所有权转移（`own`/`move`）与 `unsafe {}` 显式收窄，根除悬垂指针与内存泄漏。
- **单模块交付标准（4 项硬门禁）**：
  1. **原文件备份**：转换 `<stem>.zig` 前，若 `<stem>_.zig` 不存在，先备份 `cp <stem>.zig <stem>_.zig`（**严禁覆盖已存在的 `<stem>_.zig` 原始备份**）。
  2. **编译与语法校验**：`cpc --lib <stem>.cp -o <stem>.zig && vendor/zig/zig ast-check <stem>.zig` 必须 0 报错通过（且不生成多余 `.zig.map` 入库）。
  3. **静态零违例门禁**：`.cp` 源码中必须满足：
     - **0** `zig { ... }` 逃逸块；
     - **0** `var` 变量声明（仅限 `const` 与 `let`）；
     - **0** `.@"0"` / `.@"1"` 元组属性访问（必须使用 `t[0]` 或 `let [a, b] = t`）；
     - **0** `*anyopaque` / `?*anyopaque` / `*any!` / `*any!?`（必须使用 `*!` 或 `*!?` 或 `*void` / `?*void`）。
  4. **代码膨胀率门禁**：`<stem>.cp` 行数不得超过 `<stem>_.zig` 原始行数的 **+20%**（`<= 1.2x`）。

---

## 二、 严格操作纪律与工具路由 (CRITICAL)

1. **强制工具映射**：
   - **修改/新建源码文件**：优先调用 `edit_file` / `write_file`，或通过 `scripts/zig2cp_prep.py` 批量生成初稿后再用 `edit_file` 精修。
   - **阅读文件**：调用 `read_file`（指定 `StartLine`/`EndLine`）；对于 `> 800` 行的大文件或批量小文件，允许用 `run_command` 执行 `rg -n` 提取结构大纲或批量预览。
   - **代码检索与编译验证**：调用 `run_command` 执行 `rg -n`、`find`、`cpc`、`vendor/zig/zig ast-check` 等，支持管道（`|`）与重定向（`>`、`2>&1`）。
2. **清理与临时文件规范**：
   - 所有临时探测文件、最小复现用例统一存放在 `temp/` 目录（Git 已忽略），严禁污染项目根目录或 `/root/`。
   - 具备直接清理 `temp/`、`bin/`、`.cap/` 下临时产物的完全权限，无需向用户确认。
3. **编码与汇报中性化规范**：
   - 在撰写 `SESSION.md`、`DEVLOG.md` 及对话汇报时，使用中性工程术语（用「异常边界防护 / 畸形报文校验」替代含 attack/exploit 词汇，用「资源耗尽防护」替代 bomb/flood，用「终止生命周期」替代敏感进程控制词）。
   - 避免将测试夹具中的大段 Base64/证书/构造载荷打印到上下文中。
   - 交互语言统一使用**中文**。

---

## 三、 四线文档冷热分离与 Git 原子提交铁律

### 1. 四线文档冷热分离（仅主智能体维护，子智能体严禁修改）

- **热日志文件（仅记录当日活动）**：
  - `SESSION.md`：按时间顺序追加用户指令、主智能体汇报、**以及与 `cp agy` 的每一次任务发送与接收详情**（严禁覆盖或删减）。
  - `CHANGELOG.md`：按里程碑追加新增/修改的 `.cp`、`.zig`、`_.zig` 文件清单。
  - `DEVLOG.md`：按里程碑追加核心技术方案、语法 lowering 经验与问题解决思路。
- **冷归档文件**：超过当日的历史记录归档至 `SESSION_H.md`、`CHANGELOG_H.md`、`DEVLOG_H.md`（永久保留）。
- **记录节奏**：除 `cp agy` 双向通信需实时写入 `SESSION.md` 外，其余模块迁移按阶段性里程碑（每完成 1~3 个子目录）集中汇总写入一次，90% 火力集中于代码攻坚。

### 2. Git 原子提交铁律（仅主智能体执行）

1. **阶段里程碑自动提交**：每完成一个完整子目录或一批模块闭环验证后，自动完成 Git 提交。
2. **先写全文档**：提交前先更新 `SESSION.md`、`CHANGELOG.md`、`DEVLOG.md`。
3. **一次性暂存与提交**：精确 `git add` 变更的源码与文档（注意排除被 `.gitignore` 忽略的 `*.zig.map`），只执行唯一 **1 次** `git commit`。
4. **提交后绝对止步**：严禁 `--amend`，严禁事后补充二次提交。

---

## 四、 与上游 `cp agy` 跨智能体异步双向通信协议（常驻核心）

- **职责边界铁律**：
  - **主智能体 (`bun_agy`) 与子智能体严禁直接修改 `/data/dev/cp/cpc/src` 编译器源码！**
  - 所有 `cpc` 编译器 Bug 或缺失特性，必须且只能通过异步任务看板提交给上游 `cp agy` 修复。
- **任务存储与队列工具**：
  - 待处理队列：`/data/dev/cp/.tasks/pending/<task-id>.json`
  - 已解决归档：`/data/dev/cp/.tasks/resolved/<task-id>.json`
  - 事件流日志：`/data/dev/cp/TASK_QUEUE.jsonl`
- **标准操作指令**：
  1. **后台常驻监听守护进程（主智能体启动/重启后必检）**：
     主智能体必须在后台保持运行 `watch_task.py` 守护任务（`IsDaemon: true, WaitMsBeforeAsync: 500`）：
     ```bash
     python3 scripts/watch_task.py --status resolved
     ```
     一旦 `cp agy` 结单，守护进程退出并触发 Reactive Wakeup 自动唤醒主智能体（休眠期间 0 Token 消耗）。
  2. **遇到编译器缺陷时的标准上报流程（绝不单点死锁、绝不用非标语法绕过）**：
     - 单个文件编译修复尝试不超过 **5 轮**（Token 熔断安全阀）；
     - 提炼 `<= 10` 行最小可复现代码写入 `temp/repro_<name>.cp`；
     - 调用投递脚本：
       ```bash
       python3 scripts/task_queue.py submit --id "task-YYYYMMDD-<slug>" --repro "temp/repro_<name>.cp" --error "简明报错说明与代码生成根因定位" --by "bun_agy"
       ```
     - 将受阻文件标记为 `BLOCKED`（保留原 `<stem>.zig` 不被破坏），立即切换迁移下一个无依赖模块。
  3. **查看看板状态**：
     ```bash
     python3 scripts/task_queue.py list
     ```
- **双向通信双轨强制留痕规范**：
  - **发送任务给 `cp agy` 时**：必须在对话屏幕显式打印任务摘要（Task ID、Repro 代码、Error 根因），并**第一时间实时追加写入 `SESSION.md`**。
  - **收到 `cp agy` 结单通知时**：必须在对话屏幕显式打印结单详情（Task ID、修复结论、复现验证结果），**第一时间实时追加写入 `SESSION.md`**，并立即用新 `cpc` 编译解封此前 `BLOCKED` 的文件。

---

## 五、 Token 极致节省与子智能体并行调度 5 大铁律 (CRITICAL)

为防止配额因长上下文累积而快速耗尽，主智能体与子智能体必须严格执行以下 5 大铁律：

1. **铁律 1：并发上限 `<= 2` + 「用完即弃」短生命周期子智能体（严禁跨批次 `send_message` 复用！）**
   - 同时运行的子智能体严格限制在 **最多 2 路**。
   - **彻底禁止通过 `send_message` 向已完成上一批任务的子智能体追加新目录批次**！复用子智能体会使历史上下文膨胀至 200k~300k Tokens/step（$O(N^2)$ 平方级消耗）。
   - 每批任务必须通过 `invoke_subagent` 启动**全新子智能体**（从 0 上下文起步），单次任务控制在 **30~50 步以内**完成即结束。
2. **铁律 2：预转换脚本先行（`scripts/zig2cp_prep.py`）+ `edit_file` 精修**
   - 对待迁移的 `.zig` 文件，子智能体第一步优先运行：
     ```bash
     python3 scripts/zig2cp_prep.py <file1.zig> <file2.zig> ...
     ```
     该脚本自动创建 `<stem>_.zig` 备份并生成完成 80% 机械语法转换的 `<stem>.cp` 初稿。
   - 子智能体随后运行 `cpc --lib` 定位剩余的 15%~20% 语义/结构问题，并用 `edit_file` 精准修复，避免让大模型从零手写上千行重复代码（节省 70% Output Tokens）。
3. **铁律 3：工具调用批处理化（Batch Read / Parallel Edit / Single-Command Verify）**
   - 严禁对 10~50 行的小文件逐个串行执行 `read_file` → `write_file` → `cpc`。
   - 必须一次性批量预转换、同一轮并行调用多个 `edit_file`/`write_file`、并用单条 `for` 循环命令一次性完成整批文件的 `cpc --lib` 与 `zig ast-check` 验证。
4. **铁律 4：内置精炼语法速查，免除重复全文读取 `/data/dev/cp/cp_agent_ref.md`**
   - 子智能体直接使用下方的「第六节：CP 纯化语法与 `cpc` 代码生成速查表」，**严禁在子智能体启动时全文读取 57KB 的 `/data/dev/cp/cp_agent_ref.md`**；仅当遇到生僻语法时才用 `rg -n` 按行检索 `/data/dev/cp/cp_agent_ref.md`。
   - 注意：子智能体 Prompt 中不要直接包含复杂正则字符串（静态正则扫描统一由主智能体在验收阶段执行）。
5. **铁律 5：配额耗尽（Quota Exhaustion 429）自主定时恢复**
   - 若遇到 `RESOURCE_EXHAUSTED (code 429): ... Resets in XhYmZs`，主智能体计算重置秒数 + 120 秒缓冲，调用 `schedule(DurationSeconds=..., Prompt="配额已重置，请检查状态并继续迁移")` 注册单次唤醒定时器后立即休眠结束轮次。

---

## 六、 CP 纯化语法与 `cpc` 代码生成实战速查表（旧版；与第七节冲突处以第七节为准）

1. **模块导入**：
   - `import std from "std";`、`import bun from "bun";`、`import Foo from "./foo.zig";`（严禁 `@import(...)`）。
2. **变量与常量**：
   - 仅用 `const`（只读）与 `let`（可变）；**严禁 `var`**，**严禁 `let mut`**。
   - 注意：`err` 是 CP 关键字（用于声明错误集 `err MyError { ... }`），**严禁将变量、参数或 `catch |e|` / `.err e =>` 的载荷命名为 `err`**（统一命名为 `e`）。
3. **类型与指针**：
   - 原始无类型指针：`*!`（非空，对应 `*anyopaque`）、`*!?`（可空，对应 `?*anyopaque`）或 `*void` / `?*void`。
   - 可空类型指针：注意优先级！`*T?` 会被解析为「指向可空 `T` 的非空指针 `*(T?)`」；**可空指针必须写为 `(*T)?` 或 `?*T` 或 `*T!?`**。
   - 多元素裸指针切片：`[*]u8!` / `[*]const u8!`；对其进行切片 `ptr[0..len]` 或解引用 `ptr.*` 或 `@ptrCast`/`@alignCast` 必须包裹在 `unsafe { ... }` 块中。
   - 空切片初始化：若需要空可变切片 `[]T`，在模块顶层声明 `let empty_x: [0]T = [];`，使用 `empty_x[0..0]`。
4. **结构体、枚举与联合体**：
   - 文件级根结构体：`@root struct Foo { ... }`（或 `pub struct Foo { ... }`）。
   - 位域/紧凑结构体：`@packed(u8) pub struct Flags { ... }`；C ABI 结构体：`@c pub struct CStruct { ... }`；不透明类型：`@opaque pub struct Opaque {}`。
   - **重要 lowering 规则**：`struct` 或 `enum` 内部嵌套的二级子结构体（如 `pub const Sub = struct { ... };`）或包含 CP 语法的复杂 `pub const` 初始化器，会被 `cpc` 原样输出而不经过 CP 表达式 lowering。**必须将二级子结构体或复杂常量提升到模块顶层声明（`pub struct Sub { ... }`），再在父结构体内写 `pub const Sub = TopLevelSub;`**。
   - 若联合体含名为 `name` 的字段（如 `name: Data`），使用 `pub union(enum) Foo` 声明，避免与 `pub enum` 自动生成的 `pub fn name()` 方法冲突。
5. **函数与方法接收者**：
   - 接收者规范：在 `enum`（及最新规范下的类型方法）中，只读实例方法省略接收者形参、在函数体内通过 `_`（或 `_.field`）访问当前实例；可变实例方法首参写 `*_`；静态方法必须显式标注 `static fn`（`enum` 内部成员默认 `pub`，禁止在 `enum` 内写冗余 `pub`）。对于兼容 `*this` / `this` 的 `struct`，若遇 `E0121`，一律切换为省略只读接收者 + `_` / `*_`。
   - 无返回值函数：直接省略 `: void`（若可能返回错误写 `fn foo()!`）。
   - 具名可变参数：形参需在函数体内修改或作为可变借用时，写为 `param: *T`（或在函数首行 `let p = param;`）。
   - 编译期参数修饰：写 `comptime T: type`（**不要写 `@comptime`**）。
   - 线程局部变量：写 `@tls pub let x: T = ...;`（**`@tls` 必须放在 `pub` 之前**）。
   - 值传递可变参数：函数体内需修改按值传入的参数时，声明为 `mut param: T` 或在函数首行 `let p = param;`。
   - 单参数 `.slice(arg)` 调用：对自定义类型调用单参数 `obj.slice(arg)` 会被 `cpc` 当作数组切片 `[arg..]`，请改写为 `const sliceFn = Type.slice; sliceFn(obj, arg)`。
6. **控制流与运算符**：
   - 布尔逻辑：严格使用 `&&` 与 `||`（严禁 `and` / `or`）；空值合并使用 `??`（或 `orelse`）。
   - 错误传播：后缀 `expr?`（替代前缀 `try expr`）；错误捕获 `expr catch val` 或 `expr catch |e| { ... }`。
   - 可选值解包：语句级支持 `if (val := opt_expr) { ... }`、`while (val := opt_expr) { ... }`；注意在**表达式位置**（如 `const x = if (...) a else b`）不要使用 `:=`，请先声明 `let x = ...; if (val := opt) { x = ...; }`。
   - `switch` 语法：枚举/联合体分支写 `.tag => |payload| ...` 或 `.tag payload => ...`；注意当 `switch` 赋给 `u8`/`u32` 等无符号整数变量时，字面量分支写 `@as(u32, 0)` 避免默认推导为 `i32` 触发 `E0207`。
   - 结构体字面量：`Foo{ a: 1, b: 2 }` 或匿名 `{ a: 1, b: 2 }`（字段名不加前导点 `.a = 1`）。
   - 标识符转义：遇到 CP 关键字作为字段/方法/枚举名（如 `in`, `as`, `let`, `import`, `arena`, `from`, `impl`, `meta`, `macro`, `err`, `type`），必须转义为 `@"in"`, `@"arena"`, `@"macro"` 等。

---

## 七、 新版 `cpc` 语法规则（cp agy 2026-11-14 下发；与第六节冲突时**以本节为准**）

完整速查与踩坑清单见仓库根目录 **`CP_PITFALLS.md`**（约 3KB，子智能体启动时只读它，不读 `cp_agent_ref.md`）。要点：

1. **接收者**：禁 `self`/`this`；体内用 `_`；只读省略首参（写 `(_)` 报 E0123）；修改字段/`init` 写 `*_`；转移所有权 `move _`。
2. **形参**：只读 `v: T`；可变 `v: *T`；`move v: T` / `own v: T`；禁 `*v: T` / `mut v: T` / `let v: T`。
3. **局部变量**：仅 `const`/`let`；忽略返回值写 `* = expr;`（禁 `_ = expr;`）。
4. **返回类型**：有返回值必须 `): T`；void 全场景省略 `: void`（`*void` 保留）。
5. **类型修饰**：可选后缀 `T?`（禁 `?T`）；错误联合 `T!` / `T!E`；禁 `any`，泛型推导用 `auto`。
6. **条件/循环**：禁 `|capture|`；只读 `if (v := opt)`、`while (v := it.next())`；可变 `if (*v := opt)`；`for (v of arr)` / `for (*v of arr)` / `for (move v of arr)`（无需写 const/let）；`while (..) : (i += 1)` 合法。**`let v = e` 形式 cpc 不支持**。
7. **C-FFI**：`@c { fn foo(a: i32): i32; struct bar {..} enum baz: c_int {..} }`，导出 `pub @c {..}`，块内不写 `pub`/`extern`。
8. **可见性**：struct 字段/方法、enum 方法默认 `pub`（删多余 `pub`），静态方法 `static fn`；空切片 `= []`（禁 `&.{}`）。
9. **std**：删除 `const std = @import("std");`，直接用 `std.*`；严禁为调 std 用 `zig {}`。
10. **编译宏**：`@is_nest` 替代 `@is_beam`。
11. **一等公民内置类**：`JSON` 与 `RegExp` 已作为全局一等公民内置类（无需 `import`，且与 Bun 100% 同构）。直接使用 `JSON.stringify(val)`、`JSON.parse<T>(str)`、`JSON.parse(str)` 以及 `/pattern/flags` 或 `new RegExp(pattern, flags)`。废除早期遗留的 `@json_parse`。

## 八、 Token 节省固化铁律（2026-11-14 追加，主/子智能体必须遵守）

1. **工具链**：
   - `python3 scripts/zig2cp_prep.py <a.zig> ...`：备份 + 按第七节规则机械转换（接收者、返回类型、`|capture|`、`?T`、`try`、struct/err 声明等）。
   - `/data/dev/cp/cap/bin/cpc fix <file.cp>`（或 `cpc check --fix`）：利用编译器内置修复能力自动完成 `E0101`..`E0104`, `E0115` (`?T`->`T?`), `E0116` (`:`), `E0118` (`|v|`), `E0119`, `E0120` (`* =`), `E0121` (`*_`), `E0122`, `E0123`, `E0126` 的多轮就地机械修复。
   - `cpc check --brief <file.cp>` / `python3 scripts/cpc_brief.py -n 8 <a.cp> ...`：聚合去重诊断模式，折叠 `cascade_of` 连带错误；一条命令完成 `cpc check --json --brief` 提取 + `cpc --lib` 编译 + `ast-check` + 4 门禁。**严禁**直接查看 `cpc` 原始报错（单次 40~70KB）。
2. **踩坑表驱动**：子智能体 Prompt 只写「读 `CP_PITFALLS.md`、按其 C 节工作流执行」+ 目标文件与门禁，不再内联长篇语法说明。发现新坑由主智能体追加一行到 `CP_PITFALLS.md`（子智能体可在汇报中提交）。
3. **步数熔断**：子智能体单文件 ≤ 50 步仍未过门禁 → 汇报 `BLOCKED` 并停止；>2,500 行大文件按函数段拆给多个全新子智能体；同类错误一次修完整个文件再重编，禁止逐条编译。
4. **主智能体禁止轮询**：不得为“看进度”解析子智能体 transcript / 重复 `manage_subagents list`；只等 `send_message` 唤醒。收到汇报后由主智能体自己用 `cpc_brief.py` 一条命令验收。
5. **汇报精简**：阶段汇报只给 3~5 行数字（模块数、闭合目录数、膨胀率区间）；`SESSION/CHANGELOG/DEVLOG` 用单行条目追加，不重复叙述；每 3~5 个模块（或闭合一个目录）提交一次。
6. **配额耗尽（429）**：计算重置秒数 + 120 秒，`schedule` 单次唤醒后立即结束轮次，唤醒后重派全新子智能体（不复用旧的）。
7. **存量净化**：已迁移 `.cp` 若含旧写法（`this`/`_ = `/`: void`/`?T`/`|capture|`/`zig {}` 取 std 等），在新 `cpc` 全量生效后分批用 `cpc_brief.py` 批量发现并净化，优先于新增迁移前先抽样 3 个目录验证。
8. **`@root struct` 与辅助结构体组织铁律**：
   - 当原 Zig 文件表示根结构体时，文件级的辅助结构体/枚举（如 `Route`, `Type`, `EncodedPattern`, `TinyLog` 等）必须声明在模块顶层（位于 `@root struct Name { ... }` 外部），使 `cpc` 能进行完整的 CP 语法 lowering（`static fn` -> `pub fn`, `: ReturnType` -> `ReturnType` 等）；
   - `@root struct Name { ... }` 仅包裹根结构体自身的字段与自身的方法（如 `initEmpty`, `deinit`, `scan`, `insert`），确保方法体内的 `_` 与 `*_` 能正确绑定到根结构体实例接收者；
   - 严禁将二级子结构体嵌套在 `@root struct` 字段声明中间（会被 codegen 跳过或解析为 field 报错）。
