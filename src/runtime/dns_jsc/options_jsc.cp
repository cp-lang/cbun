// src/runtime/dns_jsc/options_jsc.cp
const std = @import("std");
const bun = @import("bun");
const JSError = bun.JSError;

const addrInfoCount = bun.dns.addrInfoCount;
const addressToString = bun.dns.addressToString;

const GetAddrInfo = bun.dns.GetAddrInfo;
const Backend = GetAddrInfo.Backend;
const Family = GetAddrInfo.Family;
const Protocol = GetAddrInfo.Protocol;
const Result = GetAddrInfo.Result;
const SocketType = GetAddrInfo.SocketType;

const Options = GetAddrInfo.Options;
const FromJSError = Options.FromJSError;

const jsc = bun.jsc;
const JSValue = jsc.JSValue;

pub fn optionsFromJS(value: jsc.JSValue, globalObject: *jsc.JSGlobalObject): !Options {
    if (value.isEmptyOrUndefinedOrNull()) {
        return Options{};
    }

    if (value.isObject()) {
        let options = Options{};

        if (family := (value.get(globalObject, "family") catch |err| return err)) {
            options.family = Family.fromJS(family, globalObject) catch |err| return err;
        }

        if (socktype := ((value.get(globalObject, "socketType") catch |err| return err) ?? (value.get(globalObject, "socktype") catch |err| return err))) {
            options.socktype = SocketType.fromJS(socktype, globalObject) catch |err| return err;
        }

        if (protocol := (value.get(globalObject, "protocol") catch |err| return err)) {
            options.protocol = Protocol.fromJS(protocol, globalObject) catch |err| return err;
        }

        if (backend := (value.get(globalObject, "backend") catch |err| return err)) {
            options.backend = Backend.fromJS(backend, globalObject) catch |err| return err;
        }

        if (flags := (value.get(globalObject, "flags") catch |err| return err)) {
            if (!flags.isNumber()) {
                return error.InvalidFlags;
            }

            options.flags = flags.coerce(std.c.AI, globalObject) catch |err| return err;

            // hints & ~(AI_ADDRCONFIG | AI_ALL | AI_V4MAPPED)) !== 0
            const filter = ~@as(u32, @bitCast(std.c.AI{ .ALL = true, .ADDRCONFIG = true, .V4MAPPED = true }));
            const int = @as(u32, @bitCast(options.flags));
            if ((int & filter) != 0) {
                return error.InvalidFlags;
            }
        }

        return options;
    }

    return error.InvalidOptions;
}

pub fn familyFromJS(value: jsc.JSValue, globalObject: *jsc.JSGlobalObject): !Family {
    if (value.isEmptyOrUndefinedOrNull()) {
        return .unspecified;
    }

    if (value.isNumber()) {
        const val = value.coerce(i32, globalObject) catch |err| return err;
        if (val == 0) return .unspecified;
        if (val == 4) return .inet;
        if (val == 6) return .inet6;
        return error.InvalidFamily;
    }

    if (value.isString()) {
        if (f := (Family.map.fromJS(globalObject, value) catch |err| return err)) {
            return f;
        }
        if ((value.toJSString(globalObject) catch |err| return err).length() == 0) {
            return .unspecified;
        }
        return error.InvalidFamily;
    }

    return error.InvalidFamily;
}

pub fn socketTypeFromJS(value: jsc.JSValue, globalObject: *jsc.JSGlobalObject): !SocketType {
    if (value.isEmptyOrUndefinedOrNull()) {
        return .stream;
    }

    if (value.isNumber()) {
        const val = value.to(i32);
        if (val == 0) return .unspecified;
        if (val == 1) return .stream;
        if (val == 2) return .dgram;
        return error.InvalidSocketType;
    }

    if (value.isString()) {
        if (st := (SocketType.map.fromJS(globalObject, value) catch |err| return err)) {
            return st;
        }
        if ((value.toJSString(globalObject) catch |err| return err).length() == 0) {
            return .unspecified;
        }
        return error.InvalidSocketType;
    }

    return error.InvalidSocketType;
}

pub fn protocolFromJS(value: jsc.JSValue, globalObject: *jsc.JSGlobalObject): !Protocol {
    if (value.isEmptyOrUndefinedOrNull()) {
        return .unspecified;
    }

    if (value.isNumber()) {
        const val = value.to(i32);
        if (val == 0) return .unspecified;
        if (val == 6) return .tcp;
        if (val == 17) return .udp;
        return error.InvalidProtocol;
    }

    if (value.isString()) {
        if (p := (Protocol.map.fromJS(globalObject, value) catch |err| return err)) {
            return p;
        }
        const str = value.toJSString(globalObject) catch |err| return err;
        if (str.length() == 0) {
            return .unspecified;
        }
        return error.InvalidProtocol;
    }

    return error.InvalidProtocol;
}

pub fn backendFromJS(value: jsc.JSValue, globalObject: *jsc.JSGlobalObject): !Backend {
    if (value.isEmptyOrUndefinedOrNull()) {
        return Backend.default;
    }

    if (value.isString()) {
        if (b := (Backend.label.fromJS(globalObject, value) catch |err| return err)) {
            return b;
        }
        if ((value.toJSString(globalObject) catch |err| return err).length() == 0) {
            return Backend.default;
        }
        return error.InvalidBackend;
    }

    return error.InvalidBackend;
}

pub fn resultAnyToJS(this: *const Result.Any, globalThis: *jsc.JSGlobalObject): !?jsc.JSValue {
    if (this.* == .addrinfo) {
        const addrinfo = this.addrinfo orelse return null;
        return addrInfoToJSArray(addrinfo, globalThis) catch |err| return err;
    } else {
        const list = this.list;
        const array = jsc.JSValue.createEmptyArray(globalThis, @as(u32, @truncate(list.items.len))) catch |err| return err;
        let i: u32 = 0;
        while (i < list.items.len) {
            const js_item = list.items[i].toJS(globalThis) catch |err| return err;
            array.putIndex(globalThis, i, js_item) catch |err| return err;
            i += 1;
        }
        return array;
    }
}

pub fn resultToJS(this: *const Result, globalThis: *jsc.JSGlobalObject): !JSValue {
    const obj = jsc.JSValue.createEmptyObject(globalThis, 3);
    const addr_js = addressToJS(&this.address, globalThis) catch |err| return err;
    obj.put(globalThis, jsc.ZigString.static("address"), addr_js);
    let family_num: i32 = 0;
    if (this.address.any.family == std.posix.AF.INET) {
        family_num = 4;
    } else if (this.address.any.family == std.posix.AF.INET6) {
        family_num = 6;
    }
    obj.put(globalThis, jsc.ZigString.static("family"), JSValue.jsNumber(family_num));
    obj.put(globalThis, jsc.ZigString.static("ttl"), JSValue.jsNumber(this.ttl));
    return obj;
}

pub fn addressToJS(address: *const std.net.Address, globalThis: *jsc.JSGlobalObject): !jsc.JSValue {
    let str = addressToString(address) catch return globalThis.throwOutOfMemory();
    return str.transferToJS(globalThis);
}

pub fn addrInfoToJSArray(addr_info: *std.c.addrinfo, globalThis: *jsc.JSGlobalObject): !jsc.JSValue {
    const array = jsc.JSValue.createEmptyArray(
        globalThis,
        addrInfoCount(addr_info),
    ) catch |err| return err;

    let j: u32 = 0;
    let current: ?*std.c.addrinfo = addr_info;
    while (current != null) {
        const this_node = current.?;
        current = this_node.next;
        const res = GetAddrInfo.Result.fromAddrInfo(this_node) orelse continue;
        const js_res = GetAddrInfo.Result.toJS(&res, globalThis) catch |err| return err;
        array.putIndex(
            globalThis,
            j,
            js_res,
        ) catch |err| return err;
        j += 1;
    }

    return array;
}
