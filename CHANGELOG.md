# 2026-11-12 变更日志 (CHANGELOG)

## 模块迁移与编译产物

### `src/runtime/socket/` 全子系统 14 个核心网络套接字模块
- `Handlers.cp` / `Handlers.zig`:
  - 迁移为纯 CP 语言，去除 `var`，验证 `cpc --lib` 与 `vendor/zig/zig ast-check`。
- `Listener.cp` / `Listener.zig`:
  - 重写数组乘法为 `[64]u8 = undefined`；重写 `orelse` 语句块；
  - 修复 `const this_ptr` 为 `let` 支持解引用赋值；
  - 作用域隔离 `prev` 捕获变量与引入 `bun.cast(*TLSSocket, prev_tls)` 彻底消除跨分支移动语义误报；
  - 移除冗余导出消解符号歧义，生成 1,124 行（+0.36%），双重验证全部通过。
- `SSLConfig.cp` / `SSLConfig.zig`:
  - 修复联合体与结构体配置，规范 C-ABI 字段与生命周期，生成 607 行（+5.2%），双重验证全部通过。
- `SocketAddress.cp` / `SocketAddress.zig`:
  - 修复多地址族转换与 IP 缓冲区处理，生成 570 行（+12.6%），双重验证全部通过。
- `UpgradedDuplex.cp` / `UpgradedDuplex.zig`:
  - 规范双工流管道状态与缓冲区转移，生成 456 行（+4.8%），双重验证全部通过。
- `WindowsNamedPipe.cp` / `WindowsNamedPipe.zig`:
  - 全面消除 `var`，规范 Windows 管道句柄操作，双重验证全部通过。
- `WindowsNamedPipeContext.cp`:
  - 双重验证全部通过，0 处 `zig {}`，0 处 `var`。
- `socket.cp` / `socket.zig`:
  - 彻底攻克 2,028 行超大型核心套接字实现；
  - 规避保留关键字 `in` 为 `in_buf`；
  - 将泛型返回类型由 `class` 调整为 `struct`；
  - 修复 328 处 `_` 裸标识符为 `this`，消解块表达式为标准 if-else；
  - 命名空间重构消除 `string` 与 `u8` 类型遮蔽；
  - 生成 2,126 行 vs 原始 2,286 行（-7.00% 净压缩），双重验证全部通过。
- `ssl_wrapper.cp`:
  - 消除残留 `var`，生成 420 行 vs 原版 640 行（-34.5% 净压缩），双重验证全部通过。
- `tls_socket_functions.cp` / `tls_socket_functions.zig`:
  - 消除跨分支移动误判，生成 648 行 vs 原版 673 行（-3.7% 净压缩），双重验证全部通过。
- `udp_socket.cp` / `udp_socket.zig`:
  - 攻克 1,010 行 UDP 套接字模块；
  - 修复哨兵数组语法为定长缓冲区 `[67]u8`；
  - 重命名保留字 `interface` 为 `iface`，`arena` 为 `temp_arena`；
  - 替换转义字符为整型字节字面量；采用 `@c fn` 声明顶层 C-ABI 函数；
  - 修复本地 `socket` 成员遮蔽与解引用赋值；
  - 生成 1,168 行 vs 原版 1,091 行（+7.06%），双重验证全部通过。
- `uws_dispatch.cp`, `uws_handlers.cp`, `uws_jsc.cp`:
  - 全面消除 `var`，双重验证全部通过。

### 冷热文档归档
- `SESSION_H.md`, `CHANGELOG_H.md`, `DEVLOG_H.md`: 归档历史记录，确立热日志极简轻量。
