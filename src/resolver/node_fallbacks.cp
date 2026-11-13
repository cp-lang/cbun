import Fs from "./fs.zig";
import std from "std";
import { PackageJSON } from "./package_json.zig";
import bun from "bun";
const Environment = bun.Environment;
const logger = bun.logger;

pub const import_path = "/bun-vfs$$/node_modules/";

comptime {
    bun.assert(import_path.len % 8 == 0);
}

pub struct FallbackModule {
    path: Fs.Path,
    package_json: *const PackageJSON,
    code: *const fn () string,

    static fn createSourceCodeGetter(comptime code_path: string): *const fn () string {
        const Getter = struct {
            static fn get(): string {
                if (bun.Environment.codegen_embed) {
                    return @embedFile(code_path);
                }

                return bun.runtimeEmbedFile(.codegen, code_path);
            }
        };

        return Getter.get;
    }

    pub static fn init(comptime name: string): FallbackModule {
        @setEvalBranchQuota(99999);
        const version = "0.0.0-polyfill";
        const code_path = "node-fallbacks/" + name + ".js";
        return FallbackModule{
            path: Fs.Path.initWithNamespaceVirtual(import_path + name + "/index.js", "node", name),
            package_json: &PackageJSON{
                name: name,
                version: version,
                module_type: .esm,
                main_fields: undefined,
                browser_map: undefined,
                source: logger.Source.initPathString(import_path + name + "/package.json", ""),
                side_effects: .false,
            },
            code: createSourceCodeGetter(code_path),
        };
    }
}

pub const Map = bun.ComptimeStringMap(FallbackModule, [
    ["assert", FallbackModule.init("assert")],
    ["buffer", FallbackModule.init("buffer")],
    ["console", FallbackModule.init("console")],
    ["constants", FallbackModule.init("constants")],
    ["crypto", FallbackModule.init("crypto")],
    ["domain", FallbackModule.init("domain")],
    ["events", FallbackModule.init("events")],
    ["http", FallbackModule.init("http")],
    ["https", FallbackModule.init("https")],
    ["net", FallbackModule.init("net")],
    ["os", FallbackModule.init("os")],
    ["path", FallbackModule.init("path")],
    ["process", FallbackModule.init("process")],
    ["punycode", FallbackModule.init("punycode")],
    ["querystring", FallbackModule.init("querystring")],
    ["stream", FallbackModule.init("stream")],
    ["string_decoder", FallbackModule.init("string_decoder")],
    ["sys", FallbackModule.init("sys")],
    ["timers", FallbackModule.init("timers")],
    ["tty", FallbackModule.init("tty")],
    ["url", FallbackModule.init("url")],
    ["util", FallbackModule.init("util")],
    ["zlib", FallbackModule.init("zlib")],
]);

pub fn contentsFromPath(path: string): string? {
    if (Environment.allow_assert) {
        bun.assert(bun.strings.hasPrefixComptime(path, import_path));
    }

    let module_name = path[import_path.len..];
    module_name = module_name[0 .. std.mem.indexOfScalar(u8, module_name, '/') ?? module_name.len];

    if (mod := Map.get(module_name)) {
        return mod.code();
    }

    return null;
}
