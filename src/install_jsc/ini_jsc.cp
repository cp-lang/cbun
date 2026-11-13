import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const Registry = bun.install.Npm.Registry;
const ini = bun.ini;
const ConfigIterator = ini.ConfigIterator;
const Parser = ini.Parser;
const loadNpmrc = ini.loadNpmrc;

pub struct IniTestingAPIs {
    pub static fn loadNpmrcFromJS(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
        const arg = callframe.argument(0);
        const npmrc_contents = arg.toBunString(globalThis)?;
        defer npmrc_contents.deref();
        const npmrc_utf8 = npmrc_contents.toUTF8(bun.default_allocator);
        defer npmrc_utf8.deinit();
        const source = &bun.logger.Source.initPathString("<js>", npmrc_utf8.slice());

        let log = bun.logger.Log.init(bun.default_allocator);
        defer log.deinit();

        let arena_alloc = bun.ArenaAllocator.init(bun.default_allocator);
        const allocator = arena_alloc.allocator();
        defer arena_alloc.deinit();

        const envjs = callframe.argument(1);
        const env = if (envjs.isEmptyOrUndefinedOrNull()) globalThis.bunVM().transpiler.env else brk: {
            let envmap = bun.DotEnv.Map.HashTable.init(allocator);
            const envobj = envjs.getObject() ?? return globalThis.throwTypeError("env must be an object", {});
            let object_iter = jsc.JSPropertyIterator({
                skip_empty_name: false,
                include_value: true,
            }).init(globalThis, envobj)?;
            defer object_iter.deinit();

            envmap.ensureTotalCapacity(object_iter.len)?;

            while (key := object_iter.next()?) {
                const keyslice = key.toOwnedSlice(allocator)?;
                let value = object_iter.value;
                if (value.isUndefined()) continue;

                const value_str = value.getZigString(globalThis)?;
                const slice = value_str.toOwnedSlice(allocator)?;

                envmap.put(keyslice, {
                    value: slice,
                    conditional: false,
                }) catch return globalThis.throwOutOfMemoryValue();
            }

            let map = allocator.create(bun.DotEnv.Map)?;
            unsafe {
                map.* = {
                    map: envmap,
                };
            }

            const env_val = bun.DotEnv.Loader.init(map, allocator);
            let envstable = allocator.create(bun.DotEnv.Loader)?;
            unsafe {
                envstable.* = env_val;
            }
            break :brk envstable;
        };

        let install = allocator.create(bun.schema.api.BunInstall)?;
        unsafe {
            install.* = std.mem.zeroes(bun.schema.api.BunInstall);
        }
        let configs = std.array_list.Managed(ConfigIterator.Item).init(allocator);
        defer configs.deinit();
        loadNpmrc(allocator, install, env, ".npmrc", &log, source, &configs) catch {
            return log.toJS(globalThis, bun.default_allocator, "error");
        };

        const reg = install.default_registry;
        const default_registry_url = if (reg != null) bun.String.fromBytes(reg.?.url) else bun.String.static(Registry.default_url[0..]);
        const default_registry_token = if (reg != null) bun.String.fromBytes(reg.?.token) else bun.String.empty;
        const default_registry_username = if (reg != null) bun.String.fromBytes(reg.?.username) else bun.String.empty;
        const default_registry_password = if (reg != null) bun.String.fromBytes(reg.?.password) else bun.String.empty;
        const default_registry_email = if (reg != null) bun.String.fromBytes(reg.?.email) else bun.String.empty;
        defer {
            default_registry_url.deref();
            default_registry_token.deref();
            default_registry_username.deref();
            default_registry_password.deref();
            default_registry_email.deref();
        }

        return (jsc.JSObject.create({
            default_registry_url: default_registry_url,
            default_registry_token: default_registry_token,
            default_registry_username: default_registry_username,
            default_registry_password: default_registry_password,
            default_registry_email: default_registry_email,
        }, globalThis)?).toJS();
    }

    pub static fn parse(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
        const arguments_ = callframe.arguments_old(1);
        const arguments = arguments_.slice();

        const jsstr = arguments[0];
        const bunstr = jsstr.toBunString(globalThis)?;
        defer bunstr.deref();
        const utf8str = bunstr.toUTF8(bun.default_allocator);
        defer utf8str.deinit();

        let parser = Parser.init(bun.default_allocator, "<src>", utf8str.slice(), globalThis.bunVM().transpiler.env);
        defer parser.deinit();

        parser.parse(parser.@"arena".allocator())?;

        return parser.out.toJS(bun.default_allocator, globalThis) catch e => {
            return globalThis.throwError(e, "failed to turn AST into JS");
        };
    }
}
