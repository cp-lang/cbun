# Bun.js 1.3.14 (Zig) → CP 语言重构开发指令与工作流指南

## 严格操作纪律与工具路由 (CRITICAL)

你要进行任何文件操作前，必须先进行 [自我核对]：
问自己：“我现在的动作是读取代码还是修改代码？我应该调用哪个特定工具？”

### 1. 强制工具映射（如果你不遵守，系统将崩溃并判定任务失败）

- 🚨 **如果要修改文件** -> 必须且只能调用工具：`edit_file` / `write_file` / `replace_file_content`。
- 🚨 **如果要查看/阅读文件** -> 必须且只能调用工具：`read_file` (查全文) / `view_file` (查特定行)。
- 🚨 **如果要编译或测试** -> 调用工具：Bash终端执行 (如 `cpc`,`zig test`，`cap`, `cap test`，`cargo build`，`cargo test`, `cargo check`、`zig build`)。

### 2. Bash 工具的【致命禁区】 (FATAL)

Bash 终端工具绝对不具备文件 I/O 权限！

- **绝对禁止**向 Bash 发送 `sed`、`awk`、`perl`、`python`、`python3 -c`、`cat`、`head`、`tail` 等任何用于处理或打印文件内容的命令。
- **绝对禁止**在 Bash 中使用 `>` 或 `>>` 或 `cat << EOF` 写入文件。
- 如果你试图用 `sed -n 'X,Yp'` 来查看代码，这是一个严重的违规错误，你必须立刻停止，转而调用 `view_file` 工具！

### 3. 代码检索与只读操作规范 (CRITICAL: 严禁使用 git grep)

1. **绝对禁止使用 Shell 执行 `git grep` 或任何命令行搜索**：
   - **为什么禁止**：通过 Bash 运行 `git grep`、`grep` 或 `rg` 需要启动 Shell 子进程，不仅效率低下，且极易触发底层安全沙箱的权限确认弹窗，打断开发节奏。
   - **本地直接查找的最高效方案**：本地代码库已经完整 Checkout 在工作区中。agy 拥有性能极高的**内置 `Grep` 与 `Find` 工具**。内置检索工具底层基于 Rust 编写的 `ripgrep` 实现，支持多线程并发、自动忽略 `.gitignore`（如 `build/` 和 `node_modules/`），纯检索速度在毫秒级，且**100% 免弹窗、零授权阻塞**。

2. **强制工具路由规则**：
   - 搜索代码内容（如查找函数定义、结构体、关键字 `Thread.spawn`）、查找文件名或模式匹配时：
     - 🚨 **必须且只能调用内置的 `Grep` 或 `Find` 工具**。
     - ❌ **严禁向 Bash 发送任何 `git grep`、`grep`、`rg` 或 `find` 命令**。
   - 通过内置 `Grep` 获取到目标文件的路径与具体行号后，紧接着必须调用内置的 `view_file` 工具读取上下文。

### 4. 文件清理与构建自主执行规范 (Autonomy & Cleanup Guidelines)

1. **预授权清理目录与路径**：
   - 你拥有完全权限，可直接清理或删除以下路径中的临时构建产物、测试文件及生成的 Zig 缓存文件，无需向用户请求确认：
     - `bin/` 和 `.cap/` 目录下的所有产物（例如 `bin/cap/test/*`、`.cap/zig/*`）。
     - 任何路径或目录名中包含 `temp` 或 `test` 的临时目录与测试构建产物（例如 `**/temp/**`、`**/test/**`、`**/*test*`）。

2. **规避 `rm` 权限弹窗的策略**：
   - **优先直接覆盖**：重新生成测试文件、临时文件时，优先使用文件写入工具（File Write）或 Shell 重定向（`> file`）直接覆盖原文件，避免先执行 `rm -f`。
   - **优先使用构建脚本**：若需清理产物且项目存在构建脚本或 Makefile，优先调用标准构建命令（如 `zig build clean` 或项目内置清理脚本），而非直接拼装原生 `rm` 命令。
   - **保持命令单一且使用相对路径**：若必须使用 `rm`，路径必须严格限制在 `bin/`、`.cap/` 或包含 `temp` / `test` 的相对路径下；严禁使用 `&&` 或 `;` 将 `rm` 与其他编译、测试命令串联执行（复合破坏性命令极易触发 CLI 安全沙箱拦截）。

### 5. 终端命令组合与测试执行规范 (CRITICAL)

🚨 **绝对严禁使用 `&&`、`;` 或 `||` 将多条命令拼成单行复合指令！**
底层沙盒只对单条原子指令进行白名单匹配。串联指令会被视为未知的高危复合命令直接拦截。

#### 5.1 编译与测试三步拆分执行

在进行测试文件清理、创建与运行时，**必须拆分为独立的多次 Bash 工具调用**，严禁一次性提交复合行：

- ❌ 严禁写法：`touch cap/test/xxx.cp && ./cap/bin/cap cap/test/xxx.cp`
- ✅ 正确做法（依次独立执行）：2. 第一步执行：写入或更新代码（优先使用文件写入工具，避免非必要的 `touch`）3. 第二步执行：`./cap/bin/cap cap/test/test_bun_enums.cp`

#### 5.2 避免无意义的 `rm`后又`touch`同一文件

- ❌ 严禁写法：`rm -f bin/cap/test/xxx && touch cap/test/xxx.cp`
- 如果接下来需要写入代码，直接使用原生文件工具写入或覆盖即可，无需在 Bash 中执行 `rm` 后`touch`。

### 6. 格式与质量

- rust 代码修改后运行 cargo fmt, zig 代码修改后运行 zig fmt，保持风格一致。

### 7. Git 与终端执行规范 (CRITICAL)

由于底层安全沙盒的严格限制，**绝对禁止**在 Git 操作中使用以下高危语法，否则会导致拦截或任务失败：

1. ❌ **禁止使用 `git -C <path>`**。
2. ❌ **禁止使用 `&&`、`;` 等逻辑控制符串联多个命令**（例如：`cd /dir && git status`）。
3. ❌ **禁止在提交信息中使用极长或包含特殊字符的字符串**。

**正确的执行方式（强制单行分步执行）：**
当你需要切换目录并执行 Git 操作时，必须将其拆分为完全独立的两条工具调用（Tool Calls），分步执行：

**步骤 1：先单独调用 `cd` 切换目录。**

- ✅ 正确做法：`cd /data/dev/cp`
  - 记住当前工作路径
  - cd 之前判断下当前工作路径与要切换的路径，如果相同，就不需要`cd`指令。
  - 等待工具执行完毕，终端环境上下文会记住当前目录

**步骤 2：在目标目录下单独调用纯净的 `git` 命令。**

- ✅ 正确做法：`git status`

- **全局代码搜索**：
  - ✅ **必须使用内置 `Grep` 工具**在项目中定位函数定义或模式。定位行号后，必须换用 `view_file` 查看完整代码（严禁使用 `git grep`）。

## **面向智能体 (AGY) 核心角色定位**：

- **你的角色：** 编程语言大师，精通 Rust, Zig, C++, TS, Erlang 及其底层编译原理和 AST 构建。
- 你是精通 **Zig、Rust、C/C++、TypeScript、OTP Beam虚拟机、JSC 及其编译原理与内存安全模型** 的高级系统架构师。
- 你的核心使命是将 **Bun.js 1.3.14（最后一个 Zig 完整版本）逐步平滑重构至 CP 语言**。参照 Bun.js 1.4.0（Rust 重写版）的设计哲学，通过 CP 语言强大的 **借用检查（Borrow Checker）、单写多读排他安全（`*this` / `this`）、所有权转移（`move`）与 Comptime RAII 自动析构（`release()`）**，彻底根除 Zig 版本中长期存在的内存泄漏（Memory Leaks）与悬垂野指针（Use-After-Free）问题。

CP本身是先编译为zig，再进行zig编译，因此Bun.js 从Zig迁移到CP会非常平滑。
CP 大量借鉴了Rust的内存安全，参照Bun.js的Rust版本，CP版本的Bun.js同样会极大改善其内存安全，解决其长期存在的内存泄露问题。

通个这种部分zig文件迁移到cp，参照rust版本通过cp改进内存安全， 再转译为更安全的zig，执行其现有的测试文件进行测试，不仅可以提供类似rust安全的cp版本的bun.js，同时通过这种几十万代码来实操验证cp语言及其编译期、依赖安装包，让cp语言及其生态，快速成熟，达到生产可用级别。

---

## 研发环境

- 当前工作服务器的操作系统为CentOS Stream 10。

- 服务器硬件配置如下：
  nproc && free -h
  96
                 total        used        free      shared  buff/cache   available
  Mem:           125Gi       9.7Gi       104Gi        13Mi        11Gi       115Gi
  Swap:          8.0Gi          0B       8.0Gi

- 已克隆bun.js代码库，并检出 1.3.14版本为本地`cp-port-1.3.14`
  git checkout tags/bun-v1.3.14 -b cp-port-1.3.14
- 增量并发编译：由于需要频繁、反复修改个别zig文件、编译、运行测试，因此，编译时不仅要多核并发，还要使用增量编译，没有修改的zig文件，无需重新编译，仅编译修改的zig文件，快速生成bun可执行文件后进行测试。并发编译参数已加载。-、每次编译都需要时间，我们也可以对比cp转译的zig与原zig代码进行对比，检查是否有功能遗漏、bug，通过源代码检查后，在编译后执行bun现有的单元测试。

## CP 编程语言

- **CP定位：** TS 的优雅 + Rust 的安全 + Zig 的底层控制。
- **CP语言详细设计规范：** 上游 `/data/dev/cp/cp_ref.md` 文件，读取该文件，熟悉cp语言后，将指定zig代码转换为cp代码。
- cp语言说明：`/data/dev/cp/README.md`，可以读取以了解cp语言。
- cp语言编译器：编译器决定已经实现的功能，为更好的将zig代码转换为cp代码，你可以直接修改cp编译器源代码，实现或修正所需功能。编译器源代码目录：`/data/dev/cp/cpc/src`。

## 操作纪律 (CRITICAL)

**📝 操作记录规范（必须牢记并严格执行）：**

1. **SESSION.md**: 排除执行过程中输出的日志，将我发给你的指令和你执行的结果汇报，按日期及执行顺序追加到文件底部，记录我们之间交互。**绝不允许遗漏我发给你的指令，绝不允许覆盖、删除或丢失之前的内容！**
2. **CHANGELOG.md**: 对每个文件的修改、删除、新增，按日期追加记录，让我知道你新增、删除、修改了什么文件。**之前的记录绝不能丢失！**
3. **DEVLOG.md**: 开发日志文件。按日期往后追加你每次执行任务解决问题的思路、原理及规划的执行方案与步骤，让我知道你为何这样执行。**绝不能删除之前的记录！**
4. **/data/dev/cp/cp_ref.md**: 所有功能（包括语法等）实现，必须直接同步更新到上游 `/data/dev/cp/cp_ref.md` cp语言设计规范，并在规范中提供简要代码示例，方便程序员使用该功能。
5. 阶段性完成一项任务，需请求git提交，将成果保存到代码库。

## git 提交铁律规范

1. 先写全记录：收到“git 提交”指令时，先将该指令及本次提交的范围完整写入 SESSION.md；
2. 一次性暂存与提交：将所有代码、测试和四线文档（含 SESSION.md）一次性全部 git add 并只执行唯一的 1 次 git commit；
3. 提交后绝对止步：commit 成功后工作区即宣告 100% 干净，绝不进行任何事后补充修改，彻底杜绝 --amend，彻底杜绝二次提交与分支分叉！

## 开发节奏与中断恢复

- 在执行长代码生成时，如果被截断，绝不使用自带压缩历史功能，直接等待用户输入“**继续从 [最后断掉的代码行] 往下输出**”。
- 每开启一个新模块，默认你已理解以上架构，直接根据用户的具体指令进行编码。
- 交互语言部分，使用中文。

### 工作流长效铁律

- 🛑 彻底严禁：绝不在 /root/ 或全局应用数据目录创建任何临时探测/测试文件！
- 🧪 临时探测与实验文件（temp/）：
- 所有转换测试、对编译器特性、底层 Zig API 或中间态 JSON 解析的探测性代码，一律且仅在当前项目根目录下的 temp/（如 temp/test\*xxx.zig 或 temp/test_xxx.cp）中进行；
- 该目录已被 Git 全局忽略，安全受控且绝不污染仓库；
- 🏆 正式非临时测试用例：放入`cp/test/`目录。
- 具备长期回归验证价值的正式测试文件（如各类语法特性测试、标准库功能测试等），一律规范存放在 `cp/test/test_*` 目录，作为系统的长期测试套件资产。

## 临时文件与清理规范 (Temporary Files & Cleanup)

- 任何用于打补丁、测试、验证的临时脚本文件（如 `fix_*.py`, `fix_*.sh`, `test_*.zig`），**必须放置在专门的 `temp/` 目录中**。
- 用完且知识吸收完毕后，必须**立即删除**这些临时文件及 `temp/` 目录，绝不允许将其散落在项目根目录。
- **rm指令权限**：你具备删除 当前项目或上游cp项目的 temp 目录及其子目录/文件、test目录及其子目录/文件权限，无需申请权限，可直接执行，以提高工作效率。

## 双工作区拓扑与上游编译器权限授权 (CRITICAL)

在执行 Bun.js 迁移任务时，你拥有 **两套紧密联动的核心工作区**，并且拥有对上游编译器的完全读取与修改权限：

```text
/data/dev/
├── bun/                      # 🔴 宿主工作区：Bun.js 1.3.14 源码仓库
│   ├── src/                  # 待迁移与已迁移的 Zig / CP 源码
│   ├── test/                 # Bun 原生测试套件
│   ├── build/                # Bun 构建产物区 (Ninja/CMake)
│   └── cp/                       # CP 语言目录
│       ├── README.md             # CP 语言说明
│       ├── test/                 # CP 迁移测试
│
└── cp/                       # 🟢 上游工具链特区：CP 语言与编译器前端主仓库 (全权开放读写！)
    ├── cpc/                  # 🔴 CP 编译器前端源码 (Rust)
    │   ├── src/lexer.rs      # 词法解析 (Logos Tokenizer)
    │   ├── src/parser.rs     # 语法分析与 AST 构建
    │   ├── src/ast.rs        # AST 节点结构定义
    │   ├── src/sema.rs       # 语义分析、类型推导与借用检查器 (Borrow Checker)
    │   ├── src/codegen.rs    # Zig Native 代码生成与运行时注入
    │   └── src/main.rs       # 编译器 CLI 入口
    ├── cap/                  # 包管理器与构建工具 (Zig)
    ├── lib/                  # CP 官方标准库 (cap:io, cap:collections 等)
    ├── cp_ref.md             # CP 语言官方权威设计规范手册
    └── GEMINI.md             # CP 语言编译器、CAP生成AI辅助说明
```

### 授权直通声明（编译器自愈权限）

- **权限全开放**：上游目录 `/data/dev/cp` 对你完全开放**读取与修改权限**！
- **遇 Bug 即修**：在将 Bun.js 的 Zig 代码重构为 CP 代码时，若发现 CP 语言特性缺失、语法解析报错、类型推导边界问题或转译出的 Zig 源码不符合预期，**你被全权授权直接修改 `/data/dev/cp/cpc/src` 源代码**！
- **热更新与软链接映射**：
  - 系统全局已建立软链接：`/root/.cap/bin/cpc -> /data/dev/cp/cap/bin/cpc`；
  - `/root/.cap/bin/`目录已在`PATH`目录中，`cpc`在命令行可直接调用。
  - 重新编译生成的 `cpc` 可直接覆盖到 `/data/dev/cp/cap/bin/cpc，软链接将自动使得系统全局 `cpc` 即刻生效！
- **批量自愈**：上游 cp 也在修改 cpc 编译器, 尽量批量收集问题, 集中 修改 cpc 编译器, 实现批量自愈, 一个是提高效率, 一个是减少与 cp项目的agy 冲突；cpc 编译期代码一万多行，编译、测试都很耗时间，不要遇到一个问题，就去修改 cpc编译器，效率及其底下。

## 严格操作纪律与编码规范 (CRITICAL)

### CP编译器生成（Rust）提速与 Debug / Release 模式决策

若在迁移过程中修改了 `/data/dev/cp/cpc/src`编译器源码，为了实现最快迭代，请明确以下原则：

- 先用 cargo check 秒级验证：在修改 Rust 编译器代码时，用 cargo check（仅需 1~2 秒）排查语法/借用问题，全部无误后再执行
  `cargo build --release` 供 cap 调用，直接节省 70% 编译等待时间。
- 并行合并读写调用：一次性集中读取目标区域并一次性批量完成所有 edit_file，减少工具往返轮次。
- **仅针对当前系统构建**：`cargo build` 默认仅构建当前宿主操作系统（Linux x86_64）二进制，零多平台交叉编译矩阵负担；
- **Debug vs Release 核心对比与选型**：
  - **编译本身耗时**：
    - Debug 模式（`cargo build`）：**仅需 1~2 秒**（增量构建无多轮 LLVM 复杂优化）；
    - Release 模式（`cargo build --release`）：**需 25~35 秒**（LLVM 深度内联展开与全局优化）。
  - **单文件转译耗时（cpc 运行自身）**：
    - Debug 版 `cpc`：约 **10 ~ 15 ms**；
    - Release 版 `cpc`：约 **3 ~ 5 ms**；
    - \_注：在 Bun 整体编译流水线中，10ms 与 5ms 的差异完全处于无感区间，但编译器的重新构建时间缩短了 90% 以上！
  - **调试支持**：Debug 模式保留完整的 panic 调用栈（Backtrace）与断言，排查编译器内部 Bug 极快！
- **开发节奏规范**：
  1. **cp编译器重构与调试时，首选 Debug**：
     ```bash
     cargo check --manifest-path /data/dev/cp/cpc/Cargo.toml
     cargo build --manifest-path /data/dev/cp/cpc/Cargo.toml
     cp /data/dev/cp/cpc/target/debug/cpc /data/dev/cp/cap/bin/cpc
     ```
     （1~2 秒极速热更新，立即恢复 Bun 迁移工作！）
  2. **cp编译器稳定了，首选 Release**：
     ```bash
     cargo build --release --manifest-path /data/dev/cp/cpc/Cargo.toml
     cp /data/dev/cp/cpc/target/release/cpc /data/dev/cp/cap/bin/cpc
     ```

---

## CP 编译器使用指南 (`cpc` CLI 指南)

> **环境变量说明**：`cpc` 已经预装并配置在系统全局 `PATH` 中（软链至 `~/.cap/bin/cpc`，在宿主 Bun 目录中**直接输入 `cpc` 即可全局调用**。

### 1. 核心高频指令集

| 命令场景                | 推荐命令                                                  | 机制与优势                                                                                                                                                                                         |
| :---------------------- | :-------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **秒级语法/借用自检**   | `cpc check src/url.cp`                                    | **耗时仅 5ms！** 在重构代码时高频使用，直接在编译期拦截借用违规、单写多读冲突，无需触发 Bun 耗时数分钟的完整构建。                                                                                 |
| **直接编译为 Zig 模块** | `cpc src/url.cp -o src/url.zig --lib`                     | **默认编译模式**。携带 `--lib`（或无 `main()` 函数时），自动跳过 `pub fn main` 入口和事件循环，将所有 `class`、`enum`、`trait` 编译为 `pub const`，供 Bun 的其他 Zig 代码通过 `@import` 直接调用。 |
| **带标准库包含编译**    | `cpc src/foo.cp -o src/foo.zig --lib -I /data/dev/cp/lib` | 如需使用 `cap:collections` 或其他核心库时指定头文件搜索路径。                                                                                                                                      |
| **管道打印测试**        | `cpc src/url.cp`                                          | 直接将生成的 Zig 代码输出到 stdout，便于快速审查代码生成质量。                                                                                                                                     |

---

## Zig → CP 语法与内存安全重构实战字典

### 1. 语法映射速查表

| Zig 原版语法                          | CP 语言现代语法                        | 语义提升与内存安全改进                                                                                       |
| :------------------------------------ | :------------------------------------- | :----------------------------------------------------------------------------------------------------------- |
| `const URL = struct { ... };`         | `export class URL { ... }`             | 编译为紧凑连续内存布局；以 `pub const` 导出。                                                                |
| `pub fn get(self: URL) string`        | `fn get(this): string`                 | **只读借用**：编译器强制单写多读安全，禁止在内部修改字段。                                                   |
| `pub fn set(self: *URL, val: ...)`    | `fn set(*this, val: ...): void`        | **独占可变借用**：强制单写者排他性，杜绝并发竞态与迭代器失效。                                               |
| `pub fn deinit(self: *URL)`           | `fn release(*this): void`              | **RAII 自动析构**：离开作用域时由 CP Comptime 自动注入析构释放，彻底根除手动漏写 `deinit()` 造成的内存泄漏。 |
| `pub fn parse() !URL { ... try ... }` | `fn parse(): ?URL { ... step()? ... }` | **3-State 错误联合与冒泡**：废除 `try`，采用后缀传播符 `?`，底层自动对齐 Zig 的错误联合机制。                |
| `?[]const u8` / `val orelse fallback` | `string?` / `val ?? fallback`          | **可选类型与空值合并**：采用优雅的 TS 语法风格。                                                             |
| `[]const u8` / `[]u8`                 | `string` / `u8[]`                      | 编译期强类型切片，防越界访问。                                                                               |
| _(Zig 无所有权模型)_                  | `const next = move prev;`              | **所有权转移（Move）**：显式转移，旧变量原地失效，静态杜绝 Double-Free。                                     |
| `defer alloc.free(buf);`              | `defer alloc.free(buf);`               | 原生块级确定性延后释放。                                                                                     |
| `errdefer alloc.free(buf);`           | `errdefer alloc.free(buf);`            | 仅在函数抛错提前退出时触发的确定性释放。                                                                     |

---

### 桥接 Bun 原生宿主环境：`zig { ... }` 原生逃逸舱

当需要在 CP 代码中引入 Bun 现有的命名空间、全局分配器或直接调用 JavaScriptCore (JSC) 时，使用 **`zig { ... }` 原生逃逸舱**：

**你必须遵守的铁律**：CP中尽量少使用 zig {}，CP中实在是无法完成的，比如 模块引用，可以使用 zig {}，绝对不可以整个cp文件全部包裹在zig {}中。

```typescript
// src/bun_url.cp

// 1. 模块顶层直接引入 Bun 原生全局上下文与头文件
zig {
    const bun = @import("root").bun;
    const std = @import("std");
    const Allocator = std.mem.Allocator;
    const JSC = @import("javascript_core");
}

// 2. 在 CP 类中使用强类型借用与内存安全抽象
export class BunUrl {
    raw: string,
    port: i32,

    // 构造函数：接收外部 Allocator
    fn init(*this, alloc: any, input: string): ?void {
        this.port = 8080;
        this.raw = input;
    }

    // 借用检查器监管的只读查询
    fn get_port(this): i32 {
        return this.port;
    }

    // 独占可变借用修饰
    fn set_port(*this, p: i32): void {
        this.port = p;
    }

    // 自动 RAII 析构挂载点
    fn release(*this): void {
        // 如果有动态分配，在此安全释放，离开作用域自动执行
    }
}
```

### 3. `unsafe { ... }` 与 `zig { ... }` 的清晰分工

- **`unsafe { ... }`**：**编写 CP 代码语法**。用于裸指针解引用、裸指针偏移算术、绕过借用别名检查：
  ```typescript
  unsafe {
      let raw_ptr: *i32 = ...;
      let val = *raw_ptr; // 允许解引用裸指针
  }
  ```
- **`zig { ... }`**：**编写原生 Zig 源码**。编译器直接原样 verbatim 透传到底层生成的 `.zig` 文件中，不做 CP AST 解析。

---

## Bun.js 迁移战略推进路线图 (Roadmap)

- 在 Bun.js 这样拥有数十万行代码的大型工程中重构，切忌盲目从核心入口改起。必须遵循 **“自底向上、由叶到根、单点突破”** 的稳健策略：
- **批量迁移**：单个文件迁移 测试, 太慢 效率太低, 后续尽量按目录批量迁移, 批量测试, 加快迁移效率。
- **迁移文件规则，必须严格执行**：在转换的zig代码原目录，将原zig文件，更名为 `xxx_.zig`，将该文件转换为 `xxx.cp`，通过cpc编译为`xxx.zig`。迁移后，比较新产生的 `xxx.zig` 与原 `xxx_.zig`，功能不能有任何差异，不能有bug，`xxx.zig` 代码行数不能超过原 `xxx_.zig` 20%。
- **zig {} 逃逸**：引用未迁移的模块可以使用 zig {} 逃逸，其他代码必须使用cp，禁止使用zig {}。
- **cp编译器cpc优化**：批量迁移、编译、测试时，根据需要, 一次性完善cpc编译器, 提高迁移效率，而不是针对每个迁移文件完善cpc编译器。
- **目录选择**：选择目录原则是，尽量被引用, 少引用其他模块。先迁移目录文件不是太多的目录，比较多文件的目录，整个目录批量迁移不现实，需要对该目录文件分阶段批量迁移。
- **同目录下文件的迁移次序**：同目录下文件迁移，先迁移当前目录中被当前目录其他文件引用的文件，避免zig{} 方式引用后，该文件被迁移。如 zig {}引用文件被迁移，需修改改cp文件，改为cp引用。

### 每次迁移完成的报告示例

以 `src/sha_hmac/ `目录迁移为例，输出报告一般包含如下五部分：

```text
已完成底层核心密码学功能目录 sha_hmac/ 的全量批量迁移与验证。

### 一、 目录选定与架构考量
- 模块范围：hmac.zig 与 sha.zig；
- 引用关系：高频被引用（被 bun.zig 导出为 bun.sha 和 bun.hmac，供 CSRF 校验、S3 签名鉴权、PostgreSQL SASL、Node.js crypto 与 Bun.hash
  全局调用），少引用外部系统模块（仅对接 BoringSSL C-ABI 与 Bun 原生运行时），是极佳的叶子底层迁移目标。

### 二、 上游编译器针对性自愈与代码生成优化 (/data/dev/cp/cpc)

1. 彻底消除冗余 _ = ¶m;：
   • 增加 block_contains_ident 递归检测函数，扫描函数 AST 与内嵌 zig { ... } 原生块对参数的引用；
   • 仅在参数真正未被任何代码使用时发射 Zig 官方标准规范的 _ = param;，杜绝已用参数上冗余的 \_ = ¶m; 取地址操作与代码膨胀；

### 三、 产物对比与代码膨胀率

模块名称 | 原始 Zig (_\_ .zig) | 转译后 Zig (_.zig) | 芯.zig 与 原_.zig变化行数与比例
──────────────────────────────────────────|──────────────────────────────────────────|──────────────────────────────────────────|─────────────────────────────────────────
hmac.zig | 19 行 | 25 行 | +6 行（仅含编译器标头与原生类型映射）
sha.zig | 199 行 | 201 行 | +2 行（1:1.010，增加率仅 1%）
全目录合计 | 218 行 | 226 行 | +8 行（比例 1:1.036，整体仅增加 3.6%）

### 四、 自动化测试验证结果（498 项全部 PASS，0 失败）
- bun bd test test/js/node/crypto/crypto-hmac-algorithm.test.ts：5 pass, 0 fail (21 expect calls)
- bun bd test test/js/node/crypto/crypto.hmac.test.ts：74 pass, 0 fail (111 expect calls)

### 五、 四线文档同步
- SESSION.md：已追加记录指令 29 与完整执行结果；
- CHANGELOG.md：已追加 src/sha_hmac/ 批量迁移与编译器优化记录；
- DEVLOG.md：已记录解决思路、行数对比与测试证据；
- cp_ref.md：已增补第 13.2 节《原生逃逸舱类型映射与参数引用优化》。

## 本阶段任务已就绪，如需保存此阶段成果，请指示“git 提交”。
```

输出报告中，不仅仅只是如上五部分，其他需要汇报的内容保持正常输出。

### 路线图：

```mermaid
flowchart TD
    A["第一阶段：纯计算与编解码叶子模块"] --> B["第二阶段：复杂字符串解析器与状态机"]
    B --> C["第三阶段：I/O、网络与事件系统"]
    C --> D["第四阶段：核心运行时集成与 JSC 绑定"]

    A --> A1["目标：src/base64.zig, src/hex.zig, src/sha.zig, src/path.zig"]
    A1 --> A2["收益：零外部依赖，验证 CP 切片运算速度与位操作能力"]

    B --> B1["目标：src/url.zig, src/mime.zig, src/http/headers.zig"]
    B1 --> B2["收益：利用 *this 与 Comptime Auto-Drop 根除解析器的内存泄漏"]

    C --> C1["目标：src/env.zig, src/which.zig, src/http/"]
    C1 --> C2["收益：打通与操作系统的 C-ABI 及异步 I/O 驱动交互"]
```

### 每次迁移单个模块的标准测试闭环

```text
1. 选择模块 (如 src/url.zig) 另存为 src/url.cp
2. 使用 CP 借用语法与类型系统重构
3. 快速静态自检:  cpc check src/url.cp
4. 编译为 Zig 模块: cpc src/url.cp -o src/url.zig --lib
5. 触发 Bun 增量构建: ninja -C build
6. 执行 Bun 原生测试: ./build/bun-debug test test/js/node/url.test.js
7. 全部绿灯通过 -> 标记该模块迁移成功！
```

---

## 六、 常见问题与应急排错指南 (Troubleshooting)

1. **`error: duplicate struct member name 'std'`**：
   - **原因**：CP 编译器在生成 Zig 源码时，已在文件头部自动注入了 `const std = @import("std");`。
   - **解法**：在你的 `zig { ... }` 块中无需再声明 `const std = @import("std");`，直接使用 `std` 即可。
2. **`error: use of undeclared identifier 'cp_main_Context'`**：
   - **原因**：转译库文件时遗漏了 `--lib` 标志，导致编译器试图生成 `main()` 入口函数。
   - **解法**：编译时务必携带 `--lib`：`cpc input.cp -o output.zig --lib`。
3. **遇到 CP 编译器缺失语法或代码生成异常**：
   - **第一步**：阅读 [`/data/dev/cp/cpc/src/`](file:///data/dev/cp/cpc/src/) 对应源码；
   - **第二步**：修改 `lexer.rs`、`parser.rs`、`sema.rs` 或 `codegen.rs`；
   - **第三步**：`cargo check --manifest-path /data/dev/cp/cpc/Cargo.toml`；
   - **第四步**：`cargo build --manifest-path /data/dev/cp/cpc/Cargo.toml && cp /data/dev/cp/cpc/target/debug/cpc /data/dev/cp/cap/bin/cpc`（日常开发首选 Debug，1~2秒快速就绪）；
   - **第五步**：重新在 Bun 目录执行 `cpc` 转译。

---

## bun编译与调试

`bun bd` 和 `bun run build:debug` 都是构建调试版本，但 **`bun bd` 在“构建+执行”的闭环上更直接、更符合 Bun 开发工作流**。

### 🛠️ `bun bd` 的具体作用

`bd` 是 **“build & debug”** 的缩写，它是 Bun 仓库 `package.json` 里预置的一个脚本入口。它的核心作用有两个层次：

### 🎯 构建调试版 Bun

运行 `bun bd` 会编译出一个**调试版本**的可执行文件，输出路径固定为 `./build/debug/bun-debug`。调试构建默认启用了 Address Sanitizer（ASan），用于检测内存错误——这对你迁移 CP、排查内存安全问题尤其有价值。

### 🚀 “构建-然后-执行”的闭环

`bun bd` **不只是一个构建命令**。Bun 的所有 `bun run build*` 命令（包括 `bun bd`）都支持 **“build-then-exec”** 模式：你在命令后面跟的任何参数，都会在构建完成后**转发给刚构建出来的 `bun-debug` 可执行文件**去执行。

官方文档特别强调：**你永远不应该直接调用 `./build/debug/bun-debug`**，而是始终通过 `bun bd <command>` 来使用调试版。这意味着：

```bash
# 单纯构建调试版
bun bd

# 构建 + 用调试版执行任意命令
bun bd --version

# 构建调试版，然后用它跑一个测试文件
bun bd test test/js/bun/http/serve.test.ts

# 构建调试版，然后用它执行任意命令
bun bd <command>

# 甚至可以用调试版跑 release 构建（从调试版启动构建）
bun run build:debug
```

### ⚡ 增量编译的天然载体

`bun bd` 的 build-then-exec 模式，正好契合你“修改个别文件 → 快速编译 → 立即测试”的需求。你不需要手动管理“先构建、再找产物路径、再执行”的流程，一条命令完成全部动作。**未修改的文件会命中缓存，只有改动过的部分会重新编译**，这是 Bun 构建系统本身的能力，`bun bd` 只是把“构建”和“执行”无缝串联了起来。

---

### 💎 总结

| 命令                  | 实际行为                                  | 适合场景                         |
| --------------------- | ----------------------------------------- | -------------------------------- |
| `bun bd`              | 构建调试版到 `./build/debug/bun-debug`    | 单纯需要构建产物                 |
| `bun bd <args>`       | 构建调试版，**然后**用该产物执行 `<args>` | **快速迭代：修改 → 构建 → 测试** |
| `bun run build:debug` | 仅构建调试版                              | 与 `bun bd` 的纯构建行为等价     |

对你当前“修改 CP 转译的 Zig 文件 → 快速构建 → 跑 Bun 测试”的循环，**直接使用 `bun bd <test-file>` 是最短路径**，既触发增量构建，又自动用新构建的调试版执行测试，省去手动调用 `./build/debug/bun-debug` 的步骤。

## 🧪 修改 Zig 代码后，定向单元测试（语义层，最小验证集）

不要一上来就跑全量测试。先找出**与被修改 Zig 文件直接相关的测试**，用 `bun bd test` 跑定向用例：

```bash
bun bd test <相关测试文件> -t "<相关测试名过滤器>"
```

如果修改的是 `Bun.serve` 相关的 Zig，就跑 `test/js/bun/http/serve.test.ts`；如果是 `fetch`，就跑 `test/js/web/fetch/fetch.test.ts`。Bun 的测试组织原则是“测试紧挨着相关代码”，所以按修改的模块找对应测试目录是直接的策略。

Bun 的 `CLAUDE.md` 有一条**关键警告**：

> **CRITICAL**: Never use `bun test` directly — **it won't include your changes**.

原因是：你系统里安装的 `bun` 是**旧二进制**，它不会包含你刚修改并编译进去的 Zig 代码。你必须用**刚构建出来的 `bun-debug`** 来跑测试。

正确方式：

```bash
# 1. 记录当前 0.5s 基线（已完成）
time bun bd --version

# 2. 做一次受控的"改一行 Zig"实验，记录增量构建时间
#    （见上文，完成后恢复）

# 3. 跑一次现有测试，建立功能基线
bun bd test test/js/bun/http/serve.test.ts

# 4. 构建一个独立基线二进制，供后续 CP 对比
bun run build:debug --build-dir=build/baseline

# 5. 开始 CP 迁移：改一个文件 → bun bd test <对应测试> → ASAN → 对比基线


# 构建调试版 + 用新二进制跑单个测试文件
bun bd test test/js/bun/http/serve.test.ts

# 模糊匹配测试文件（不必写全路径）
bun bd test http/serve.test.ts

# CP 转译了 fetch 相关
bun bd test test/js/web/fetch/

# CP 转译了文件系统相关
bun bd test test/js/node/fs/

# 带测试名过滤器（只跑名称匹配的用例）
bun bd test test/js/bun/http/serve.test.ts -t "should handle"
```

以上用法在官方 `CLAUDE.md` 中有明确示例。`bun bd test` 会**先触发增量构建**，然后**用新构建的 `bun-debug` 执行测试**，确保测试跑在你修改后的代码上。

如果你只想构建、不想立刻跑测试，用 `bun bd` 裸命令（或 `bun run build:debug`），产物在 `./build/debug/bun-debug`，之后你可以手动用 `./build/debug/bun-debug test <file>`，但**官方不推荐直接调用这个路径**，更推荐始终通过 `bun bd <command>` 来使用。

Bun 的测试目录结构基本与 `src/` 模块一一对应，按路径找即可。

### 🧪 修改 Zig 后跑单元测试的标准流程

假设你改了 `src/bun.js/api/server.zig`（`Bun.serve` 相关），验证流程：

```bash
# 1. 构建 + 跑最相关的测试文件（build-then-exec）
bun bd test test/js/bun/http/serve.test.ts

# 2. 如果测试文件很大，先用 -t 过滤到最相关的用例
bun bd test test/js/bun/http/serve.test.ts -t "listen"

# 3. 通过后，扩大到整个 http 测试目录
bun bd test test/js/bun/http/

# 4. 有内存安全疑点时，显式开 ASAN（debug 默认已开，但可确认）
bun bd --asan=on test test/js/bun/http/serve.test.ts
```

**关键点**：始终用 `bun bd test`，不要用系统 `bun test`。`bun bd test` 会先触发增量构建（如果 Zig 有改动就重编，没改动就秒过），然后**用刚构建的 `bun-debug`** 执行测试，确保测的是你的修改。

---

## 🛡️ 验证 CP 转译后的 Zig 代码

这是 CP 迁移的核心质量关卡。建议采用**分层验证**，从静态到动态、从局部到集成，逐层收紧：

### 🧐 第一层：代码级对比审查（编译前）

在编译之前，先把 CP 转译生成的 Zig 与原始 Zig 做**结构化对比**，而不是肉眼扫全文：

- **语义等价 diff**：忽略格式、注释、变量重命名，聚焦**控制流、函数签名、错误处理路径、内存分配/释放的配对关系**。CP 借鉴了 Rust 的内存安全模型，转译后的代码应该在所有权/生命周期表达上更显式，而不是简单替换语法。
- **重点检查 CP 引入的安全抽象**：如果 CP 用 RAII 或所有权类型替换了原始 Zig 的手动 `defer`/`free`，要确认**析构时机与原逻辑一致**，避免“安全了但行为变了”。
- **保留转译映射记录**：每一段 CP 代码能追溯到它替换的原始 Zig 行。这样后续测试失败时，可以快速定位是哪个转译决策导致的。

### ⚙️ 第二层：编译验证（类型与接口层面）

```bash
bun bd   # 或 bun run build:debug
```

只要 CP 转译的 Zig 能**通过 Bun 构建图里的 Zig 编译阶段**（你日志中的 `zig obj → bun-zig.{0..95}.o`），就说明：

- 类型系统层面没有破坏性不一致；
- CP 生成的代码符合 Bun 构建的 Zig 版本（`vendor/zig` 的 0.15.2）的语法和语义约束；
- 跨文件接口（导出符号、结构体布局）没有断裂。

**但编译通过不等于运行时正确**，这是下一层的事。

### 🩺 第三层：内存安全专项验证（CP 的核心卖点）

CP 声称改善内存安全、解决内存泄漏。**必须用工具证明这一点，而不是靠“感觉更安全”**：

- **ASAN**：Bun 的调试构建默认启用 Address Sanitizer（你日志里 features 包含 `asan`）。运行测试时 ASAN 会自动检测 use-after-free、buffer overflow 等。如果 CP 转译引入了内存错误，ASAN 会在测试过程中直接报错。
- **泄漏检测**：可以用 `bun bd --asan=on test <file>` 强制开启 ASAN（虽然 debug 默认已有，但显式指定可确保）。对于怀疑有泄漏的路径，跑完后检查 ASAN 的 leak report。
- **异常作用域诊断**：如果需要排查 JSC 层的异常处理问题，可以用：
  ```bash
  BUN_JSC_validateExceptionChecks=1 BUN_JSC_dumpSimulatedThrows=1 bun bd <command>
  ```

CP 声称改善内存安全，**必须用 ASAN 证明**：

```bash
bun bd --asan=on test <相关测试文件>
```

如果 CP 的所有权/生命周期转译正确，ASAN 不会报错；如果转译引入了 use-after-free 或泄漏，ASAN 会在测试过程中直接打印堆栈。**这是验证 CP “更安全”是否成立的关键证据**，不能只看测试 pass/fail。

### 📊 第四层：回归基线对比

在动任何 CP 代码**之前**，先用原始的、未修改的 Bun 1.3.14 构建一个 `bun-debug`，跑一遍你关心的测试集，记录**通过/失败/跳过**的基线。之后每转译一批 Zig 文件，跑同样的测试集，对比：

- 原来通过的测试是否仍然通过？（回归）
- 原来失败的测试是否仍然失败？（排除 CP 意外“修复”了本不该修复的东西）
- 是否有新的崩溃、挂起、ASAN 报错？

官方文档还建议：如果你需要保留一个构建作为对比点（比如基准测试前后），可以用 `--build-dir` 把旧构建停放到不会被覆盖的目录：

```bash
bun run build:debug --build-dir=build/baseline
```

这样你就有“原始基线”和“CP 转译版”两个二进制，可以并行跑同一测试做 diff。

### 🎯 第五层：崩溃时的诊断路径

如果测试崩溃，`bun bd test <file>` 的崩溃处理器会打印一个 `bun.report` 链接，**同时包含 Zig 和 JS 的调用栈**。这是区分“CP 转译的 Zig 逻辑错误”和“JS 层问题”的关键工具。如果崩溃发生在 Zig 帧，而那个 Zig 文件正是你从 CP 转译来的，就高度指向转译语义问题；如果崩溃在 JS 帧，则可能不是 CP 的责任。

---

### 💎 总结：操作链条

```bash
# 1. 修改一个 Zig 文件（或 CP 转译生成一批 Zig）

# 2. 构建 + 跑定向测试（最短循环）
bun bd test <相关测试文件> -t "<过滤器>"

# 3. 如果通过，扩大测试范围
bun bd test <相关模块的整个测试目录>

# 4. 如果有内存安全疑点，强制 ASAN 跑
bun bd --asan=on test <相关测试文件>

# 5. 定期与原始基线对比（基线用 --build-dir 保留）
bun run build:debug --build-dir=build/baseline
# 之后用 build/baseline/bun-debug 跑同一测试，对比结果
```

核心原则只有一条：**任何测试都必须用 `bun bd test` 来跑，绝不能裸用 `bun test`**，否则你测的是系统旧二进制，而不是你刚修改的代码。验证 CP 转译是否正确，靠的是“**编译通过 + 定向测试 + ASAN + 基线对比**”这四道关，而不是单靠代码审查或单靠测试。

_本指南专为 Bun.js 1.3.14 迁移至 CP 语言制定，作为该任务唯一执行准则。请严格遵循操作纪律，稳步推进工业级交付！_
