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

### `src/uws_sys/` 全网络层子系统 24 个模块
- `App.cp` / `App.zig`: 285 行 (-38.8% 压缩)，消解泛型构造，双重验证全部通过。
- `BodyReaderMixin.cp` / `BodyReaderMixin.zig`: 79 行 (-16.0% 压缩)，双重验证全部通过。
- `ConnectingSocket.cp` / `ConnectingSocket.zig`: 79 行 (-1.2% 压缩)，双重验证全部通过。
- `InternalLoopData.cp` / `InternalLoopData.zig`: 73 行 (0.0%)，双重验证全部通过。
- `ListenSocket.cp` / `ListenSocket.zig`: 74 行 (+7.2%)，双重验证全部通过。
- `Loop.cp` / `Loop.zig`: 304 行 (-18.3% 压缩)，双重验证全部通过。
- `Request.cp` / `Request.zig`: 110 行 (+13.4%)，规范枚举与属性提取，双重验证全部通过。
- `Response.cp` / `Response.zig`: 634 行 (-18.9% 压缩)，重构响应流管线与回调，双重验证全部通过。
- `SocketContext.cp` / `SocketContext.zig`: 148 行 (+6.5%)，规范 SSL 上下文配置与辅助函数，双重验证全部通过。
- `SocketGroup.cp` / `SocketGroup.zig`: 172 行 (-17.3% 压缩)，规范 C 套接字组生命周期管理，双重验证全部通过。
- `SocketKind.cp` / `SocketKind.zig`: 53 行 (-47.5% 压缩)，双重验证全部通过。
- `Timer.cp` / `Timer.zig`: 50 行 (-21.9% 压缩)，消除裸指针异常，双重验证全部通过。
- `WebSocket.cp` / `WebSocket.zig`: 314 行 (-13.3% 压缩)，重构 WS 状态机与握手逻辑，双重验证全部通过。
- `h3.cp` / `h3.zig`: 478 行 (+10.1%)，规范 HTTP/3 路由器与回调分发，双重验证全部通过。
- `quic.cp` / `quic.zig`: 21 行 (+5.0%)，规范模块重导出，双重验证全部通过。
- `quic/Context.cp`, `quic/Header.cp`, `quic/PendingConnect.cp`, `quic/Socket.cp`, `quic/Stream.cp`:
  - 5 个 QUIC 核心模块全部 100% 迁移并通过双重验证。
- `socket.cp` / `socket.zig`: 656 行 (+0.5%)，使用 `pub static fn` 替代原生 Zig 内部结构体捕获，消除 `let` 未转译报错与 `field` 作用域遮蔽，双重验证全部通过。
- `udp.cp` / `udp.zig`: 140 行 (+18.6%)，规范 UDP 套接字数据包路由，双重验证全部通过。
- `us_socket_t.cp` / `us_socket_t.zig`: 288 行 (-17.7% 压缩)，规范底层 C 结构体与回调指针，双重验证全部通过。
- `vtable.cp` / `vtable.zig`: 98 行 (-16.9% 压缩)，规范虚函数表派发与 trampoline，双重验证全部通过。

### `src/runtime/webcore/blob/` 底层二进制与存储子系统 4 个核心模块
- `Store.cp` / `Store.zig`:
  - 572 行 vs 原版 577 行 (-0.9%)；
  - 拆解嵌套内部类 `File`, `S3`, `Bytes` 为顶层独立类 `StoreFile`, `StoreS3`, `StoreBytes`，并在 `Store` 内部设置别名；
  - 修复 `import { Value as AtomicValue }` 为两段式模块别名导入；
  - 规范内层异步回调包装器结构体避免参数名遮蔽，双重验证全部通过。
- `copy_file.cp` / `copy_file.zig`:
  - 817 行 vs 原版 1,286 行 (-36.5% 净压缩)；
  - 消除裸接收器 `*_`，全面规范为 `*this: *CopyFile` 声明可变方法接收器；
  - 拆解嵌套类 `ReadWriteLoop` 为顶层 `CopyFileWindowsReadWriteLoop`；
  - 规范 `@c static fn onRead`, `onWrite`, `onChmod` 消除字段伪冲突；
  - 优化克隆降级逻辑消除 catch 块中 unreachable 跳转；
  - 修复 Zig 关键字枚举 `.@"continue"`，双重验证全部通过。
- `read_file.cp` / `read_file.zig`:
  - 751 行 vs 原版 829 行 (-9.4% 净压缩)；
  - 引入 `const ClosingStateAtomic = AtomicValue(ClosingState);` 规避泛型类字段定义冲突；
  - 采用顶层 `export type ReadFileOnReadFileCallback` 声明函数指针别名；
  - 显式声明静态回调函数 `@c static fn onReadable`, `onRequestReadable`, `doReadLoopTask`, `onFileInitialStat`, `onRead`；
  - 消除冗余 `@ptrCast`，双重验证全部通过。
- `write_file.cp` / `write_file.zig`:
  - 684 行 vs 原版 757 行 (-9.6% 净压缩)；
  - 规范可变接收器 `*this: *WriteFile` 和 `*this: *WriteFileWindows`；
  - 消除多分支模式匹配尾部逗号语法问题；
  - 提升 `const log` 与 `const bloblog` 至模块顶层消除重复定义；
  - 规避平台指针断言过度嵌套类型转换，双重验证全部通过。

### 冷热文档归档
- `SESSION_H.md`, `CHANGELOG_H.md`, `DEVLOG_H.md`: 归档历史记录，确立热日志极简轻量。
