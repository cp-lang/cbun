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
        return this as *bun.uws.Socket;
    }
}

@c {
    fn uws_ws_memory_cost(ssl: i32, ws: *RawWebSocket): usize;
    fn uws_ws(ssl: i32, app: *uws_app_t, ctx: *any!, pattern: [*]const u8!, pattern_len: usize, id: usize, behavior: *const WebSocketBehavior): void;
    fn uws_ws_get_user_data(ssl: i32, ws: *RawWebSocket?!): *any?!;
    fn uws_ws_close(ssl: i32, ws: *RawWebSocket?!): void;
    fn uws_ws_send(ssl: i32, ws: *RawWebSocket?!, message: [*]const u8!, length: usize, opcode: bun.uws.Opcode): bun.uws.SendStatus;
    fn uws_ws_send_with_options(ssl: i32, ws: *RawWebSocket?!, message: [*]const u8!, length: usize, opcode: bun.uws.Opcode, compress: bool, fin: bool): bun.uws.SendStatus;
    fn uws_ws_send_fragment(ssl: i32, ws: *RawWebSocket?!, message: [*]const u8!, length: usize, compress: bool): bun.uws.SendStatus;
    fn uws_ws_send_first_fragment(ssl: i32, ws: *RawWebSocket?!, message: [*]const u8!, length: usize, compress: bool): bun.uws.SendStatus;
    fn uws_ws_send_first_fragment_with_opcode(ssl: i32, ws: *RawWebSocket?!, message: [*]const u8!, length: usize, opcode: bun.uws.Opcode, compress: bool): bun.uws.SendStatus;
    fn uws_ws_send_last_fragment(ssl: i32, ws: *RawWebSocket?!, message: [*]const u8!, length: usize, compress: bool): bun.uws.SendStatus;
    fn uws_ws_end(ssl: i32, ws: *RawWebSocket?!, code: i32, message: [*]const u8!, length: usize): void;
    fn uws_ws_cork(ssl: i32, ws: *RawWebSocket?!, handler: @c ((ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_ws_subscribe(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, length: usize): bool;
    fn uws_ws_unsubscribe(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, length: usize): bool;
    fn uws_ws_is_subscribed(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, length: usize): bool;
    fn uws_ws_iterate_topics(ssl: i32, ws: *RawWebSocket?!, callback: @c ((topic: [*]const u8!, len: usize, ud: *any?!): void)?, user_data: *any?!): void;
    fn uws_ws_publish(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, topic_length: usize, message: [*]const u8!, message_length: usize): bool;
    fn uws_ws_publish_with_options(ssl: i32, ws: *RawWebSocket?!, topic: [*]const u8!, topic_length: usize, message: [*]const u8!, message_length: usize, opcode: bun.uws.Opcode, compress: bool): bool;
    fn uws_ws_get_buffered_amount(ssl: i32, ws: *RawWebSocket?!): usize;
    fn uws_ws_get_remote_address(ssl: i32, ws: *RawWebSocket?!, dest: *[*]u8!): usize;
    fn uws_ws_get_remote_address_as_text(ssl: i32, ws: *RawWebSocket?!, dest: *[*]u8!): usize;
}

export class WebSocket<const ssl_flag: i32> {
    pub fn raw(*this): *RawWebSocket {
        return this as *RawWebSocket;
    }

    pub fn as<T>(*this): *T?! {
        return uws_ws_get_user_data(ssl_flag, this.raw()) as *T?!;
    }

    pub fn close(*this): void {
        uws_ws_close(ssl_flag, this.raw());
    }

    pub fn send(*this, message: string, opcode: bun.uws.Opcode): bun.uws.SendStatus {
        return uws_ws_send(ssl_flag, this.raw(), message.ptr, message.len, opcode);
    }

    pub fn sendWithOptions(*this, message: string, opcode: bun.uws.Opcode, compress: bool, fin: bool): bun.uws.SendStatus {
        return uws_ws_send_with_options(ssl_flag, this.raw(), message.ptr, message.len, opcode, compress, fin);
    }

    pub fn memoryCost(*this): usize {
        return this.raw().memoryCost(ssl_flag);
    }

    pub fn sendLastFragment(*this, message: string, compress: bool): bun.uws.SendStatus {
        return uws_ws_send_last_fragment(ssl_flag, this.raw(), message.ptr, message.len, compress);
    }

    pub fn end(*this, code: i32, message: string): void {
        uws_ws_end(ssl_flag, this.raw(), code, message.ptr, message.len);
    }

    pub fn cork<Ctx>(*this, ctx: *Ctx, callback: fn(*Ctx): void): void {
        let Wrapper = struct {
            @c pub fn wrap(user_data: *any?!): void {
                callback(user_data as *Ctx);
            }
        };
        uws_ws_cork(ssl_flag, this.raw(), Wrapper.wrap, ctx as *any?!);
    }

    pub fn subscribe(*this, topic: string): bool {
        return uws_ws_subscribe(ssl_flag, this.raw(), topic.ptr, topic.len);
    }

    pub fn unsubscribe(*this, topic: string): bool {
        return uws_ws_unsubscribe(ssl_flag, this.raw(), topic.ptr, topic.len);
    }

    pub fn isSubscribed(*this, topic: string): bool {
        return uws_ws_is_subscribed(ssl_flag, this.raw(), topic.ptr, topic.len);
    }

    pub fn publish(*this, topic: string, message: string): bool {
        return uws_ws_publish(ssl_flag, this.raw(), topic.ptr, topic.len, message.ptr, message.len);
    }

    pub fn publishWithOptions(*this, topic: string, message: string, opcode: bun.uws.Opcode, compress: bool): bool {
        return uws_ws_publish_with_options(ssl_flag, this.raw(), topic.ptr, topic.len, message.ptr, message.len, opcode, compress);
    }

    pub fn getBufferedAmount(*this): u32 {
        return uws_ws_get_buffered_amount(ssl_flag, this.raw()) as u32;
    }

    pub fn getRemoteAddress(*this, buf: u8[]): u8[] {
        let ptr: [*]u8! = undefined;
        let len = uws_ws_get_remote_address(ssl_flag, this.raw(), &ptr);
        buf[0..len] = ptr[0..len];
        return buf[0..len];
    }
}

export enum AnyWebSocket {
    ssl(*WebSocket<1>),
    tcp(*WebSocket<0>),

    pub fn raw(this): *RawWebSocket {
        return match (this) {
            case .ssl(w) => w.raw(),
            case .tcp(w) => w.raw(),
        };
    }

    pub fn as<T>(this): *T?! {
        return match (this) {
            case .ssl(w) => w.as<T>(),
            case .tcp(w) => w.as<T>(),
        };
    }

    pub fn memoryCost(this): usize {
        return match (this) {
            case .ssl(w) => w.memoryCost(),
            case .tcp(w) => w.memoryCost(),
        };
    }

    pub fn close(this): void {
        match (this) {
            case .ssl(w) => w.close(),
            case .tcp(w) => w.close(),
        };
    }

    pub fn send(this, message: string, opcode: bun.uws.Opcode, compress: bool, fin: bool): bun.uws.SendStatus {
        return match (this) {
            case .ssl(w) => w.sendWithOptions(message, opcode, compress, fin),
            case .tcp(w) => w.sendWithOptions(message, opcode, compress, fin),
        };
    }

    pub fn sendLastFragment(this, message: string, compress: bool): bun.uws.SendStatus {
        return match (this) {
            case .ssl(w) => w.sendLastFragment(message, compress),
            case .tcp(w) => w.sendLastFragment(message, compress),
        };
    }

    pub fn end(this, code: i32, message: string): void {
        match (this) {
            case .ssl(w) => w.end(code, message),
            case .tcp(w) => w.end(code, message),
        };
    }

    pub fn cork<Ctx>(this, ctx: *Ctx, callback: fn(*Ctx): void): void {
        match (this) {
            case .ssl(w) => w.cork<Ctx>(ctx, callback),
            case .tcp(w) => w.cork<Ctx>(ctx, callback),
        };
    }

    pub fn subscribe(this, topic: string): bool {
        return match (this) {
            case .ssl(w) => w.subscribe(topic),
            case .tcp(w) => w.subscribe(topic),
        };
    }

    pub fn unsubscribe(this, topic: string): bool {
        return match (this) {
            case .ssl(w) => w.unsubscribe(topic),
            case .tcp(w) => w.unsubscribe(topic),
        };
    }

    pub fn isSubscribed(this, topic: string): bool {
        return match (this) {
            case .ssl(w) => w.isSubscribed(topic),
            case .tcp(w) => w.isSubscribed(topic),
        };
    }

    pub fn publish(this, topic: string, message: string, opcode: bun.uws.Opcode, compress: bool): bool {
        return match (this) {
            case .ssl(w) => w.publishWithOptions(topic, message, opcode, compress),
            case .tcp(w) => w.publishWithOptions(topic, message, opcode, compress),
        };
    }

    pub fn getBufferedAmount(this): usize {
        return match (this) {
            case .ssl(w) => w.getBufferedAmount() as usize,
            case .tcp(w) => w.getBufferedAmount() as usize,
        };
    }

    pub fn getRemoteAddress(this, buf: u8[]): u8[] {
        return match (this) {
            case .ssl(w) => w.getRemoteAddress(buf),
            case .tcp(w) => w.getRemoteAddress(buf),
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
    upgrade: @c ((ptr: *any!, res: *uws_res, req: *Request, ctx: *bun.uws.WebSocketUpgradeContext, id: usize): void)? = null,
    open: @c ((raw_ws: *RawWebSocket): void)? = null,
    message: @c ((raw_ws: *RawWebSocket, msg: [*]const u8!, len: usize, op: bun.uws.Opcode): void)? = null,
    drain: @c ((raw_ws: *RawWebSocket): void)? = null,
    ping: @c ((raw_ws: *RawWebSocket, msg: [*]const u8!, len: usize): void)? = null,
    pong: @c ((raw_ws: *RawWebSocket, msg: [*]const u8!, len: usize): void)? = null,
    close: @c ((raw_ws: *RawWebSocket, code: i32, msg: [*]const u8!, len: usize): void)? = null,

    pub static fn wrap<ServerType, Type, const ssl: bool>(behavior: WebSocketBehavior): WebSocketBehavior {
        let Wrap = struct {
            @c pub fn onOpen(raw_ws: *RawWebSocket): void {
                let ws = if (ssl) AnyWebSocket.ssl(raw_ws as *WebSocket<1>) else AnyWebSocket.tcp(raw_ws as *WebSocket<0>);
                let this = ws.as<Type>()!;
                Type.onOpen(this, ws);
            }

            @c pub fn onMessage(raw_ws: *RawWebSocket, message: [*]const u8!, length: usize, opcode: bun.uws.Opcode): void {
                let ws = if (ssl) AnyWebSocket.ssl(raw_ws as *WebSocket<1>) else AnyWebSocket.tcp(raw_ws as *WebSocket<0>);
                let this = ws.as<Type>()!;
                Type.onMessage(this, ws, if (length > 0) message[0..length] else "", opcode);
            }

            @c pub fn onDrain(raw_ws: *RawWebSocket): void {
                let ws = if (ssl) AnyWebSocket.ssl(raw_ws as *WebSocket<1>) else AnyWebSocket.tcp(raw_ws as *WebSocket<0>);
                let this = ws.as<Type>()!;
                Type.onDrain(this, ws);
            }

            @c pub fn onPing(raw_ws: *RawWebSocket, message: [*]const u8!, length: usize): void {
                let ws = if (ssl) AnyWebSocket.ssl(raw_ws as *WebSocket<1>) else AnyWebSocket.tcp(raw_ws as *WebSocket<0>);
                let this = ws.as<Type>()!;
                Type.onPing(this, ws, if (length > 0) message[0..length] else "");
            }

            @c pub fn onPong(raw_ws: *RawWebSocket, message: [*]const u8!, length: usize): void {
                let ws = if (ssl) AnyWebSocket.ssl(raw_ws as *WebSocket<1>) else AnyWebSocket.tcp(raw_ws as *WebSocket<0>);
                let this = ws.as<Type>()!;
                Type.onPong(this, ws, if (length > 0) message[0..length] else "");
            }

            @c pub fn onClose(raw_ws: *RawWebSocket, code: i32, message: [*]const u8!, length: usize): void {
                let ws = if (ssl) AnyWebSocket.ssl(raw_ws as *WebSocket<1>) else AnyWebSocket.tcp(raw_ws as *WebSocket<0>);
                let this = ws.as<Type>()!;
                Type.onClose(this, ws, code, if (length > 0 && message != null) message[0..length] else "");
            }

            @c pub fn onUpgrade(ptr: *any!, res: *uws_res, req: *Request, context: *bun.uws.WebSocketUpgradeContext, id: usize): void {
                ServerType.onWebSocketUpgrade(
                    ptr as *ServerType,
                    res as *bun.uws.Response<if (ssl) 1 else 0>,
                    req,
                    context,
                    id,
                );
            }
        };

        return WebSocketBehavior {
            compression = behavior.compression,
            maxPayloadLength = behavior.maxPayloadLength,
            idleTimeout = behavior.idleTimeout,
            maxBackpressure = behavior.maxBackpressure,
            closeOnBackpressureLimit = behavior.closeOnBackpressureLimit,
            resetIdleTimeoutOnSend = behavior.resetIdleTimeoutOnSend,
            sendPingsAutomatically = behavior.sendPingsAutomatically,
            maxLifetime = behavior.maxLifetime,
            upgrade = Wrap.onUpgrade,
            open = Wrap.onOpen,
            message = if (reflect.hasMethod(Type, "onMessage")) Wrap.onMessage else null,
            drain = if (reflect.hasMethod(Type, "onDrain")) Wrap.onDrain else null,
            ping = if (reflect.hasMethod(Type, "onPing")) Wrap.onPing else null,
            pong = if (reflect.hasMethod(Type, "onPong")) Wrap.onPong else null,
            close = Wrap.onClose,
        };
    }
}
