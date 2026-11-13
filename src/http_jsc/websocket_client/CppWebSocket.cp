import bun from "bun";
import WebSocketDeflate from "./WebSocketDeflate.zig";
import WsClientPkg from "../websocket_client.zig";

const ErrorCode = WsClientPkg.ErrorCode;
const jsc = bun.jsc;
const uws = bun.uws;

extern fn WebSocket__didConnect(websocket_context: *CppWebSocket, socket: *uws.Socket, buffered_data: ([*]u8!)?, buffered_len: usize, deflate_params: (*const WebSocketDeflate.Params)?, secure: (*uws.SslCtx)?) void;
extern fn WebSocket__didConnectWithTunnel(websocket_context: *CppWebSocket, tunnel: *!, buffered_data: ([*]u8!)?, buffered_len: usize, deflate_params: (*const WebSocketDeflate.Params)?) void;
extern fn WebSocket__didAbruptClose(websocket_context: *CppWebSocket, reason: ErrorCode) void;
extern fn WebSocket__didClose(websocket_context: *CppWebSocket, code: u16, reason: *const bun.String) void;
extern fn WebSocket__didReceiveText(websocket_context: *CppWebSocket, clone: bool, text: *const jsc.ZigString) void;
extern fn WebSocket__didReceiveBytes(websocket_context: *CppWebSocket, bytes: [*]const u8!, byte_len: usize, opcode: u8) void;
extern fn WebSocket__rejectUnauthorized(websocket_context: *CppWebSocket) bool;
extern fn WebSocket__incrementPendingActivity(websocket_context: *CppWebSocket) void;
extern fn WebSocket__decrementPendingActivity(websocket_context: *CppWebSocket) void;
extern fn WebSocket__setProtocol(websocket_context: *CppWebSocket, protocol: *bun.String) void;

@opaque export struct CppWebSocket {
    pub fn didAbruptClose(*this, reason: ErrorCode) void {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        unsafe { WebSocket__didAbruptClose(this, reason); }
    }

    pub fn didClose(*this, code: u16, reason: *bun.String) void {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        unsafe { WebSocket__didClose(this, code, reason); }
    }

    pub fn didReceiveText(*this, clone: bool, text: *const jsc.ZigString) void {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        unsafe { WebSocket__didReceiveText(this, clone, text); }
    }

    pub fn didReceiveBytes(*this, bytes: [*]const u8!, byte_len: usize, opcode: u8) void {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        unsafe { WebSocket__didReceiveBytes(this, bytes, byte_len, opcode); }
    }

    pub fn rejectUnauthorized(*this) bool {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        return unsafe { WebSocket__rejectUnauthorized(this) };
    }

    pub fn didConnect(*this, socket: *uws.Socket, buffered_data: ([*]u8!)?, buffered_len: usize, deflate_params: (*const WebSocketDeflate.Params)?, secure: (*uws.SslCtx)?) void {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        unsafe { WebSocket__didConnect(this, socket, buffered_data, buffered_len, deflate_params, secure); }
    }

    pub fn didConnectWithTunnel(*this, tunnel: *!, buffered_data: ([*]u8!)?, buffered_len: usize, deflate_params: (*const WebSocketDeflate.Params)?) void {
        const vm: *jsc.VirtualMachine = jsc.VirtualMachine.get();
        const loop = vm.eventLoop();
        loop.enter();
        defer loop.exit();
        unsafe { WebSocket__didConnectWithTunnel(this, tunnel, buffered_data, buffered_len, deflate_params); }
    }

    pub fn ref(*this) void {
        jsc.markBinding(@src());
        unsafe { WebSocket__incrementPendingActivity(this); }
    }

    pub fn unref(*this) void {
        jsc.markBinding(@src());
        unsafe { WebSocket__decrementPendingActivity(this); }
    }

    pub fn setProtocol(*this, protocol: *bun.String) void {
        jsc.markBinding(@src());
        unsafe { WebSocket__setProtocol(this, protocol); }
    }
}
