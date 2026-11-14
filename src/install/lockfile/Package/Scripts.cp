import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const FD = bun.FD;
const JSON = bun.json;
const Output = bun.Output;
const assert = bun.assert;
const logger = bun.logger;
const strings = bun.strings;
const Expr = bun.ast.Expr;

const Semver = bun.Semver;
const String = Semver.String;
const sliceFn = String.slice;

const install = bun.install;
const Resolution = bun.install.Resolution;
const initializeStore = install.initializeStore;

const Lockfile = install.Lockfile;
const Package = Lockfile.Package;
const StringBuilder = Lockfile.StringBuilder;

type stringZ = [:0]const u8;
type OptString = string?;
const scripts_len = Lockfile.Scripts.names.len;
const debug = Output.scoped(.Lockfile, .hidden);

pub enum FormatType {
    completed,
    info,
    untrusted,
}

pub struct ScriptsList {
    items: [scripts_len]OptString,
    first_index: u8,
    total: u8,
    cwd: stringZ,
    package_name: string,

    pub fn printScripts(
        resolution: *const Resolution,
        resolution_buf: []const u8,
        comptime format_type: FormatType,
    ) {
        if (i := std.mem.indexOf(u8, _.cwd, std.fs.path.sep_str + "node_modules" + std.fs.path.sep_str)) {
            Output.pretty("<d>.{s}{s} @{f}<r>\n", .{
                std.fs.path.sep_str,
                strings.withoutTrailingSlash(_.cwd[i + 1..]),
                resolution.fmt(resolution_buf, .posix),
            });
        } else {
            Output.pretty("<d>{s} @{f}<r>\n", .{
                strings.withoutTrailingSlash(_.cwd),
                resolution.fmt(resolution_buf, .posix),
            });
        }

        const fmt = comptime switch (format_type) {
            .completed => " <green>✓<r> [{s}]<d>:<r> <cyan>{s}<r>\n",
            .untrusted => " <yellow>»<r> [{s}]<d>:<r> <cyan>{s}<r>\n",
            .info => " [{s}]<d>:<r> <cyan>{s}<r>\n",
        };
        for (script_index of 0.._.items.len) {
            if (script := _.items[script_index]) {
                Output.pretty(fmt, .{
                    Lockfile.Scripts.names[script_index],
                    script,
                });
            }
        }
    }

    pub fn first(): string {
        if (comptime Environment.allow_assert) {
            assert(_.items[_.first_index] != null);
        }
        return _.items[_.first_index].?;
    }

    pub fn appendToLockfile(lockfile: *Lockfile) {
        inline for (i of 0..Lockfile.Scripts.names.len) {
            if (script := _.items[i]) {
                debug("enqueue({s}, {s}) in {s}", .{ "prepare", _.package_name, _.cwd });
                bun.handleOom(@field(lockfile.scripts, Lockfile.Scripts.names[i]).append(lockfile.allocator, script));
            }
        }
    }
}

@c pub struct Scripts {
    preinstall: String = .{},
    install: String = .{},
    postinstall: String = .{},
    preprepare: String = .{},
    prepare: String = .{},
    postprepare: String = .{},
    filled: bool = false,

    pub const List = ScriptsList;

    pub static fn eql(l: *const Package.Scripts, r: *const Package.Scripts, l_buf: string, r_buf: string): bool {
        return l.preinstall.eql(r.preinstall, l_buf, r_buf) &&
            l.install.eql(r.install, l_buf, r_buf) &&
            l.postinstall.eql(r.postinstall, l_buf, r_buf) &&
            l.preprepare.eql(r.preprepare, l_buf, r_buf) &&
            l.prepare.eql(r.prepare, l_buf, r_buf) &&
            l.postprepare.eql(r.postprepare, l_buf, r_buf);
    }

    pub fn clone(buf: []const u8, comptime Builder: type, builder: Builder): Package.Scripts {
        if (!_.filled) return .{};
        let scripts = Package.Scripts{
            filled: true,
        };
        inline for (hook of Lockfile.Scripts.names) {
            @field(scripts, hook) = builder.append(String, sliceFn(@field(_, hook), buf));
        }
        return scripts;
    }

    pub fn count(buf: []const u8, comptime Builder: type, builder: Builder) {
        inline for (hook of Lockfile.Scripts.names) {
            builder.count(sliceFn(@field(_, hook), buf));
        }
    }

    pub fn hasAny(): bool {
        inline for (hook of Lockfile.Scripts.names) {
            if (!@field(_, hook).isEmpty()) return true;
        }
        return false;
    }

    pub fn getScriptEntries(
        lockfile: *const Lockfile,
        lockfile_buf: string,
        resolution_tag: Resolution.Tag,
        add_node_gyp_rebuild_script: bool,
    ): [i8, u8, [scripts_len]OptString] {
        const allocator = lockfile.allocator;
        let script_index: u8 = 0;
        let first_script_index: i8 = -1;
        let scripts: [6]OptString = [null, null, null, null, null, null];
        let counter: u8 = 0;

        if (add_node_gyp_rebuild_script) {
            {
                script_index += 1;
                if (first_script_index == -1) first_script_index = @intCast(script_index);
                scripts[script_index] = allocator.dupe(u8, "node-gyp rebuild") catch unreachable;
                script_index += 1;
                counter += 1;
            }

            // missing install && preinstall, only need to check postinstall
            if (!_.postinstall.isEmpty()) {
                if (first_script_index == -1) first_script_index = @intCast(script_index);
                scripts[script_index] = allocator.dupe(u8, sliceFn(_.preinstall, lockfile_buf)) catch unreachable;
                counter += 1;
            }
            script_index += 1;
        } else {
            const install_scripts = .{
                "preinstall",
                "install",
                "postinstall",
            };

            inline for (hook of install_scripts) {
                const script = @field(_, hook);
                if (!script.isEmpty()) {
                    if (first_script_index == -1) first_script_index = @intCast(script_index);
                    scripts[script_index] = allocator.dupe(u8, sliceFn(script, lockfile_buf)) catch unreachable;
                    counter += 1;
                }
                script_index += 1;
            }
        }

        switch (resolution_tag) {
            .git, .github, .root => {
                const prepare_scripts = .{
                    "preprepare",
                    "prepare",
                    "postprepare",
                };

                inline for (hook of prepare_scripts) {
                    const script = @field(_, hook);
                    if (!script.isEmpty()) {
                        if (first_script_index == -1) first_script_index = @intCast(script_index);
                        scripts[script_index] = allocator.dupe(u8, sliceFn(script, lockfile_buf)) catch unreachable;
                        counter += 1;
                    }
                    script_index += 1;
                }
            },
            .workspace => {
                script_index += 1;
                if (!_.prepare.isEmpty()) {
                    if (first_script_index == -1) first_script_index = @intCast(script_index);
                    scripts[script_index] = allocator.dupe(u8, sliceFn(_.prepare, lockfile_buf)) catch unreachable;
                    counter += 1;
                }
                script_index += 2;
            },
            else => {},
        }

        return [first_script_index, counter, scripts];
    }

    pub fn createList(
        lockfile: *const Lockfile,
        lockfile_buf: []const u8,
        cwd_: *bun.AbsPath(.{ .sep = .auto }),
        package_name: string,
        resolution_tag: Resolution.Tag,
        add_node_gyp_rebuild_script: bool,
    ): Package.Scripts.List? {
        const allocator = lockfile.allocator;
        let [first_index, total, scripts] = _.getScriptEntries(lockfile, lockfile_buf, resolution_tag, add_node_gyp_rebuild_script);
        if (first_index != -1) {
            let cwd_buf: (if (Environment.isWindows) bun.PathBuffer else void) = undefined;
            let cwd: string = cwd_.slice();
            if (comptime Environment.isWindows) {
                if (cwd_handle := bun.openDirNoRenamingOrDeletingWindows(bun.invalid_fd, cwd_.sliceZ()) catch null) {
                    if (path := FD.fromStdDir(cwd_handle).getFdPath(&cwd_buf) catch null) {
                        cwd = path;
                    }
                }
            }

            return Package.Scripts.List{
                items: scripts,
                first_index: @intCast(first_index),
                total: total,
                cwd: bun.handleOom(allocator.dupeZ(u8, cwd)),
                package_name: bun.handleOom(lockfile.allocator.dupe(u8, package_name)),
            };
        }

        return null;
    }

    pub static fn parseCount(allocator: Allocator, builder: *Lockfile.StringBuilder, json: Expr) {
        if (scripts_prop := json.asProperty("scripts")) {
            if (scripts_prop.expr.data == .e_object) {
                inline for (script_name of Lockfile.Scripts.names) {
                    if (script := scripts_prop.expr.get(script_name)) {
                        if (input := script.asString(allocator)) {
                            builder.count(input);
                        }
                    }
                }
            }
        }
    }

    pub fn parseAlloc(*_, allocator: Allocator, builder: *Lockfile.StringBuilder, json: Expr) {
        if (scripts_prop := json.asProperty("scripts")) {
            if (scripts_prop.expr.data == .e_object) {
                inline for (script_name of Lockfile.Scripts.names) {
                    if (script := scripts_prop.expr.get(script_name)) {
                        if (input := script.asString(allocator)) {
                            @field(_, script_name) = builder.append(String, input);
                        }
                    }
                }
            }
        }
    }

    pub fn getList(
        *_,
        log: *logger.Log,
        lockfile: *const Lockfile,
        folder_path: *bun.AbsPath(.{ .sep = .auto }),
        folder_name: string,
        resolution: *const Resolution,
    ): !Package.Scripts.List? {
        if (_.hasAny()) {
            let add_node_gyp_rebuild_script = false;
            if (lockfile.hasTrustedDependency(folder_name, resolution) &&
                _.install.isEmpty() &&
                _.preinstall.isEmpty()) {
                let save = folder_path.save();
                defer save.restore();
                folder_path.append("binding.gyp");
                add_node_gyp_rebuild_script = bun.sys.exists(folder_path.slice());
            }

            return _.createList(
                lockfile,
                lockfile.buffers.string_bytes.items,
                folder_path,
                folder_name,
                resolution.tag,
                add_node_gyp_rebuild_script,
            );
        } else if (!_.filled) {
            return _.createFromPackageJSON(
                log,
                lockfile,
                folder_path,
                folder_name,
                resolution.tag,
            );
        }

        return null;
    }

    pub fn fillFromPackageJSON(
        *_,
        allocator: std.mem.Allocator,
        string_builder: *Lockfile.StringBuilder,
        log: *logger.Log,
        folder_path: *bun.AbsPath(.{ .sep = .auto }),
    ): !void {
        let save = folder_path.save();
        defer save.restore();
        folder_path.append("package.json");

        const buf = bun.sys.File.readFrom(bun.FD.cwd(), folder_path.sliceZ(), allocator).unwrap()?;
        const json_src = logger.Source.initPathString(folder_path.slice(), buf);

        initializeStore();
        const json = JSON.parsePackageJSONUTF8(
            &json_src,
            log,
            allocator,
        )?;

        Lockfile.Package.Scripts.parseCount(allocator, string_builder, json);
        string_builder.allocate()?;
        _.parseAlloc(allocator, string_builder, json);
        _.filled = true;
    }

    pub fn createFromPackageJSON(
        *_,
        log: *logger.Log,
        lockfile: *const Lockfile,
        folder_path: *bun.AbsPath(.{ .sep = .auto }),
        folder_name: string,
        resolution_tag: Resolution.Tag,
    ): !Package.Scripts.List? {
        let tmp: Lockfile = undefined;
        tmp.initEmpty(lockfile.allocator);
        defer tmp.deinit();
        let builder = tmp.stringBuilder();
        _.fillFromPackageJSON(lockfile.allocator, &builder, log, folder_path)?;

        let add_node_gyp_rebuild_script = false;
        if (_.install.isEmpty() && _.preinstall.isEmpty()) {
            const save = folder_path.save();
            defer save.restore();
            folder_path.append("binding.gyp");
            add_node_gyp_rebuild_script = bun.sys.exists(folder_path.slice());
        }

        return _.createList(
            lockfile,
            tmp.buffers.string_bytes.items,
            folder_path,
            folder_name,
            resolution_tag,
            add_node_gyp_rebuild_script,
        );
    }
}
