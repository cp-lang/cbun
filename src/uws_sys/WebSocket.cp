
pub const CorkCallback = ?*const fn (?*anyopaque) callconv(.c) void;
pub const TopicCallback = ?*const fn ([*c]const u8, usize, ?*anyopaque) callconv(.c) void;

pub const uws_websocket_handler = ?*const fn (*RawWebSocket) callconv(.c) void;
pub const uws_websocket_message_handler = ?*const fn (*RawWebSocket, [*c]const u8, usize, bun.uws.Opcode) callconv(.c) void;
pub const uws_websocket_close_handler = ?*const fn (*RawWebSocket, i32, [*c]const u8, usize) callconv(.c) void;
pub const uws_websocket_upgrade_handler = ?*const fn (?*anyopaque, *uws_res, *Request, *bun.uws.WebSocketUpgradeContext, usize) callconv(.c) void;
pub const uws_websocket_ping_pong_handler = ?*const fn (*RawWebSocket, [*c]const u8, usize) callconv(.c) void;
// src/uws_sys/WebSocket.cp
// WebSocket wrapper and behavior dispatcher in CP language

import bun from "bun";
import { Request } from "./Request.cp";
import { uws_res } from "./Response.cp";
import { uws_app_t } from "./App.cp";

export @opaque class RawWebSocket {
    pub fn memoryCost(*this, ssl_flag: i32): usize {
        return uws_ws_memory_cost(ssl_flag, this);
    }

    pub fn asSocket(*this): *bun.uws.Socket {
        return bun.cast(*bun.uws.Socket, this);
    }
}

@c {
    fn uws_ws_memory_cost(ssl: i32, ws: *RawWebSocket): usize;
    fn uws_ws(ssl: i32, app: *uws_app_t, ctx: *any!, pattern: [*]const u8!, pattern_len: usize, id: usize, behavior: *const WebSocketBehavior): void;
    fn uws_ws_get_user_data(ssl: i32, ws: *RawWebSocket?!): *any?!;
    fn uws_ws_close(ssl: i32, ws: *RawWebSocket?!): void;
    fn uws_ws_send(ssl: i32, ws: *RawWebSocket?!, message: [*c]const u8, length: usize, opcode: bun.uws.Opcode): bun.uws.SendStatus;
    fn uws_ws_send_with_options(ssl: i32, ws: *RawWebSocket?!, message: [*c]const u8, length: usize, opcode: bun.uws.Opcode, compress: bool, fin: bool): bun.uws.SendStatus;
    fn uws_ws_send_fragment(ssl: i32, ws: *RawWebSocket?!, message: [*c]const u8, length: usize, compress: bool): bun.uws.SendStatus;
    fn uws_ws_send_first_fragment(ssl: i32, ws: *RawWebSocket?!, message: [*c]const u8, length: usize, compress: bool): bun.uws.SendStatus;
    fn uws_ws_send_first_fragment_with_opcode(ssl: i32, ws: *RawWebSocket?!, message: [*c]const u8, length: usize, opcode: bun.uws.Opcode, compress: bool): bun.uws.SendStatus;
    fn uws_ws_send_last_fragment(ssl: i32, ws: *RawWebSocket?!, message: [*c]const u8, length: usize, compress: bool): bun.uws.SendStatus;
    fn uws_ws_end(ssl: i32, ws: *RawWebSocket?!, code: i32, message: [*c]const u8, length: usize): void;
    fn uws_ws_cork(ssl: i32, ws: *RawWebSocket?!, handler: CorkCallback, user_data: ?*anyopaque): void;
    fn uws_ws_subscribe(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, length: usize): bool;
    fn uws_ws_unsubscribe(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, length: usize): bool;
    fn uws_ws_is_subscribed(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, length: usize): bool;
    fn uws_ws_iterate_topics(ssl: i32, ws: *RawWebSocket?!, callback: TopicCallback, user_data: ?*anyopaque): void;
    fn uws_ws_publish(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, topic_length: usize, message: [*]const u8!, message_length: usize): bool;
    fn uws_ws_publish_with_options(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, topic_length: usize, message: [*]const u8!, message_length: usize, opcode: bun.uws.Opcode, compress: bool): bool;
    fn uws_ws_get_buffered_amount(ssl: i32, ws: *RawWebSocket?!): usize;
    fn uws_ws_get_remote_address(ssl: i32, ws: *RawWebSocket?!, dest: *[*c]u8): usize;
    fn uws_ws_get_remote_address_as_text(ssl: i32, ws: *RawWebSocket?!, dest: *[*c]u8): usize;
}

pub fn WebSocket(comptime ssl_flag: i32): type { return struct {
    pub fn raw(this: *@This()): *RawWebSocket {
        return bun.cast(*RawWebSocket, this);
    }

    pub fn as(this: *@This(), comptime T: type): ?*T {
        return bun.cast(?*T, uws_ws_get_user_data(ssl_flag, this.raw()));
    }

    pub fn close(this: *@This()): void {
        uws_ws_close(ssl_flag, this.raw());
    }

    pub fn send(this: *@This(), message: string, opcode: bun.uws.Opcode): bun.uws.SendStatus {
        return uws_ws_send(ssl_flag, this.raw(), message.ptr, message.len, opcode);
    }

    pub fn sendWithOptions(this: *@This(), message: string, opcode: bun.uws.Opcode, compress: bool, fin: bool): bun.uws.SendStatus {
        return uws_ws_send_with_options(ssl_flag, this.raw(), message.ptr, message.len, opcode, compress, fin);
    }

    pub fn memoryCost(this: *@This()): usize {
        return this.raw().memoryCost(ssl_flag);
    }

    pub fn sendLastFragment(this: *@This(), message: string, compress: bool): bun.uws.SendStatus {
        return uws_ws_send_last_fragment(ssl_flag, this.raw(), message.ptr, message.len, compress);
    }

    pub fn end(this: *@This(), code: i32, message: string): void {
        uws_ws_end(ssl_flag, this.raw(), code, message.ptr, message.len);
    }

    pub fn cork(this: *@This(), ctx: anytype, comptime callback: anytype): void {
        const Wrapper = struct {
            pub fn wrap(user_data: ?*anyopaque) callconv(.c) void {
                callback(@ptrCast(user_data));
            }
        };
        uws_ws_cork(ssl_flag, this.raw(), Wrapper.wrap, @ptrCast(ctx));
    }

    pub fn subscribe(this: *@This(), topic: string): bool {
        return uws_ws_subscribe(ssl_flag, this.raw(), topic.ptr, topic.len);
    }

    pub fn unsubscribe(this: *@This(), topic: string): bool {
        return uws_ws_unsubscribe(ssl_flag, this.raw(), topic.ptr, topic.len);
    }

    pub fn isSubscribed(this: *@This(), topic: string): bool {
        return uws_ws_is_subscribed(ssl_flag, this.raw(), topic.ptr, topic.len);
    }

    pub fn publish(this: *@This(), topic: string, message: string): bool {
        return uws_ws_publish(ssl_flag, this.raw(), topic.ptr, topic.len, message.ptr, message.len);
    }

    pub fn publishWithOptions(this: *@This(), topic: string, message: string, opcode: bun.uws.Opcode, compress: bool): bool {
        return uws_ws_publish_with_options(ssl_flag, this.raw(), topic.ptr, topic.len, message.ptr, message.len, opcode, compress);
    }

    pub fn getBufferedAmount(this: *@This()): u32 {
        return @intCast(uws_ws_get_buffered_amount(ssl_flag, this.raw()));
    }

    pub fn getRemoteAddress(this: *@This(), buf: []u8): []u8 {
        let ptr: [*c]u8 = undefined;
        const len = uws_ws_get_remote_address(ssl_flag, this.raw(), &ptr);
        bun.copy(u8, buf, ptr[0..len]);
        return buf[0..len];
    }
    };
}

export enum AnyWebSocket {
    ssl(*WebSocket(1)),
    tcp(*WebSocket(0)),

    pub fn raw(this): *RawWebSocket {
        return switch (this) {
            .ssl(w) => w.raw(),
            .tcp(w) => w.raw(),
        };
    }

    pub fn as(this, comptime T: type): ?*T {
        return switch (this) {
            .ssl(w) => w.as(T),
            .tcp(w) => w.as(T),
        };
    }

    pub fn memoryCost(this): usize {
        return switch (this) {
            .ssl(w) => w.memoryCost(),
            .tcp(w) => w.memoryCost(),
        };
    }

    pub fn close(this): void {
        switch (this) {
            .ssl(w) => w.close(),
            .tcp(w) => w.close(),
        };
    }

    pub fn send(this, message: string, opcode: bun.uws.Opcode, compress: bool, fin: bool): bun.uws.SendStatus {
        return switch (this) {
            .ssl(w) => w.sendWithOptions(message, opcode, compress, fin),
            .tcp(w) => w.sendWithOptions(message, opcode, compress, fin),
        };
    }

    pub fn sendLastFragment(this, message: string, compress: bool): bun.uws.SendStatus {
        return switch (this) {
            .ssl(w) => w.sendLastFragment(message, compress),
            .tcp(w) => w.sendLastFragment(message, compress),
        };
    }

    pub fn end(this, code: i32, message: string): void {
        switch (this) {
            .ssl(w) => w.end(code, message),
            .tcp(w) => w.end(code, message),
        };
    }

    pub fn cork(this, ctx: anytype, comptime callback: anytype): void {
        switch (this) {
            .ssl(w) => w.cork(ctx, callback),
            .tcp(w) => w.cork(ctx, callback),
        };
    }

    pub fn subscribe(this, topic: string): bool {
        return switch (this) {
            .ssl(w) => w.subscribe(topic),
            .tcp(w) => w.subscribe(topic),
        };
    }

    pub fn unsubscribe(this, topic: string): bool {
        return switch (this) {
            .ssl(w) => w.unsubscribe(topic),
            .tcp(w) => w.unsubscribe(topic),
        };
    }

    pub fn isSubscribed(this, topic: string): bool {
        return switch (this) {
            .ssl(w) => w.isSubscribed(topic),
            .tcp(w) => w.isSubscribed(topic),
        };
    }

    pub fn publish(this, topic: string, message: string, opcode: bun.uws.Opcode, compress: bool): bool {
        return switch (this) {
            .ssl(w) => w.publishWithOptions(topic, message, opcode, compress),
            .tcp(w) => w.publishWithOptions(topic, message, opcode, compress),
        };
    }

    pub fn getBufferedAmount(this): usize {
        return switch (this) {
            .ssl(w) => @intCast(w.getBufferedAmount()),
            .tcp(w) => @intCast(w.getBufferedAmount()),
        };
    }

    pub fn getRemoteAddress(this, buf: []u8): []u8 {
        return switch (this) {
            .ssl(w) => w.getRemoteAddress(buf),
            .tcp(w) => w.getRemoteAddress(buf),
        };
    }
}

export @extern @value class WebSocketBehavior {
    compression: i32 = 0,
    maxPayloadLength: u32 = 0xffffffff,
    idleTimeout: u16 = 120,
    maxBackpressure: u32 = 1024 * 1024,
    closeOnBackpressureLimit: bool = false,
    resetIdleTimeoutOnSend: bool = true,
    sendPingsAutomatically: bool = true,
    maxLifetime: u16 = 0,
    upgrade: uws_websocket_upgrade_handler = null,
    open: uws_websocket_handler = null,
    message: uws_websocket_message_handler = null,
    drain: uws_websocket_handler = null,
    ping: uws_websocket_ping_pong_handler = null,
    pong: uws_websocket_ping_pong_handler = null,
    close: uws_websocket_close_handler = null,

    pub static fn wrap(comptime ServerType: type, comptime Type: type, comptime ssl: bool, behavior: WebSocketBehavior): WebSocketBehavior {
        let Wrap = struct {
            pub fn onOpen(raw_ws: *RawWebSocket) callconv(.c) void {
                let ws = if (ssl) AnyWebSocket.ssl(bun.cast(*WebSocket(1), raw_ws)) else AnyWebSocket.tcp(bun.cast(*WebSocket(0), raw_ws));
                const this_val = ws.as(Type).?;
                Type.onOpen(this_val, ws);
            }

            pub fn onMessage(raw_ws: *RawWebSocket, message: [*c]const u8, length: usize, opcode: bun.uws.Opcode) callconv(.c) void {
                let ws = if (ssl) AnyWebSocket.ssl(bun.cast(*WebSocket(1), raw_ws)) else AnyWebSocket.tcp(bun.cast(*WebSocket(0), raw_ws));
                const this_val = ws.as(Type).?;
                Type.onMessage(this_val, ws, if (length > 0) message[0..length] else "", opcode);
            }

            pub fn onDrain(raw_ws: *RawWebSocket) callconv(.c) void {
                let ws = if (ssl) AnyWebSocket.ssl(bun.cast(*WebSocket(1), raw_ws)) else AnyWebSocket.tcp(bun.cast(*WebSocket(0), raw_ws));
                const this_val = ws.as(Type).?;
                Type.onDrain(this_val, ws);
            }

            pub fn onPing(raw_ws: *RawWebSocket, message: [*c]const u8, length: usize) callconv(.c) void {
                let ws = if (ssl) AnyWebSocket.ssl(bun.cast(*WebSocket(1), raw_ws)) else AnyWebSocket.tcp(bun.cast(*WebSocket(0), raw_ws));
                const this_val = ws.as(Type).?;
                Type.onPing(this_val, ws, if (length > 0) message[0..length] else "");
            }

            pub fn onPong(raw_ws: *RawWebSocket, message: [*c]const u8, length: usize) callconv(.c) void {
                let ws = if (ssl) AnyWebSocket.ssl(bun.cast(*WebSocket(1), raw_ws)) else AnyWebSocket.tcp(bun.cast(*WebSocket(0), raw_ws));
                const this_val = ws.as(Type).?;
                Type.onPong(this_val, ws, if (length > 0) message[0..length] else "");
            }

            pub fn onClose(raw_ws: *RawWebSocket, code: i32, message: [*c]const u8, length: usize) callconv(.c) void {
                let ws = if (ssl) AnyWebSocket.ssl(bun.cast(*WebSocket(1), raw_ws)) else AnyWebSocket.tcp(bun.cast(*WebSocket(0), raw_ws));
                const this_val = ws.as(Type).?;
                Type.onClose(this_val, ws, code, if (length > 0 and message != null) message[0..length] else "");
            }

            pub fn onUpgrade(ptr: ?*anyopaque, res: *uws_res, req: *Request, context: *bun.uws.WebSocketUpgradeContext, id: usize) callconv(.c) void {
                ServerType.onWebSocketUpgrade(
                    bun.cast(*ServerType, ptr),
                    bun.cast(*bun.uws.Response(if (ssl) 1 else 0), res),
                    req,
                    context,
                    id,
                );
            }
        };

        return WebSocketBehavior{
            .compression = behavior.compression,
            .maxPayloadLength = behavior.maxPayloadLength,
            .idleTimeout = behavior.idleTimeout,
            .maxBackpressure = behavior.maxBackpressure,
            .closeOnBackpressureLimit = behavior.closeOnBackpressureLimit,
            .resetIdleTimeoutOnSend = behavior.resetIdleTimeoutOnSend,
            .sendPingsAutomatically = behavior.sendPingsAutomatically,
            .maxLifetime = behavior.maxLifetime,
            .upgrade = Wrap.onUpgrade,
            .open = Wrap.onOpen,
            .message = if (@hasDecl(Type, "onMessage")) Wrap.onMessage else null,
            .drain = if (@hasDecl(Type, "onDrain")) Wrap.onDrain else null,
            .ping = if (@hasDecl(Type, "onPing")) Wrap.onPing else null,
            .pong = if (@hasDecl(Type, "onPong")) Wrap.onPong else null,
            .close = Wrap.onClose,
        };
    }
}
