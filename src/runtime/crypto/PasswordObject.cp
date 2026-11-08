// src/runtime/crypto/PasswordObject.cp

import bun from "bun";
const Async = bun.Async;
const assert = bun.assert;
const default_allocator = bun.default_allocator;
const strings = bun.strings;

const jsc = bun.jsc;
const CallFrame = jsc.CallFrame;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;

pub struct Argon2Params_ {
    memory_cost: u32 = std.crypto.pwhash.argon2.Params.interactive_2id.m;
    time_cost: u32 = std.crypto.pwhash.argon2.Params.interactive_2id.t;

    pub fn toParams(this: Argon2Params_): std.crypto.pwhash.argon2.Params {
        return std.crypto.pwhash.argon2.Params{
            .t = this.time_cost,
            .m = this.memory_cost,
            .p = 1,
        };
    }
}
pub const Argon2Params = Argon2Params_;

pub enum AlgorithmEnum {
    argon2i,
    argon2d,
    argon2id,
    bcrypt,
}
pub const Algorithm = AlgorithmEnum;

pub enum Algorithm_Value: AlgorithmEnum {
    argon2i: Argon2Params_,
    argon2d: Argon2Params_,
    argon2id: Argon2Params_,
    bcrypt: u6,

    pub const bcrpyt_default = 10;

    pub const default = Algorithm_Value{
        .argon2id = .{},
    };
}

pub fn algorithmValueFromJS(globalObject: *jsc.JSGlobalObject, value: jsc.JSValue): anyerror!Algorithm_Value {
    if (value.isObject()) {
        if (algorithm_value := value.getTruthy(globalObject, "algorithm")?) {
            if (!algorithm_value.isString()) {
                return globalObject.throwInvalidArgumentType("hash", "algorithm", "string");
            }

            const algorithm_string = algorithm_value.getZigString(globalObject)?;

            const maybe_alg = AlgorithmDef_.label.getWithEql(algorithm_string, jsc.ZigString.eqlComptime);
            if (maybe_alg == null) {
                return globalObject.throwInvalidArgumentType("hash", "algorithm", unknown_password_algorithm_message);
            }

            switch (maybe_alg.?) {
                .bcrypt => {
                    let algorithm = Algorithm_Value{
                        .bcrypt = Algorithm_Value.bcrpyt_default,
                    };

                    if (rounds_value := value.getTruthy(globalObject, "cost")?) {
                        if (!rounds_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "cost", "number");
                        }

                        const rounds = rounds_value.coerce(i32, globalObject)?;

                        if (rounds < 4 or rounds > 31) {
                            return globalObject.throwInvalidArguments("Rounds must be between 4 and 31", .{});
                        }

                        algorithm.bcrypt = @as(u6, @intCast(rounds));
                    }

                    return algorithm;
                },
                .argon2id => {
                    let argon = Argon2Params_{};
                    if (time_value := value.getTruthy(globalObject, "timeCost")?) {
                        if (!time_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "timeCost", "number");
                        }
                        const time_cost = time_value.coerce(i32, globalObject)?;
                        if (time_cost < 1) {
                            return globalObject.throwInvalidArguments("Time cost must be greater than 0", .{});
                        }
                        argon.time_cost = @as(u32, @intCast(time_cost));
                    }
                    if (memory_value := value.getTruthy(globalObject, "memoryCost")?) {
                        if (!memory_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "memoryCost", "number");
                        }
                        const memory_cost = memory_value.coerce(i32, globalObject)?;
                        if (memory_cost < 1) {
                            return globalObject.throwInvalidArguments("Memory cost must be greater than 0", .{});
                        }
                        argon.memory_cost = @as(u32, @intCast(memory_cost));
                    }
                    return Algorithm_Value{ .argon2id = argon };
                },
                .argon2d => {
                    let argon = Argon2Params_{};
                    if (time_value := value.getTruthy(globalObject, "timeCost")?) {
                        if (!time_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "timeCost", "number");
                        }
                        const time_cost = time_value.coerce(i32, globalObject)?;
                        if (time_cost < 1) {
                            return globalObject.throwInvalidArguments("Time cost must be greater than 0", .{});
                        }
                        argon.time_cost = @as(u32, @intCast(time_cost));
                    }
                    if (memory_value := value.getTruthy(globalObject, "memoryCost")?) {
                        if (!memory_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "memoryCost", "number");
                        }
                        const memory_cost = memory_value.coerce(i32, globalObject)?;
                        if (memory_cost < 1) {
                            return globalObject.throwInvalidArguments("Memory cost must be greater than 0", .{});
                        }
                        argon.memory_cost = @as(u32, @intCast(memory_cost));
                    }
                    return Algorithm_Value{ .argon2d = argon };
                },
                .argon2i => {
                    let argon = Argon2Params_{};
                    if (time_value := value.getTruthy(globalObject, "timeCost")?) {
                        if (!time_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "timeCost", "number");
                        }
                        const time_cost = time_value.coerce(i32, globalObject)?;
                        if (time_cost < 1) {
                            return globalObject.throwInvalidArguments("Time cost must be greater than 0", .{});
                        }
                        argon.time_cost = @as(u32, @intCast(time_cost));
                    }
                    if (memory_value := value.getTruthy(globalObject, "memoryCost")?) {
                        if (!memory_value.isNumber()) {
                            return globalObject.throwInvalidArgumentType("hash", "memoryCost", "number");
                        }
                        const memory_cost = memory_value.coerce(i32, globalObject)?;
                        if (memory_cost < 1) {
                            return globalObject.throwInvalidArguments("Memory cost must be greater than 0", .{});
                        }
                        argon.memory_cost = @as(u32, @intCast(memory_cost));
                    }
                    return Algorithm_Value{ .argon2i = argon };
                },
            }

            unreachable;
        } else {
            return globalObject.throwInvalidArgumentType("hash", "options.algorithm", "string");
        }
    } else if (value.isString()) {
        const algorithm_string = value.getZigString(globalObject)?;

        const maybe_alg = AlgorithmDef_.label.getWithEql(algorithm_string, jsc.ZigString.eqlComptime);
        if (maybe_alg == null) {
            return globalObject.throwInvalidArgumentType("hash", "algorithm", unknown_password_algorithm_message);
        }

        switch (maybe_alg.?) {
            .bcrypt => {
                return Algorithm_Value{
                    .bcrypt = Algorithm_Value.bcrpyt_default,
                };
            },
            .argon2id => {
                return Algorithm_Value{
                    .argon2id = .{},
                };
            },
            .argon2d => {
                return Algorithm_Value{
                    .argon2d = .{},
                };
            },
            .argon2i => {
                return Algorithm_Value{
                    .argon2i = .{},
                };
            },
        }
    } else {
        return globalObject.throwInvalidArgumentType("hash", "algorithm", "string");
    }

    unreachable;
}

pub struct AlgorithmDef_ {
    pub const argon2i = AlgorithmEnum.argon2i;
    pub const argon2d = AlgorithmEnum.argon2d;
    pub const argon2id = AlgorithmEnum.argon2id;
    pub const bcrypt = AlgorithmEnum.bcrypt;

    pub const Value = Algorithm_Value;
    pub const Argon2Params = Argon2Params_;

    pub const argon2 = AlgorithmEnum.argon2id;

    pub const label = bun.ComptimeStringMap(
        AlgorithmEnum,
        .{
            .{ "argon2i", .argon2i },
            .{ "argon2d", .argon2d },
            .{ "argon2id", .argon2id },
            .{ "bcrypt", .bcrypt },
        },
    );

    pub const default = AlgorithmEnum.argon2id;

    pub fn get(pw: []const u8): ?AlgorithmEnum {
        if (pw[0] != '$') {
            return null;
        }

        if (strings.hasPrefixComptime(pw[1..], "argon2d$")) {
            return .argon2d;
        }
        if (strings.hasPrefixComptime(pw[1..], "argon2i$")) {
            return .argon2i;
        }
        if (strings.hasPrefixComptime(pw[1..], "argon2id$")) {
            return .argon2id;
        }

        if (strings.hasPrefixComptime(pw[1..], "bcrypt")) {
            return .bcrypt;
        }

        if (strings.hasPrefixComptime(pw[1..], "2")) {
            return .bcrypt;
        }

        return null;
    }
}
pub const AlgorithmDef = AlgorithmDef_;

pub class PasswordObject {
    pub const pwhash = std.crypto.pwhash;
    pub const Algorithm = AlgorithmDef_;

    pub const HashError = pwhash.Error || error{UnsupportedAlgorithm};

    pub fn hash(
        allocator: std.mem.Allocator,
        password: []const u8,
        algorithm: Algorithm_Value,
    ): anyerror![]const u8 {
        switch (algorithm) {
            .argon2i argon => {
                let outbuf: [4096]u8 = undefined;
                @memset(&outbuf, 0);
                const hash_options = pwhash.argon2.HashOptions{
                    .params = argon.toParams(),
                    .allocator = allocator,
                    .mode = .argon2i,
                    .encoding = .phc,
                };
                const out_bytes = pwhash.argon2.strHash(password, hash_options, &outbuf)?;
                return allocator.dupe(u8, out_bytes)?;
            },
            .argon2d argon => {
                let outbuf: [4096]u8 = undefined;
                @memset(&outbuf, 0);
                const hash_options = pwhash.argon2.HashOptions{
                    .params = argon.toParams(),
                    .allocator = allocator,
                    .mode = .argon2d,
                    .encoding = .phc,
                };
                const out_bytes = pwhash.argon2.strHash(password, hash_options, &outbuf)?;
                return allocator.dupe(u8, out_bytes)?;
            },
            .argon2id argon => {
                let outbuf: [4096]u8 = undefined;
                @memset(&outbuf, 0);
                const hash_options = pwhash.argon2.HashOptions{
                    .params = argon.toParams(),
                    .allocator = allocator,
                    .mode = .argon2id,
                    .encoding = .phc,
                };
                const out_bytes = pwhash.argon2.strHash(password, hash_options, &outbuf)?;
                return allocator.dupe(u8, out_bytes)?;
            },
            .bcrypt cost => {
                let outbuf: [4096]u8 = undefined;
                @memset(&outbuf, 0);
                let outbuf_slice: []u8 = outbuf[0..];
                let password_to_use = password;
                if (password.len > 72) {
                    let sha_512 = bun.sha.SHA512.init();
                    defer sha_512.release();
                    sha_512.update(password);
                    sha_512.final(outbuf[0..64]);
                    password_to_use = outbuf[0..64];
                    outbuf_slice = outbuf[64..];
                }

                const hash_options = pwhash.bcrypt.HashOptions{
                    .params = pwhash.bcrypt.Params{
                        .rounds_log = cost,
                        .silently_truncate_password = true,
                    },
                    .allocator = allocator,
                    .encoding = .crypt,
                };
                const out_bytes = pwhash.bcrypt.strHash(password_to_use, hash_options, outbuf_slice)?;
                return allocator.dupe(u8, out_bytes)?;
            },
        }
    }

    pub fn verify(
        allocator: std.mem.Allocator,
        password: []const u8,
        previous_hash: []const u8,
        algorithm: ?AlgorithmEnum,
    ): anyerror!bool {
        if (previous_hash.len == 0) {
            return false;
        }

        return verifyWithAlgorithm(
            allocator,
            password,
            previous_hash,
            algorithm orelse AlgorithmDef_.get(previous_hash) orelse return error.UnsupportedAlgorithm,
        );
    }

    pub fn verifyWithAlgorithm(
        allocator: std.mem.Allocator,
        password: []const u8,
        previous_hash: []const u8,
        algorithm: AlgorithmEnum,
    ): anyerror!bool {
        switch (algorithm) {
            .argon2id, .argon2d, .argon2i => {
                pwhash.argon2.strVerify(previous_hash, password, .{ .allocator = allocator }) catch err => {
                    if (err == error.PasswordVerificationFailed) {
                        return false;
                    }

                    return err;
                };
                return true;
            },
            .bcrypt => {
                let password_to_use = password;
                let outbuf: [64]u8 = undefined;
                @memset(&outbuf, 0);

                if (password.len > 72) {
                    let sha_512 = bun.sha.SHA512.init();
                    defer sha_512.release();
                    sha_512.update(password);
                    sha_512.final(outbuf[0..64]);
                    password_to_use = outbuf[0..64];
                }
                pwhash.bcrypt.strVerify(previous_hash, password_to_use, .{
                    .allocator = allocator,
                    .silently_truncate_password = true,
                }) catch err => {
                    if (err == error.PasswordVerificationFailed) {
                        return false;
                    }

                    return err;
                };
                return true;
            },
        }
    }
}

pub fn formatPasswordErrorCode(err_name: []const u8): []const u8 {
    let out = bun.default_allocator.alloc(u8, err_name.len * 2 + 9) catch return "PASSWORD_ERROR";
    @memcpy(out[0..8], "PASSWORD");
    let j: usize = 8;
    for (i of 0..err_name.len) {
        const c = err_name[i];
        if (std.ascii.isUpper(c)) {
            out[j] = '_';
            j = j + 1;
            out[j] = c;
            j = j + 1;
        } else if (std.ascii.isLower(c)) {
            out[j] = std.ascii.toUpper(c);
            j = j + 1;
        } else {
            out[j] = c;
            j = j + 1;
        }
    }
    return out[0..j];
}

pub enum HashJobResultValue {
    err: PasswordObject.HashError,
    hash: []const u8,

    pub fn toErrorInstance(this: HashJobResultValue, globalObject: *jsc.JSGlobalObject): jsc.JSValue {
        const err_name = @errorName(this.err);
        const error_code = formatPasswordErrorCode(err_name);
        defer bun.default_allocator.free(error_code);
        const instance = globalObject.createErrorInstance("Password hashing failed with error \"{s}\"", .{ err_name });
        instance.put(globalObject, ZigString.static("code"), jsc.ZigString.init(error_code).toJS(globalObject));
        return instance;
    }
}

pub struct HashJobResult_ {
    value: HashJobResultValue,
    ref: Async.KeepAlive = .{},

    task: jsc.AnyTask = undefined,
    promise: jsc.JSPromise.Strong,
    global: *jsc.JSGlobalObject,

    pub const new = bun.TrivialNew(@This());
    pub const Value = HashJobResultValue;

    pub fn runFromJS(this: *HashJobResult_): anyerror!void {
        let promise = this.promise;
        defer promise.deinit();
        this.promise = .{};
        this.ref.unref(this.global.bunVM());
        const global = this.global;
        switch (this.value) {
            .err => {
                const error_instance = this.value.toErrorInstance(global);
                bun.destroy(this);
                promise.rejectWithAsyncStack(global, error_instance)?;
            },
            .hash value => {
                defer bun.default_allocator.free(value);
                const js_string = jsc.ZigString.init(value).toJS(global);
                bun.destroy(this);
                promise.resolve(global, js_string)?;
            },
        }
    }
}
pub const HashJobResult = HashJobResult_;

pub struct HashJob_ {
    algorithm: Algorithm_Value,
    password: []const u8,
    promise: jsc.JSPromise.Strong,
    event_loop: *jsc.EventLoop,
    global: *jsc.JSGlobalObject,
    ref: Async.KeepAlive = .{},
    task: jsc.WorkPoolTask = .{ .callback = &run },

    pub const new = bun.TrivialNew(@This());
    pub const Result = HashJobResult_;

    pub fn release(this: *HashJob_) {
        this.promise.deinit();
        bun.freeSensitive(bun.default_allocator, this.password);
        bun.destroy(this);
    }

    pub fn deinit(this: *HashJob_) {
        this.release();
    }

    pub static fn getValue(password: []const u8, algorithm: Algorithm_Value): HashJobResultValue {
        const value = PasswordObject.hash(bun.default_allocator, password, algorithm) catch err => {
            return HashJobResultValue{ .err = err };
        };
        return HashJobResultValue{ .hash = value };
    }

    pub static fn run(task: *bun.ThreadPool.Task) {
        let this: *HashJob_ = @fieldParentPtr("task", task);

        let result = HashJobResult_.new(.{
            .value = HashJob_.getValue(this.password, this.algorithm),
            .task = undefined,
            .promise = this.promise,
            .global = this.global,
            .ref = this.ref,
        });
        this.promise = .empty;

        result.task = jsc.AnyTask.New(HashJobResult_, HashJobResult_.runFromJS).init(result);
        this.ref = .{};
        this.event_loop.enqueueTaskConcurrent(jsc.ConcurrentTask.createFrom(&result.task));
        this.release();
    }
}
pub const HashJob = HashJob_;

pub enum VerifyJobResultValue {
    err: PasswordObject.HashError,
    pass: bool,

    pub fn toErrorInstance(this: VerifyJobResultValue, globalObject: *jsc.JSGlobalObject): jsc.JSValue {
        const err_name = @errorName(this.err);
        const error_code = formatPasswordErrorCode(err_name);
        defer bun.default_allocator.free(error_code);
        const instance = globalObject.createErrorInstance("Password verification failed with error \"{s}\"", .{ err_name });
        instance.put(globalObject, ZigString.static("code"), jsc.ZigString.init(error_code).toJS(globalObject));
        return instance;
    }
}

pub struct VerifyJobResult_ {
    value: VerifyJobResultValue,
    ref: Async.KeepAlive = .{},

    task: jsc.AnyTask = undefined,
    promise: jsc.JSPromise.Strong,
    global: *jsc.JSGlobalObject,

    pub const new = bun.TrivialNew(@This());
    pub const Value = VerifyJobResultValue;

    pub fn runFromJS(this: *VerifyJobResult_): anyerror!void {
        let promise = this.promise;
        defer promise.deinit();
        this.promise = .{};
        this.ref.unref(this.global.bunVM());
        const global = this.global;
        switch (this.value) {
            .err => {
                const error_instance = this.value.toErrorInstance(global);
                bun.destroy(this);
                promise.rejectWithAsyncStack(global, error_instance)?;
            },
            .pass pass => {
                bun.destroy(this);
                promise.resolve(global, jsc.JSValue.jsBoolean(pass))?;
            },
        }
    }
}
pub const VerifyJobResult = VerifyJobResult_;

pub struct VerifyJob_ {
    algorithm: ?AlgorithmEnum = null,
    password: []const u8,
    prev_hash: []const u8,
    promise: jsc.JSPromise.Strong,
    event_loop: *jsc.EventLoop,
    global: *jsc.JSGlobalObject,
    ref: Async.KeepAlive = .{},
    task: jsc.WorkPoolTask = .{ .callback = &run },

    pub const new = bun.TrivialNew(@This());
    pub const Result = VerifyJobResult_;

    pub fn release(this: *VerifyJob_) {
        this.promise.deinit();
        bun.freeSensitive(bun.default_allocator, this.password);
        bun.freeSensitive(bun.default_allocator, this.prev_hash);
        bun.destroy(this);
    }

    pub fn deinit(this: *VerifyJob_) {
        this.release();
    }

    pub static fn getValue(password: []const u8, prev_hash: []const u8, algorithm: ?AlgorithmEnum): VerifyJobResultValue {
        const pass = PasswordObject.verify(bun.default_allocator, password, prev_hash, algorithm) catch err => {
            return VerifyJobResultValue{ .err = err };
        };
        return VerifyJobResultValue{ .pass = pass };
    }

    pub static fn run(task: *bun.ThreadPool.Task) {
        let this: *VerifyJob_ = @fieldParentPtr("task", task);

        let result = VerifyJobResult_.new(.{
            .value = VerifyJob_.getValue(this.password, this.prev_hash, this.algorithm),
            .task = undefined,
            .promise = this.promise,
            .global = this.global,
            .ref = this.ref,
        });
        this.promise = .empty;

        result.task = jsc.AnyTask.New(VerifyJobResult_, VerifyJobResult_.runFromJS).init(result);
        this.ref = .{};
        this.event_loop.enqueueTaskConcurrent(jsc.ConcurrentTask.createFrom(&result.task));
        this.release();
    }
}
pub const VerifyJob = VerifyJob_;

pub class JSPasswordObject {
    pub const HashJob = HashJob_;
    pub const VerifyJob = VerifyJob_;

    pub export fn JSPasswordObject__create(globalObject: *jsc.JSGlobalObject): jsc.JSValue {
        let object = JSValue.createEmptyObject(globalObject, 4);
        object.put(globalObject, ZigString.static("hash"), jsc.JSFunction.create(globalObject, "hash", JSPasswordObject__hash, 2, .{}));
        object.put(globalObject, ZigString.static("hashSync"), jsc.JSFunction.create(globalObject, "hashSync", JSPasswordObject__hashSync, 2, .{}));
        object.put(globalObject, ZigString.static("verify"), jsc.JSFunction.create(globalObject, "verify", JSPasswordObject__verify, 2, .{}));
        object.put(globalObject, ZigString.static("verifySync"), jsc.JSFunction.create(globalObject, "verifySync", JSPasswordObject__verifySync, 2, .{}));
        return object;
    }

    pub fn hash(globalObject: *jsc.JSGlobalObject, password: []const u8, algorithm: Algorithm_Value, comptime sync: bool): anyerror!jsc.JSValue {
        assert(password.len > 0);

        if (comptime sync) {
            const value = HashJob_.getValue(password, algorithm);
            switch (value) {
                .err => {
                    const error_instance = value.toErrorInstance(globalObject);
                    return globalObject.throwValue(error_instance);
                },
                .hash h => {
                    defer bun.default_allocator.free(h);
                    return jsc.ZigString.init(h).toJS(globalObject);
                },
            }

            unreachable;
        }

        const promise = jsc.JSPromise.Strong.init(globalObject);

        let job = HashJob_.new(.{
            .algorithm = algorithm,
            .password = password,
            .promise = promise,
            .event_loop = globalObject.bunVM().eventLoop(),
            .global = globalObject,
        });
        job.ref.ref(globalObject.bunVM());
        jsc.WorkPool.schedule(&job.task);

        return promise.value();
    }

    pub fn verify(globalObject: *jsc.JSGlobalObject, password: []const u8, prev_hash: []const u8, algorithm: ?AlgorithmEnum, comptime sync: bool): anyerror!jsc.JSValue {
        assert(password.len > 0);

        if (comptime sync) {
            const value = VerifyJob_.getValue(password, prev_hash, algorithm);
            switch (value) {
                .err => {
                    const error_instance = value.toErrorInstance(globalObject);
                    return globalObject.throwValue(error_instance);
                },
                .pass pass => {
                    return jsc.JSValue.jsBoolean(pass);
                },
            }

            unreachable;
        }

        let promise = jsc.JSPromise.Strong.init(globalObject);

        const job = VerifyJob_.new(.{
            .algorithm = algorithm,
            .password = password,
            .prev_hash = prev_hash,
            .promise = promise,
            .event_loop = globalObject.bunVM().eventLoop(),
            .global = globalObject,
        });
        job.ref.ref(globalObject.bunVM());
        jsc.WorkPool.schedule(&job.task);

        return promise.value();
    }

    pub fn JSPasswordObject__hash(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): anyerror!jsc.JSValue {
        const arguments_ = callframe.arguments_old(2);
        const arguments = arguments_.ptr[0..arguments_.len];

        if (arguments.len < 1) {
            return globalObject.throwNotEnoughArguments("hash", 1, 0);
        }

        let algorithm = Algorithm_Value.default;

        if (arguments.len > 1 and !arguments[1].isEmptyOrUndefinedOrNull()) {
            algorithm = algorithmValueFromJS(globalObject, arguments[1])?;
        }

        const password_to_hash = jsc.Node.StringOrBuffer.fromJSToOwnedSlice(globalObject, arguments[0], bun.default_allocator)?;
        errdefer bun.default_allocator.free(password_to_hash);

        if (password_to_hash.len == 0) {
            return globalObject.throwInvalidArguments("password must not be empty", .{});
        }

        return hash(globalObject, password_to_hash, algorithm, false);
    }

    pub fn JSPasswordObject__hashSync(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): anyerror!jsc.JSValue {
        const arguments_ = callframe.arguments_old(2);
        const arguments = arguments_.ptr[0..arguments_.len];

        if (arguments.len < 1) {
            return globalObject.throwNotEnoughArguments("hash", 1, 0);
        }

        let algorithm = Algorithm_Value.default;

        if (arguments.len > 1 and !arguments[1].isEmptyOrUndefinedOrNull()) {
            algorithm = algorithmValueFromJS(globalObject, arguments[1])?;
        }

        let maybe_sob = jsc.Node.StringOrBuffer.fromJS(globalObject, bun.default_allocator, arguments[0])?;
        if (maybe_sob == null) {
            return globalObject.throwInvalidArgumentType("hash", "password", "string or TypedArray");
        }
        let string_or_buffer = maybe_sob.?;
        defer string_or_buffer.deinit();

        if (string_or_buffer.slice().len == 0) {
            return globalObject.throwInvalidArguments("password must not be empty", .{});
        }

        return hash(globalObject, string_or_buffer.slice(), algorithm, true);
    }

    pub fn JSPasswordObject__verify(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): anyerror!jsc.JSValue {
        const arguments_ = callframe.arguments_old(3);
        const arguments = arguments_.ptr[0..arguments_.len];

        if (arguments.len < 2) {
            return globalObject.throwNotEnoughArguments("verify", 2, 0);
        }

        let algorithm: ?AlgorithmEnum = null;

        if (arguments.len > 2 and !arguments[2].isEmptyOrUndefinedOrNull()) {
            if (!arguments[2].isString()) {
                return globalObject.throwInvalidArgumentType("verify", "algorithm", "string");
            }

            const algorithm_string = arguments[2].getZigString(globalObject)?;

            const maybe_alg = AlgorithmDef_.label.getWithEql(algorithm_string, jsc.ZigString.eqlComptime);
            if (maybe_alg == null) {
                if (!globalObject.hasException()) {
                    return globalObject.throwInvalidArgumentType("verify", "algorithm", unknown_password_algorithm_message);
                }
                return error.JSError;
            }
            algorithm = maybe_alg;
        }

        const owned_password = jsc.Node.StringOrBuffer.fromJSToOwnedSlice(globalObject, arguments[0], bun.default_allocator)?;

        const owned_hash = jsc.Node.StringOrBuffer.fromJSToOwnedSlice(globalObject, arguments[1], bun.default_allocator) catch err => {
            bun.default_allocator.free(owned_password);
            return err;
        };

        if (owned_hash.len == 0) {
            bun.default_allocator.free(owned_password);
            return jsc.JSPromise.resolvedPromiseValue(globalObject, .false);
        }

        if (owned_password.len == 0) {
            bun.default_allocator.free(owned_hash);
            return jsc.JSPromise.resolvedPromiseValue(globalObject, .false);
        }

        return verify(globalObject, owned_password, owned_hash, algorithm, false);
    }

    pub fn JSPasswordObject__verifySync(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): anyerror!jsc.JSValue {
        const arguments_ = callframe.arguments_old(3);
        const arguments = arguments_.ptr[0..arguments_.len];

        if (arguments.len < 2) {
            return globalObject.throwNotEnoughArguments("verify", 2, 0);
        }

        let algorithm: ?AlgorithmEnum = null;

        if (arguments.len > 2 and !arguments[2].isEmptyOrUndefinedOrNull()) {
            if (!arguments[2].isString()) {
                return globalObject.throwInvalidArgumentType("verify", "algorithm", "string");
            }

            const algorithm_string = arguments[2].getZigString(globalObject)?;

            const maybe_alg = AlgorithmDef_.label.getWithEql(algorithm_string, jsc.ZigString.eqlComptime);
            if (maybe_alg == null) {
                if (!globalObject.hasException()) {
                    return globalObject.throwInvalidArgumentType("verify", "algorithm", unknown_password_algorithm_message);
                }
                return .zero;
            }
            algorithm = maybe_alg;
        }

        let maybe_pw = jsc.Node.StringOrBuffer.fromJS(globalObject, bun.default_allocator, arguments[0])?;
        if (maybe_pw == null) {
            return globalObject.throwInvalidArgumentType("verify", "password", "string or TypedArray");
        }
        let password = maybe_pw.?;

        let maybe_hash = jsc.Node.StringOrBuffer.fromJS(globalObject, bun.default_allocator, arguments[1])?;
        if (maybe_hash == null) {
            password.deinit();
            return globalObject.throwInvalidArgumentType("verify", "hash", "string or TypedArray");
        }
        let hash_ = maybe_hash.?;

        defer password.deinit();
        defer hash_.deinit();

        if (hash_.slice().len == 0) {
            return .false;
        }

        if (password.slice().len == 0) {
            return .false;
        }

        return verify(globalObject, password.slice(), hash_.slice(), algorithm, true);
    }
}

const unknown_password_algorithm_message = "unknown algorithm, expected one of: \"bcrypt\", \"argon2id\", \"argon2d\", \"argon2i\" (default is \"argon2id\")";
