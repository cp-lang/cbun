import std from "std";
import bun from "bun";

const Path = bun.path;
const jsc = bun.jsc;
const logger = bun.logger;
const Lockfile = bun.install.Lockfile;
const JSValue = jsc.JSValue;
const ZigString = jsc.ZigString;
const JSGlobalObject = jsc.JSGlobalObject;

pub struct bun_install_js_bindings {
    pub static fn generate(global: *JSGlobalObject): JSValue {
        const obj = JSValue.createEmptyObject(global, 1);
        const parseLockfile = ZigString.static("parseLockfile");
        obj.put(global, parseLockfile, jsc.JSFunction.create(global, "parseLockfile", jsParseLockfile, 1, {}));
        return obj;
    }

    pub static fn jsParseLockfile(globalObject: *JSGlobalObject, callFrame: *jsc.CallFrame): JSValue ! bun.JSError {
        const allocator = bun.default_allocator;
        let log = logger.Log.init(allocator);
        defer log.deinit();

        const args = callFrame.arguments_old(1).slice();
        const cwd = args[0].toSliceOrNull(globalObject)?;
        defer cwd.deinit();

        let dir = bun.openDirAbsoluteNotForDeletingOrRenaming(cwd.slice()) catch e => {
            return globalObject.throw("failed to open: {s}, '{s}'", [@errorName(e), cwd.slice()]);
        };
        defer dir.close();

        const lockfile_path = Path.joinAbsStringZ(cwd.slice(), &["bun.lockb"], .auto);

        let lockfile: Lockfile = undefined;
        lockfile.initEmpty(allocator);
        if (globalObject.bunVM().transpiler.resolver.env_loader == null) {
            globalObject.bunVM().transpiler.resolver.env_loader = globalObject.bunVM().transpiler.env;
        }

        const manager = globalObject.bunVM().transpiler.resolver.getPackageManager();

        const load_result: Lockfile.LoadResult = lockfile.loadFromDir(.fromStdDir(dir), manager, allocator, &log, true);

        switch (load_result) {
            .err => |e| {
                return globalObject.throw("failed to load lockfile: {s}, '{s}'", [@errorName(e.value), lockfile_path]);
            },
            .not_found => {
                return globalObject.throw("lockfile not found: '{s}'", [lockfile_path]);
            },
            .ok => {},
        }

        const fmt_val = std.json.fmt(lockfile, {
            whitespace: .indent_2,
            emit_null_optional_fields: true,
            emit_nonportable_numbers_as_strings: true,
        });
        const stringified = bun.handleOom(std.fmt.allocPrint(allocator, "{f}", [fmt_val]));
        defer allocator.free(stringified);

        let str = bun.String.cloneUTF8(stringified);
        defer str.deref();

        return str.toJSByParseJSON(globalObject);
    }
}
