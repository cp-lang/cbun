# Bun.js 1.3.14 (Zig) → CP 语言重构开发指令与工作流指南

## 一、严格操作纪律与工具路由 (CRITICAL)

你要进行任何文件操作前，必须先进行 [自我核对]：
问自己：“我现在的动作是读取代码还是修改代码？我应该调用哪个特定工具？”

### 1. 强制工具映射（如果你不遵守，系统将崩溃并判定任务失败）

- 🚨 **如果要修改文件** -> 必须且只能调用工具：`edit_file` / `write_file` / `replace_file_content`。
- 🚨 **如果要查看/阅读文件** -> 必须且只能调用工具：`read_file` (查全文或按行读取)。如读取文件超过`read_file`允许主智能体在需要全览 800 行以上代码时，使用 run_command 执行针对大文件的结构化提取（如只读 grep/rg 提取类与方法大纲），再针对性读完。
- 🚨 **如果要检索代码或查找符号** -> 允许调用 `run_command` 执行极速只读检索（如 `rg -n`，详见第 3 节）。
- 🚨 **如果要编译、测试或运行脚本** -> 调用工具：Bash终端执行 (如 `bash scripts/sync_cpc.sh`、`cpc`、`zig test`、`cargo check` 等)。

### 2. Bash 终端命令与管道重定向规范 (REVISED)

- **解除终端重定向限制**：明确允许在 Bash 终端中使用输出重定向（`>`、`>>`、`2>&1`）与管道传递（`|`），支持将测试日志、构建信息或临时探测产物输出至临时文件进行分析。
- **源码维护原则**：核心代码库源文件的新建与精确修改仍推荐使用 `write_file` / `edit_file` 或 `temp/` 下的受控批处理脚本，保证变更的精准性与审查透明度。

### 3. 代码检索与只读操作规范 (CRITICAL)

1. **允许高效代码搜索与管道传递**：
   - 鉴于当前 CLI 运行时未挂载独立的 GUI 检索挂件，**明确允许通过 `run_command` 执行检索与管道指令**：
     - ✅ **优先使用 `rg` (ripgrep)**：`rg -n "keyword" <path>`。在 96 核高并发支持下，多线程毫秒级完成百万行代码检索。
     - ✅ **按名称查找文件**：`find <dir> -name "*.cp"` 或 `find <dir> -name "*.zig"`。
     - ✅ **管道与重定向放行**：允许使用 `| head`、`| grep`、`> temp/log.txt` 等管道与重定向进行结果筛选与持久化分析。
   - 获取到目标文件的行号后，紧接着调用 `read_file`（指定 StartLine/EndLine）读取上下文代码。
2. 解除之前相关读取文件行数的限制。
3. “针对 > 800 行的大文件，推荐优先使用 rg -n "^(pub\s+)?(fn|class|export|const)\s+" <path> 秒级提取全局结构与方法大纲，再精准使用 read_file 查阅目标实现，消除跨 800行多次分段读取的往返开销。”

### 4. 文件清理与构建自主执行规范 (Autonomy & Cleanup Guidelines)

1. **预授权清理目录与路径**：
   - 你拥有完全权限，可直接清理或删除以下路径中的临时构建产物、测试文件及生成的 Zig 缓存文件，无需向用户请求确认：
     - `bin/` 和 `.cap/` 目录下的所有产物（例如 `bin/cap/test/*`、`.cap/zig/*`）。
     - 任何路径或目录名中包含 `temp` 或 `test` 的临时目录与测试构建产物（例如 `**/temp/**`、`**/test/**`、`**/*test*`）。

2. **规避权限弹窗的策略**：
   - **优先直接覆盖**：重新生成测试文件、临时文件时，优先使用文件写入工具（`write_file`）直接覆盖原文件，避免先执行 `rm -f`。
   - **保持命令单一且使用相对路径**：若必须使用 `rm`，路径必须严格限制在 `bin/`、`.cap/` 或包含 `temp` / `test` 的相对路径下。

### 5. 终端命令与脚本执行规范 (CRITICAL)

1. **组合指令与权限放行**：
   - 当前环境已通过 `--dangerously-skip-permissions` 启动，底层已解除沙箱弹窗拦截，允许根据需要使用 `&&` 串联逻辑相关命令（如 `cd /path && git status`）。
2. **高频流程必须脚本化（消除交互延迟与节省 Token）**：
   - 严禁在对话中反复长篇输出 `cargo check --manifest-path ... && cargo build ... && cp ...`，不仅浪费海量 Token，且容易因长路径拼写导致模型幻觉。
   - 遇到构建和同步 `cpc` 编译器，必须直接调用预置原子脚本：
     ```bash
     bash scripts/sync_cpc.sh
     ```
   - 脚本内部自带 `set -e` 安全守卫，只要一条原子命令，秒级完成编译与生效！

### 6. 格式与质量

- rust 代码修改后运行 cargo fmt, zig 代码修改后运行 zig fmt，保持风格一致。

### 7. Git 与终端执行规范 (CRITICAL)

1. **目录切换与 Git 状态**：
   - 允许使用 `cd <dir> && git status` 或分步执行。切换前判断当前工作路径与目标路径是否相同。
2. **提交信息规范**：
   - 提交信息请保持简洁清晰，避免包含极长或特殊控制字符。

### 8. 编码与分析过程中的防护策略（必须严格执行）

- 术语中性化：在撰写 SESSION.md、DEVLOG.md 以及对话汇报时，使用更中性的工程术语：
- 用「异常边界防护 / 畸形报文校验」替代包含 attack / exploit 的词汇；
- 用「资源耗尽防护」替代 bomb / flood；
- 用「终止生命周期 / 释放进程」替代敏感的进程控制用词。
- 避免大段回显测试载荷：阅读文件时使用精确行号范围（StartLine/EndLine），避免一次性将测试夹具中的 Base64 证书、伪造报文等敏感字符串打印到对话上下文中。
- 脱敏日志输出：遇到包含此类词汇的单元测试报错时，截取关键行与类型错误，过滤掉测试文件中的攻击模拟描述。

## 二、**面向智能体 (AGY) 核心角色定位**：

- **你的角色：** 编程语言大师，精通 Rust, Zig, C++, TS, Erlang 及其底层编译原理和 AST 构建。
- 你是精通 **Zig、Rust、C/C++、TypeScript、OTP Beam虚拟机、JSC 及其编译原理与内存安全模型** 的高级系统架构师。
- 你的核心使命是将 **Bun.js 1.3.14（最后一个 Zig 完整版本）逐步平滑重构至 CP 语言**。参照 Bun.js 1.4.0（Rust 重写版）的设计哲学，通过 CP 语言强大的 **借用检查（Borrow Checker）、单写多读排他安全（`*this` / `this`）、所有权转移（`move`）与 Comptime RAII 自动析构（`release()`）**，彻底根除 Zig 版本中长期存在的内存泄漏（Memory Leaks）与悬垂野指针（Use-After-Free）问题。

CP本身是先编译为zig，再进行zig编译，因此Bun.js 从Zig迁移到CP会非常平滑。
CP 大量借鉴了Rust的内存安全，参照Bun.js的Rust版本，CP版本的Bun.js同样会极大改善其内存安全，解决其长期存在的内存泄露问题。

通个这种部分zig文件迁移到cp，参照rust版本通过cp改进内存安全， 再转译为更安全的zig，执行其现有的测试文件进行测试，不仅可以提供类似rust安全的cp版本的bun.js，同时通过这种几十万代码来实操验证cp语言及其编译期、依赖安装包，让cp语言及其生态，快速成熟，达到生产可用级别。

---

## 三、研发环境

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

## 四、CP 编程语言

- **CP定位：** TS 的优雅 + Rust 的安全 + Zig 的底层控制。
- **CP语言详细设计规范：** 上游 `/data/dev/cp/cp_ref.md` 文件，读取该文件，熟悉cp语言后，将指定zig代码转换为cp代码。
- cp语言说明：`/data/dev/cp/README.md`，可以读取以了解cp语言。
- cp语言编译器：编译器决定已经实现的功能，为更好的将zig代码转换为cp代码，你可以直接修改cp编译器源代码，实现或修正所需功能。编译器源代码目录：`/data/dev/cp/cpc/src`。

## 五、操作纪律 (CRITICAL)

### 1. **📝 操作记录规范（必须牢记并严格执行）：**

1. **SESSION.md**: 排除执行过程中输出的日志，将我发给你的指令和你执行的结果汇报，按日期及执行顺序追加到文件底部，记录我们之间交互。**绝不允许遗漏我发给你的指令，绝不允许覆盖、删除或丢失之前的内容！**
2. **CHANGELOG.md**: 对每个文件的修改、删除、新增，按日期追加记录，让我知道你新增、删除、修改了什么文件。**之前的记录绝不能丢失！**
3. **DEVLOG.md**: 开发日志文件。按日期往后追加你每次执行任务解决问题的思路、原理及规划的执行方案与步骤，让我知道你为何这样执行。**绝不能删除之前的记录！**
4. **语言规范手册（权威准则）**：`/data/dev/cp/cp_ref.md` 作为cp语言设计唯一最高准则保持全量更新，所有新增或完善的语言功能（语法特性、标准库契约、类型系统等），在阶段里程碑时必须同步写入，并提供简要代码示例。
5. **阶段里程碑汇总记录**：以上四线文档记录，不要每个小文件即时记，按模块目录闭环记（例如每完成一个完整子目录如 src/router/，或消除一组特定报错时，阶段性汇总统一集中记录一次四线文档，你的火力 90% 集中于代码编译攻坚上。

### 2. 四线文档冷热分离与分类驱动执行规范（核心提速铁律）

为彻底解决大型文档随着项目演进膨胀、导致模型读取与追加时 Token 消耗与处理延迟过高的问题，四线文档全面推行**冷热数据物理分离**与**任务分类驱动**策略：

#### 2.1 冷热分离架构（Hot/Cold Partitioning）

1. **热日志文件（当日活动数据）**：
   - `SESSION.md`、`CHANGELOG.md`、`DEVLOG.md` 保持极简轻量，**仅记录当日的开发执行、变更与指令记录**；
   - 当日开始时文件体积小、检索与追加均为毫秒级，彻底消除长文往返与海量 Token 开销。
2. **冷归档文件（全量历史数据）**：
   - 超过当日的历史内容，统一自动滚动迁移归档至对应的历史文件：`SESSION_H.md`、`CHANGELOG_H.md`、`DEVLOG_H.md`；
   - 历史文件永久保存，严禁丢失任何历史记录，随时供深度回溯与查阅。

3. **加速完成迁移任务规则**：

- 在案目录迁移时，先对整个目录的zig文件不编译测试情况下迁移，完成整个目录zig文件迁移到cp后，你需尽快完成所有cp代码的编译与测试。
- cp文件编译报错, 先记录, 全部cp文件处理一遍后, 再根据出错记录多少, 从少到多, 分派多个子智能体根据编译报错提示，修改cp文件。
- 如需要修改cpc 实现自愈, 需收集所有cpc需实现功能,按GEMINI.md 中的规则, 启动多个子智能体,在各自工作区, 并行实现后合并到 cpc中。
- 如此反复,直至所有cp文件都能成功编译为zig,并且编译产生的zig与原zig代码,功能完全一致,代码行数不得超过原始zig的20%。
- 切记要顾全大局, 完成整体目标, 不要卡死在一个子任务中，导致任务失败。
- 按目录或文件分组,同时启动多个子智能体并行工作, 你作为主智能体,做好调度/分派/进度检查即可, 这样提高效率,减少任务时间。多智能体并行工作token消耗成倍，需注意按后续节省token章节的场景执行， 节省token消耗，

## 六、git 提交铁律规范

1. **阶段里程碑git提交**：阶段性成果，需自动完成git提交，将成果保存到代码库。
2. 先写全记录：收到“git 提交”指令时，先将该指令及本次提交的范围完整写入 SESSION.md；
3. 一次性暂存与提交：将所有代码、测试和四线文档（含 SESSION.md）一次性全部 git add 并只执行唯一的 1 次 git commit；
4. 提交后绝对止步：commit 成功后工作区即宣告 100% 干净，绝不进行任何事后补充修改，彻底杜绝 --amend，彻底杜绝二次提交与分支分叉！

## 七、开发节奏与中断恢复

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

## 八、临时文件与清理规范 (Temporary Files & Cleanup)

- 任何用于打补丁、测试、验证的临时脚本文件（如 `fix_*.py`, `fix_*.sh`, `test_*.zig`），**必须放置在专门的 `temp/` 目录中**。
- 用完且知识吸收完毕后，必须**立即删除**这些临时文件及 `temp/` 目录，绝不允许将其散落在项目根目录。
- **rm指令权限**：你具备删除 当前项目或上游cp项目的 temp 目录及其子目录/文件、test目录及其子目录/文件权限，无需申请权限，可直接执行，以提高工作效率。

## 九、双工作区拓扑与上游编译器权限授权 (CRITICAL)

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
- **批量自愈**：；cpc 编译期代码一万多行，编译、测试都很耗时间，不要遇到一个问题，就去修改 cpc编译器，效率及其底下。先将所有需要迁移的文件，按引用依赖次序, 全部改写为cp, 能不用zig{}, 坚决不用zig{}, 最后再统一编译一遍,根据编译错误等, 批量收集所有问题, 集中修改上游 cpc 编译器, 实现批量自愈, 提高效率, 减少与 cp项目的agy 冲突。修改上游cpc编译期需谨慎, 可以添加、完善功能，修正bug, 不要破坏 cpc 已有功能。

## 十、严格操作纪律与编码规范 (CRITICAL)

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

### 3. cpc 编译器必须“AI-First 智能诊断提示

- **错误提示AI友好**：cpc check 和编译出错提示，必须对ai 友好，不仅带编译出错信息，还应提供修正方法, 方便 agy 等智能体快速识别错误，进行修正, 而不是一筹莫展, 需要查看 cpc 源代码来解决。

- 编译器报错将从冷冰冰的 Syntax Error 升级为三段式可自愈诊断（参考 Rustc 与 TypeScript Compiler）：

```text
  error[E0105]: ambiguous condition in control flow statement
    --> src/boringssl/boringssl.cp:58:24
     |
  58 |     if addr_str.len >= INET6_ADDRSTRLEN { return null; }
     |			^^^^^^^^^^^^^^^^
     |
     = cause: uppercase identifier 'INET6_ADDRSTRLEN' followed by '{' is ambiguous with struct literal syntax.
     = help: in CP, control flow conditions should be enclosed in parentheses.
     = suggestion: wrap the condition in parentheses:
     |
  58 |     if (addr_str.len >= INET6_ADDRSTRLEN) { return null; }
     |        +                                +
```

#### 典型场景的智能自愈提示规则：

1. 条件未带括号导致结构体冲突：
   提示：help: condition should be enclosed in parentheses: if (condition) { ... }；
2. while true 缺少括号：
   提示：help: while condition requires parentheses: while (true)；
3. 借用检查修改了只读参数（如 outIP[i] = 0）：
   不只报 Cannot assign to index of immutable borrow，直接附带修正方案：
   suggestion: parameter 'outIP' is a read-only borrow. Add '*' to declare it as mutable borrow: '*outIP: OutIPBuf'。

工程价值：AI Agent 看到带有 suggestion 的报错后，耗时 0 秒，单轮对话直接采纳建议自动修复！

## 十一、 多智能体并行工作流与 `cpc_dev/agent_*` 构建规范（核心）

### 1. 多智能体并行开发cpc机制（`cpc_dev_*` vs `cpc`）

cpc 源代码几万行，修改、编译、测试非常耗时，需要修改、测试cpc 源代码实现自愈功能时，需将多个功能分派给多个子智能体在各自拷贝的沙箱目录中修改、测试，加快cpc研发速度。

为避免多个智能体相互覆盖代码或阻塞等待，本项目采用 **“多缓冲隔离目录（`cp/cpc_dev/agent_*`）+ 乐观锁校验合并（CAS）”** 模式：

- 子智能体创建自己的cpc的拷贝目录， 如 `cpc_dev/agent_1`、`cpc_dev/agent_2`。
- **子智能体开发与调试一律在自己的 `cpc_dev/agent_*` 目录中进行**，与正式目录 `cpc/` 保持隔离，待测试全部通过后再按流程合并至 `cpc/`。

### 2. CP 编译器（Rust）提速与 Debug / Release 选型及错误提示

- **先用 `cargo check` 秒级验证**：修改 `cpc_dev_*/src` 代码时，先运行 `cargo check`（仅需 1~2 秒）排查语法与借用问题。
- **并行合并读写调用**：集中读取目标区域并一次性批量完成 `edit_file`，减少工具往返轮次。
- **Debug 与 Release 对比选型**：
- **Debug 模式（`cargo build`）**：编译仅需 **1~2 秒**，单文件转译耗时约 10~15ms，且保留完整 panic 调用栈（Backtrace）与断言，排查编译器内部问题极快。**日常在 `cpc_dev_*` 中改动与高频跑测时，只构建 Debug 版。**
- **Release 模式（`cargo build --release`）**：编译需 **25~35 秒**（LLVM 深度优化），单文件转译耗时约 3~5ms。**仅在全部测试通过、合并入正式 `cpc/` 目录发布时构建一次 Release 版。**

### 4. 并行开发与合并标准执行步骤（7 步闭环）

1. **使用 `env` 至调试沙盒**：

- `cap` 支持命令行`env CPC_BIN`设置cpc运行目录，来运行cpc， 你也可以直接使用你自己目录的cpc进行测试
- 通过注入 CPC_BIN 运行官方测试用例，其中cpc_dev_2为子智能体拷贝的工作目录：

```bash
   env CPC_BIN=/data/dev/cp/cpc_dev/agent_2/target/debug/cpc ./cap/bin/cap cap/test/test_err_and_move.cp

```

3. **启动前记录状态 & 测试后校验 `cpc/src` 变更**：

- 启动拷贝工作区时，记录 `cpc/src`的状态。
- 在 `cpc_dev_*` 中修改代码（运行 `cargo check --manifest-path cpc_dev_*/Cargo.toml` 与 `cargo build --manifest-path cpc_dev_*/Cargo.toml`）并通过测试后，使用单条 Git 状态指令快速检查正式目录 `cpc/src` 在此期间是否被其他子智能体修改过：

```bash
git status --porcelain cpc/src
```

（或使用 `git diff cpc/src` 检查差异，避免使用复杂的 Shell 管道命令）。

- 3.1 **若 `cpc/src` 无变化**：直接将工作区`cpc_dev_*`代码合并到正式`cpc/src`。
- 3.2 **若 `cpc/src` 有变化**：先用文件读取与编辑工具将 `cpc/src` 中的新增修改同步合并到 `cpc_dev_*/src` 工作区中，重新编译 `cpc_dev_*` 并运行测试，确保兼容无误。
- 重复 3.2，直至`cpc/src` 无变化，按 3.1 合并子工作区代码到正式'cpc/src'。合并后，删除子工作区目录。

**注意**：以上 `cpc_dev/agent_*`的`*`，表示各个子智能体创建的cpc 拷贝目录的数字编号，必须避免不同子智能体同时在一个工作区工作，同时修改同一个代码文件，导致代码混乱。

## 十二、CP 编译器使用指南 (`cpc` CLI 指南)

> **环境变量说明**：`cpc` 已经预装并配置在系统全局 `PATH` 中（软链至 `~/.cap/bin/cpc`，在宿主 Bun 目录中**直接输入 `cpc` 即可全局调用**。

### 1. 核心高频指令集

| 命令场景                | 推荐命令                                                  | 机制与优势                                                                                                                                                                                         |
| :---------------------- | :-------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **秒级语法/借用自检**   | `cpc check src/url.cp`                                    | **耗时仅 5ms！** 在重构代码时高频使用，直接在编译期拦截借用违规、单写多读冲突，无需触发 Bun 耗时数分钟的完整构建。                                                                                 |
| **直接编译为 Zig 模块** | `cpc src/url.cp -o src/url.zig --lib`                     | **默认编译模式**。携带 `--lib`（或无 `main()` 函数时），自动跳过 `pub fn main` 入口和事件循环，将所有 `class`、`enum`、`trait` 编译为 `pub const`，供 Bun 的其他 Zig 代码通过 `@import` 直接调用。 |
| **带标准库包含编译**    | `cpc src/foo.cp -o src/foo.zig --lib -I /data/dev/cp/lib` | 如需使用 `cap:collections` 或其他核心库时指定头文件搜索路径。                                                                                                                                      |
| **管道打印测试**        | `cpc src/url.cp`                                          | 直接将生成的 Zig 代码输出到 stdout，便于快速审查代码生成质量。                                                                                                                                     |

---

## 十三、Zig → CP 语法与内存安全重构实战字典

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

**你必须遵守的铁律**：除需要引用未迁移到cp的模块,使用 zig {},其他全部使用cp改写! 已经cp改写的, 必须使用 cp方式引用, 不得使用 zig{}方式引用, 这是铁律，必须牢记并遵守，不可违反，否则项目失去意义。
**工具类函数**： 在迁移过程中,如果发现一些频繁使用的工具类函数调用, 如 log等, 你可自行先实现这些函数文件的迁移, 减少 zig {} 引用未实现模块。

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

## 十四、Bun.js 迁移战略推进路线图 (Roadmap)

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

## 十五、 常见问题与应急排错指南 (Troubleshooting)

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

## 十六、bun编译与调试

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

## 十七、🧪 修改 Zig 代码后，定向单元测试（语义层，最小验证集）

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

## 十八、🛡️ 验证 CP 转译后的 Zig 代码

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

## cpc 新增加的语法

### 一、 杀手级特性 1：`@c { ... }` 直接内联吸纳 C 头文件（彻底废除手写胶水代码）

#### 1. 痛点场景

Bun.js 底层大量重度依赖 C 库（如 BoringSSL, uSockets, picohttpparser, c-ares, SQLite3, zlib 等）。过去迁移时，必须将 Zig 的 `@cImport` 手动翻译为几十行乃至上百行脆弱的
`extern "c" fn ...` 声明，极易因指针级数、C 结构体 padding 误判导致编译失败或内存越界。

#### 2. 推荐最佳实践（极简 1 行）

在 CP 模块顶层直接使用 `@c { "xxx.h" }` 引入头文件，编译器底层自动解析并导出至统一的 `c` 全局安全命名空间：

```typescript
// 传统手写：需要手写 30+ 行 extern "c" fn
// 现代 CP：直接 1 行吸纳头文件
@c {
    "stdio.h",
    "sqlite3.h",
    "uv.h"
}

pub fn open_database(path: cstr): *any! {
    var db: *any! = null;
    // 直接以 c.xxx 调用 C 符号，零粘合代码，零 ABI 对齐风险
    let rc = c.sqlite3_open(path, &db);
    if (rc == 0) {
        return db;
    }
    return null;
}
```

│ 提示：同一个 @c { ... } 块内支持头文件与自定义 C 声明混合书写，满足定制化垫片函数的需要。
──────

### 二、 杀手级特性 2：极简 3 字母 own 关键字（无别名单写者所有权转移）

#### 1. 痛点场景

在将 Zig 的手动内存分配、Buffer 及跨线程 Task 传递转换到 CP 时，为了表达“交出独占所有权、杜绝并发别名冲突”，过去容易混淆使用 move 或被迫写冗长的约定逻辑。

#### 2. 现代推荐语法（own 关键字）

CP 现已正式确立 own（3 字母）作为一级所有权关键字，完全替代 move unique：

##### A. 函数形参修饰（独占单写者契约）

// 参数冠以 own，强制调用方必须排他转移所有权，确保函数内部拥有绝对的单写者安全
fn process_buffer(own buf: Buffer): void {
buf.transform();
// 离开函数时，由 buf 接管 RAII 释放
}

##### B. 表达式一元移交（与自动析构 Drop Flag 联动）

let my_buf = Buffer.alloc(4096);

// 转移所有权给函数或新变量
process_buffer(own my_buf);

// ❌ 编译期静态精准拦截：error[E0203]: use of moved value 'my_buf'
// 编译器自动生成 Drop Flag，防止当前作用域离开时发生二次释放（Double Free）！
──────

### 三、 杀手级工具：AI-First 机器自愈通道（cpc check --json）

#### 1. 痛点场景

当转换的代码存在语法微调（如缺少圆括号、const 重新赋值等）时，通过分析冷冰冰的文本终端报错往往需要多轮试错。

#### 2. 推荐使用命令

Bun AGY 在检查转换好的 .cp 文件时，请优先使用 --json 参数：

/data/dev/cp/cap/bin/cpc check --json path/to/file.cp

#### 3. Machine-Readable Edits 自动自愈响应

当发生错误时，编译器将直接输出结构化 JSON，内含精确计算好的 edits 数组（字符偏移量与插入替换建议）：

{
"status": "error",
"diagnostics": [
{
"code": "E0101",
"severity": "error",
"message": "CP Syntax Error: 'if' condition requires parentheses.",
"file": "path/to/file.cp",
"line": 15,
"col": 8,
"edits": [
{ "offset": 240, "delete_len": 0, "insert": "(" },
{ "offset": 248, "delete_len": 0, "insert": ")" }
]
}
]
}

│ 自愈建议：Bun AGY 解析此 JSON 后，可直接根据 offset 执行单步文件插入/替换，实现 0 秒毫秒级自愈！检查完全通过时将输出 {"status":"ok","diagnostics":[]}。
──────

### 四、 核心语法注意点（避免踩坑）

1. 控制流括号铁律：if (...)、while (...)、for (...) 必须包裹圆括号，杜绝与结构体实例化大括号产生歧义；
2. 指针解包语法：
   • 海象语法：if (*val := opt) { val = 200; }
   • 管道语法：if (opt) |*val| { val = 200; }
   • （状态机内部已透明支持块级局部变量解引用，可直接就地赋值修改）；
3. 结构体展开更新（Spread）：
   • 支持 TS/Zig 风格的展开构造：let next_state = State { ...prev_state, status: 1 };；
4. 编译测试标准命令：
   • 始终通过 cap 工具执行测试：./cap/bin/cap cap/test/test_xxx.cp；正式版二进制位于 /data/dev/cp/cap/bin/cpc（单文件转译耗时仅约 2~5ms）。

---

## 十九、Token 消耗管控与任务效率平衡铁律 (Token Economy & High-Efficiency Protocol)

为了杜绝大模型上下文爆炸（Context Bloat）与 Token 额度过快耗尽（Quota 429），所有 AGY 智能体在后续开发与维护中必须严格执行以下准则，实现 **低 Token 消耗与极致执行效率的最佳平衡**：

### 1. 智能体弹性分期治理（阶段驱动原则）

- **广度迁移期（大批量改写）**：允许启动子智能体并行处理互不依赖的独立大模块；任务颗粒度按目录或模块组切分，任务完成后立即汇报结果并归拢。
- **深度收敛期（排错、编译打通与回归测试）**：
  - 严禁盲目长期维持 7 路全开！当全工程已处于 90%+ 编译就绪、仅剩局部编译报错时，**必须由主智能体单点精准突破**，坚决切断 `主智能体 Token + N × 子智能体上下文` 的乘数倍增效应。
  - 完成任务的子智能体需及时清理或转入休眠，禁止持续在后台空转发送心跳。

### 2. 上下文与文件 I/O 严格切片规范（Surgical I/O）

- **终端编译日志抑制**：运行 `bun bd`、`ninja` 或测试时，若遇到错误，模型仅抓取并分析最后的关键编译报错摘要（如 `file.zig:line:col: error: ...`），避免将数万行的构建垃圾日志卷入历史上下文。

### 3. 本地机器自愈通道优先（Zero-Token / Local Diagnostic）

- **先行 AST 语法秒级验证**：任何 `.cp` 文件转译为 `.zig` 后，先在本地执行秒级单文件验证：

  ```bash
  cpc check --json path/to/file.cp
  vendor/zig/zig ast-check path/to/file.zig
  ```

- **结构化 edits 采纳**：优先使用 `cpc check --json` 输出的精确 `offset` 与 `edits`，毫秒级单步自愈，严禁靠多轮自然语言猜测试错。

### 4. 批处理优先于微步操作（Batching Over Micro-Steps）

- 同一模块中发现的多处同类语法问题（如去除冗余的 `: void`、修复 pipe 捕获为 walrus 语法、清理 `let mut`），必须在单轮中一次性合并完成所有 `edit_file`，再发起单次验证，严禁“改一行、测一次、聊一句”的高频碎步交互。

---

## 二十、跨智能体异步协作看板协议 (Task Mailbox) 与工程提速脚本指南

在分布式双智能体（Bun AGY 负责主工程迁移，CP AGY 负责上游编译器特性支持）协同作业中，为**彻底杜绝人工盯梢与传话等待**，采用 **“Maildir 目录级原子操作（防并发写穿） + 仅追加事件日志（防数据损坏） + Reactive Wakeup（零 Token 休眠唤醒）”** 架构。

### 1. 跨智能体异步任务看板架构

- **任务存储核心（Maildir 架构，POSIX 系统级原子隔离）**：
  - 待处理队列：`/data/dev/cp/.tasks/pending/<task-id>.json`
  - 已完成归档：`/data/dev/cp/.tasks/resolved/<task-id>.json`
  - 机制保证：状态流转通过底层 `os.rename` 瞬间原子完成，物理级杜绝多进程并发读写冲突与写穿。
- **全局事件审计流（仅追加，Append-Only Event Stream）**：
  - 看板文件：`/data/dev/cp/TASK_QUEUE.jsonl`
  - 任何任务提交与结单均以单行 JSON 仅追加写入（Append-Only），杜绝全文件重写覆盖风险。
- **任务定义标准格式**：
  ```json
  {
    "id": "task-20261110-01",
    "repro": "cap/test/repro/repro_xxx.cp",
    "error": "详细报错信息与预期行为",
    "status": "pending | resolved",
    "created_by": "bun_agy",
    "timestamp": "2026-11-10T08:30:00"
  }
  ```

#### 1.1 Bun AGY 行为军规（保通降级，绝不单点死锁）

1. **遇到阻断级编译器缺陷**：若某 `.cp` 文件遇到当前 `cpc` 编译器缺失特性或深层借用报错，**严禁在当前文件死磕停滞**。
2. **提取最小复现用例**：编写 $\le 10$ 行最小复现代码，写入 `cap/test/repro/repro_<name>.cp`（或 `temp/`）。
3. **投递异步任务**：直接调用一键投递工具（脚本自动归一化为绝对路径，杜绝跨工作区相对路径解析歧义）：
   ```bash
   python3 scripts/task_queue.py submit --id task-001 --repro temp/repro_xxx.cp --error "简明报错说明"
   ```
4. **就地保通降级（核心动作）**：
   - 在当前 Bun 代码中，使用 `zig { ... }` 原生逃逸块包裹该疑难逻辑，或打上 `// TODO(cpc): [task-id] 待上游支持后重构`。
   - **绝不死等，立即继续推进后续文件或整个模块的宏观编译与测试！**
5. **任务查询与唤醒（可选）**：若需同步等待某任务修复，可运行：
   `python3 scripts/watch_task.py --status resolved --id task-001`

#### 1.2 CP AGY 行为军规（被动唤醒，闭环自愈）

1. **无任务时挂起监听**：在待命状态下调用：
   `python3 scripts/watch_task.py`
   - **零延迟即时检测**：启动首轮立即扫描目录，若已有 pending 任务直接 0 延迟退出开工；
   - **抗并发与容错**：自动忽略半写入文件（`JSONDecodeError` 自动重试）；
   - 利用 Antigravity CLI 的后台任务退出即唤醒机制（Reactive Wakeup），CP AGY 结束当前轮次，**进入完全休眠（0 Token 消耗）**。
2. **自动唤醒与认领**：一旦检测到新任务，`watch_task.py` 立即正常退出（Exit 0），**系统毫秒级被动唤醒 CP AGY**！
3. **修改与部署**：修复 `cpc/src` 源码后，运行 `bash scripts/sync_cpc.sh` 完成极速编译生效。
4. **验收与结单**：通过 `./cap/bin/cap <repro_case>` 验证通过后，调用：
   ```bash
   python3 scripts/task_queue.py resolve <task_id> --note "已修复并验证"
   ```
   （底层原子将任务文件移入 `.tasks/resolved/` 并追加事件日志）。
5. **重回监听**：再次运行 `python3 scripts/watch_task.py`，进入下一轮待命休眠。

---

### 2. 工程提速脚本指南 (Scripts Reference)

两工作区均已内置以下提速工具脚本（位于 `scripts/` 下），所有 AGY 必须严格按指引调用：

| 脚本路径                  | 核心作用                                                                                                      | 适用场景与调用时机                                                                                                   | 具体调用命令示例                                                                                                                                                                                                             |
| :------------------------ | :------------------------------------------------------------------------------------------------------------ | :------------------------------------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `scripts/sync_cpc.sh`     | **原子构建并部署 cpc**<br>运行 `cargo check` + Debug `cargo build` + 覆盖部署到 `/data/dev/cp/cap/bin/cpc`    | 只要修改了 `/data/dev/cp/cpc/src` 编译器源码，**必须且只能**调用此脚本完成生效。严禁在对话中手写长串 cargo/cp 命令！ | `bash scripts/sync_cpc.sh`                                                                                                                                                                                                   |
| `scripts/task_queue.py`   | **原子任务队列管理器**<br>Maildir 目录级原子写入与迁移，杜绝并发竞争；自动同步仅追加审计日志                  | • Bun 投递任务：`submit`<br>• CP 结单任务：`resolve`<br>• 查看看板状态：`list`                                       | • 投递：`python3 scripts/task_queue.py submit --id task-001 --repro path/repro.cp --error "..."`<br>• 结单：`python3 scripts/task_queue.py resolve task-001 --note "已修复"`<br>• 查看：`python3 scripts/task_queue.py list` |
| `scripts/watch_task.py`   | **事件驱动任务监听器**<br>启动即时零延迟检查；检测到匹配状态的任务时瞬间退出(Exit 0)，触发 CLI 反应式自动唤醒 | CP AGY 无任务时挂起休眠；或 Bun AGY 需要等待特定任务被修复时挂起等待。                                               | • CP AGY 监听新任务：<br>`python3 scripts/watch_task.py`<br>• Bun AGY 等待特定修复：<br>`python3 scripts/watch_task.py --status resolved --id task-001`                                                                      |
| `scripts/resolve_task.py` | **原子更新任务状态（快捷别名）**<br>自动转发调用 `task_queue.py resolve`                                      | CP AGY 完成编译器 Bug 修复并验证通过后，调用此脚本通知任务结单。                                                     | `python3 scripts/resolve_task.py task-001 --note "修复借用检查器解构问题"`                                                                                                                                                   |

## 二十一、主智能体统筹调度与子智能体（Subagents）并行攻坚规范（Token 极简防暴增协议）

为实现最高开发吞吐量并彻底杜绝 Token 指数级暴增，主智能体与子智能体严格执行以下协作与管控体系：

### 1. 规范源唯一真理准绳：`/data/dev/cp/cp_agent_ref.md` 精炼规范
1. **单一规范契约**：上游 `cp agy` 维护的 `/data/dev/cp/cp_agent_ref.md`（300行精炼规范）是所有智能体（主智能体、子智能体、上游智能体）在代码重构中的最高语法契约。
2. **极简语法裁决**：
   - 模块导入：唯一规范 `import std from "std";`、`import bun from "bun";`（严禁 `import "std" as std;`）；
   - 变量绑定：仅使用 `const` 与 `let`（严禁 `var`，严禁 `let mut`）；
   - 裸指针语法：非空裸指针严格写为 **`*!`**（映射 Zig `*anyopaque`），可空裸指针严格写为 **`*!?`**（映射 Zig `?*anyopaque`）。严禁在 CP 文件中出现 `*anyopaque`、`?*anyopaque`、`*any!`、`*any!?`；
   - 元组与对象解构：严格采用 `let [a, b] = expr;` 与 `let { x, y } = expr;`，严禁使用 `.@"0"` / `.@"1"` 属性绕过；
   - 接收者修饰：独占可变修饰为 `*this`，只读修饰为 `this`。
3. **查阅权限与极简下发**：
   - 主智能体拥有对 `/data/dev/cp/cpc/src`（Rust AST、Parser、Sema）的只读查阅权限，用于核对语义细节；
   - 派离子智能体前，主智能体必须将精炼规范与避坑要点注入任务 Brief，子智能体严格对照，杜绝重复探索与 Token 浪费。

---

### 2. 子智能体（Subagents）并行开发铁律与强制分发门槛
1. **强制切片分发门槛（严禁主线程串行积压）**：
   - 凡重构子系统文件数 $\ge 3$ 或总代码量 $> 500$ 行，**严禁主智能体在自身单线程上下文中串行逐个改写**！
   - 主智能体必须将模块划分为独立切片，调用 `invoke_subagent` 派发专用子智能体并行攻坚，**同时运行的并发子智能体严格限制在 2 路以内（严禁超过 2 路，防止 Token 配额过快消耗）**；
   - 主智能体上下文严格保持清爽，仅负责分发、终审验收、四线文档记录与原子 Git 提交。
2. **纯化子智能体职责**：
   - 工具仅限 `read_file`、`edit_file`、`write_file` 与 `run_command`（本地运行 `cpc --lib` 与 `zig ast-check`）；
   - **三不原则**：子智能体绝不修改编译器、绝不更新四线文档、绝不执行 Git 提交。
3. **Token 熔断安全阀（5 轮尝试上限）**：
   - 单个文件编译报错尝试最多 5 轮；超过 5 轮仍未通过且判定为编译器缺陷时，立即触发熔断，提取 $\le 10$ 行最小复现并向主智能体汇报，由主智能体接管后续调度。

---

### 3. 与 cp agy 双向交互规范：屏幕实时回显与 SESSION.md 即时归档
为保证人机协同高度透明，消除“通信停摆”疑虑，主智能体与 `cp agy` 交互必须遵守：
1. **屏幕显式回显**：
   - **发送任务时**：必须在回复中输出清晰高亮的任务摘要（包含任务 ID、关联模块、报错类型、精简复现用例）；
   - **接收结单时**：后台 `watch_task.py` 检测到任务解决后，必须在对话中即时回显接收到的处理结果（任务 ID、修复结论、cpc 新特性或补丁详情）。
2. **SESSION.md 实时追加写入**：
   - 双向交互事件（发送任务 / 收到结单通知）**必须在发生的第一时间实时追加写入 `SESSION.md`**，绝不允许拖延到阶段性里程碑才记录！

---

### 4. 彻底杜绝“投机取巧”：编译器 Bug 阻断上报与静态门禁
1. **严禁非标语法绕过**：
   - 遇到 `cpc` 编译器缺陷、借用检查假阳性或语法不支持时，**严禁在 CP 代码中倒退使用 Zig 关键字（如 `var`、`zig {}`、`.@"0"`、`*anyopaque`）进行临时绕过**！
   - 这种行为掩盖了编译器 Bug，严重污染了 CP 代码库，属于严厉禁止的违规行为。
2. **标准化阻断与异步切换流程**：
   - 发现编译器漏洞 $\rightarrow$ 提炼最小可复现用例 $\rightarrow$ 调用 `python3 scripts/task_queue.py submit` 下发给 `cp agy`；
   - 将当前受阻文件标记为 `BLOCKED`，记录任务 ID，**立即并行切入下一个无依赖的文件或模块攻坚**，绝不在原地打转或用非标语法降级；
   - 收到结单通知后，拉取最新 `cpc` 重新编译验证。
3. **交付前静态正则门禁（必须全为 0）**：
   在验收子智能体产物与 Git 提交前，主智能体必须运行以下扫描，存在任何命中即刻拒绝合入：
   ```bash
   rg "zig\s*\{" src/**/*.cp                   # 严禁原生逃逸块
   rg "^\s*var\b" src/**/*.cp                 # 严禁 var 声明（必须 const/let）
   rg '\.@"0"' src/**/*.cp                    # 严禁元组数字属性逃逸
   rg "\*anyopaque|\?\*anyopaque" src/**/*.cp # 严禁裸露 Zig anyopaque
   rg "\*any!|\*any!\?" src/**/*.cp           # 严禁非标冗余裸指针
   ```

---

### 5. 阶段性成果保护与原子 Git 提交铁律
1. **阶段性里程碑及时固化**：
   - 每完成一个完整子目录（如 `src/bun_alloc/`），或消除一组重大系统级阻断后，必须立即完成四线文档同步并执行 1 次原子 `git commit`；
   - 严禁将多个大型子系统的修改拖延、积压到最后做“巨无霸”式提交。
2. **提交纪律**：
   - 先写全文档（`SESSION.md`、`CHANGELOG.md`、`DEVLOG.md`），将提交指令与变更范围完整记录；
   - 一次性暂存与提交：`git add .` 后只执行唯一 1 次 `git commit`；
   - 提交后工作区绝对干净，严禁 `--amend`，严禁二次提交。

---

### 6. 配额耗尽（Quota Exhaustion）自主恢复与无人值守守护机制
当触发 `⚠ Individual quota reached... Resets in 2h...` 或大模型 API 出现配额耗尽限流时：
1. **主动注册后台定时唤醒（`schedule` 工具）**：
   - 主智能体严禁直接退出成为死进程！必须根据提示计算剩余时间（如 2h3m7s $\approx$ 7387s），加上 2 分钟安全缓冲（如 7500 秒），调用系统级 `schedule` 工具注册单次高优先级唤醒定时器：
     ```json
     schedule(
       DurationSeconds=7500,
       Prompt="配额已重置完毕！请立即自检系统状态，校验 cp agy 任务队列，启动子智能体继续攻坚剩余模块。"
     )
     ```
   - 注册后立即安全休眠结束当前轮次；
   - 7500 秒后系统底层事件将自动推送该 Prompt，唤醒主智能体在配额恢复的第一时间无缝自动继续工作，彻底实现无人值守连续运转。
2. **用户端建议**：推荐用户使用 `/schedule` 或 `/goal` 指令保障长程自主攻坚。

---

## 其他你需要遵守的规则

- 后台常驻通信任务（`scripts/watch_task.py`），随时接收 `cp agy` 发送的任务结单通知；
- 主智能体与 `cp agy` 的每一次任务交互（发送与接收），必须在屏幕上实时回显并实时追加写入 `SESSION.md`；
- 所有上游编译器 `cpc/src` 修改需求，统一通过 `task_queue.py` 提交给 `cp agy` 完成，严禁主智能体私自修改编译器源码破坏已有功能；
- 每次提交任务给 `cp agy` 后，主智能体启动超时追踪（每隔 10 分钟在 `SESSION.md` 中记录状态），保持全局进度清晰可查。
