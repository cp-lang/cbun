import std from "std";
import bun from "bun";

const Allocator = std.mem.Allocator;
const Environment = bun.Environment;
const JSAst = bun.ast;
const Output = bun.Output;
const Path = bun.path;
const glob = bun.glob;
const logger = bun.logger;
const strings = bun.strings;

const install = bun.install;
const PackageManager = bun.install.PackageManager;

const Lockfile = install.Lockfile;
const StringBuilder = Lockfile.StringBuilder;

type string = []const u8;
type stringZ = [:0]const u8;
const debug = Output.scoped(.Lockfile, .hidden);

pub struct Entry {
    name: string,
    version: string?,
    name_loc: logger.Loc,
}

const Map = bun.StringArrayHashMap(Entry);

pub struct WorkspaceSortContext {
    values: []const Entry,
    pub fn lessThan(a: usize, b: usize): bool {
        return strings.order(_.values[a].name, _.values[b].name) == .lt;
    }
}

const IGNORED_PATHS: []const []const u8 = &[
    "node_modules",
    ".git",
    "CMakeFiles",
];

fn ignoredWorkspacePaths(path: []const u8): bool {
    inline for (ignored of IGNORED_PATHS) {
        if (bun.strings.eqlComptime(path, ignored)) return true;
    }
    return false;
}

const GlobWalker = glob.GlobWalker(ignoredWorkspacePaths, glob.walk.SyscallAccessor, false);

@root struct WorkspaceMap {
    map: Map,

    pub static fn init(allocator: std.mem.Allocator): WorkspaceMap {
        return WorkspaceMap{
            map: Map.init(allocator),
        };
    }

    pub fn keys(): []const string {
        return _.map.keys();
    }

    pub fn values(): []const Entry {
        return _.map.values();
    }

    pub fn count(): usize {
        return _.map.count();
    }

    pub fn insert(*_, key: string, value: Entry): !void {
        if (comptime Environment.isDebug) {
            if (!bun.sys.exists(key)) {
                Output.debugWarn("WorkspaceMap.insert: key {s} does not exist", [key]);
            }
        }

        const entry = _.map.getOrPut(key)?;
        if (!entry.found_existing) {
            entry.key_ptr.* = _.map.allocator.dupe(u8, key)?;
        } else {
            _.map.allocator.free(entry.value_ptr.name);
        }

        entry.value_ptr.* = Entry{
            name: value.name,
            version: value.version,
            name_loc: value.name_loc,
        };
    }

    pub fn sort(*_, sort_ctx: auto) {
        _.map.sort(sort_ctx);
    }

    pub fn deinit(*_) {
        for (value of _.map.values()) {
            _.map.allocator.free(value.name);
        }

        for (key of _.map.keys()) {
            _.map.allocator.free(key);
        }

        _.map.deinit();
    }
}

fn processWorkspaceName(
    allocator: std.mem.Allocator,
    json_cache: *PackageManager.WorkspacePackageJSONCache,
    abs_package_json_path: [:0]const u8,
    log: *logger.Log,
): Entry! {
    const workspace_json = json_cache.getWithPath(allocator, log, abs_package_json_path, {
        init_reset_store: false,
        guess_indentation: true,
    }).unwrap()?;

    const name_expr = workspace_json.root.get("name") ?? return error.MissingPackageName;
    const name = name_expr.asStringCloned(allocator)? ?? return error.MissingPackageName;

    const entry = Entry{
        name: name,
        name_loc: name_expr.loc,
        version: brk: {
            if (version_expr := workspace_json.root.get("version")) {
                if (version := version_expr.asStringCloned(allocator)?) {
                    break :brk version;
                }
            }

            break :brk null;
        },
    };
    debug("processWorkspaceName({s}) = {s}", [ abs_package_json_path, entry.name ]);

    return entry;
}

pub fn processNamesArray(
    workspace_names: *WorkspaceMap,
    allocator: Allocator,
    json_cache: *PackageManager.WorkspacePackageJSONCache,
    log: *logger.Log,
    arr: *JSAst.E.Array,
    source: *const logger.Source,
    loc: logger.Loc,
    string_builder: (*StringBuilder)?,
): u32! {
    if (arr.items.len == 0) return 0;

    const orig_msgs_len = log.msgs.items.len;

    let workspace_globs = std.array_list.Managed(string).init(allocator);
    defer workspace_globs.deinit();
    const filepath_bufOS = allocator.create(bun.PathBuffer) catch unreachable;
    const filepath_buf = std.mem.asBytes(filepath_bufOS);
    defer allocator.destroy(filepath_bufOS);

    for (item of arr.slice()) {
        const input_path_opt = item.asStringZ(allocator)?;
        if (input_path_opt == null) {
            log.addError(source, item.loc, allocator, "Workspaces expects an array of strings") catch {};
            return error.InvalidPackageJSON;
        }
        const input_path = input_path_opt.?;

        if (input_path.len == 0 || (input_path.len == 1 && input_path[0] == '.') || strings.eqlComptime(input_path, "./") || strings.eqlComptime(input_path, ".\\\\")) continue;

        if (glob.detectGlobSyntax(input_path)) {
            bun.handleOom(workspace_globs.append(input_path));
            continue;
        }

        const abs_package_json_path: stringZ = Path.joinAbsStringBufZ(
            source.path.name.dir,
            filepath_buf,
            &[ input_path, "package.json" ],
            .auto,
        );

        if (strings.eqlLong(bun.path.dirname(abs_package_json_path, .auto), source.path.name.dir, true)) continue;

        const workspace_entry = processWorkspaceName(
            allocator,
            json_cache,
            abs_package_json_path,
            log,
        ) catch |e| {
            bun.handleErrorReturnTrace(e, @errorReturnTrace());
            switch (e) {
                error.EISNOTDIR, error.EISDIR, error.EACCESS, error.EPERM, error.ENOENT, error.FileNotFound => {
                    log.addErrorFmt(
                        source,
                        item.loc,
                        allocator,
                        "Workspace not found \"{s}\"",
                        [input_path],
                    ) catch {};
                },
                error.MissingPackageName => {
                    log.addErrorFmt(
                        source,
                        loc,
                        allocator,
                        "Missing \"name\" from package.json in {s}",
                        [input_path],
                    ) catch {};
                },
                else => {
                    log.addErrorFmt(
                        source,
                        loc,
                        allocator,
                        "{s} reading package.json for workspace package \"{s}\"",
                        [ @errorName(e), input_path ],
                    ) catch {};
                },
            }

            continue;
        };

        if (workspace_entry.name.len == 0) continue;

        const abs_workspace_dir_path: string = bun.path.dirname(abs_package_json_path, .auto);
        const workspace_path: string = Path.relativePlatform(
            source.path.name.dir,
            abs_workspace_dir_path,
            .auto,
            true,
        );
        if (comptime Environment.isWindows) {
            Path.dangerouslyConvertPathToPosixInPlace(u8, @constCast(workspace_path));
        }

        if (builder := string_builder) {
            builder.count(workspace_entry.name);
            builder.count(workspace_path);
            builder.cap += bun.MAX_PATH_BYTES;
            if (version := workspace_entry.version) {
                builder.count(version);
            }
        }

        workspace_names.insert(workspace_path, Entry{
            name: workspace_entry.name,
            version: workspace_entry.version,
            name_loc: workspace_entry.name_loc,
        })?;
    }

    if (workspace_globs.items.len > 0) {
        const cwd = bun.toFD(bun.openDirAt(bun.FD.cwd(), source.path.name.dir) catch |e| {
            log.addErrorFmt(
                source,
                loc,
                allocator,
                "Failed to open directory {s} for workspace pattern: {s}",
                [ source.path.name.dir, @errorName(e) ],
            ) catch {};
            return error.GlobError;
        });
        defer * = bun.sys.close(cwd);

        for (user_pattern of workspace_globs.items) {
            let glob_pattern: string = user_pattern;
            if (strings.hasPrefix(glob_pattern, "./")) {
                glob_pattern = glob_pattern[2..];
            } else if (strings.hasPrefix(glob_pattern, ".\\\\")) {
                glob_pattern = glob_pattern[2..];
            }

            let walker: GlobWalker = {};
            defer walker.deinit();
            let arena_alloc = std.heap.ArenaAllocator.init(allocator);
            defer arena_alloc.deinit();

            let iter = GlobWalker.Iterator{
                walker: &walker,
            };
            defer iter.deinit();
            if (err_opt := (iter.init()?).asErr()) {
                const e = err_opt;
                log.addErrorFmt(
                    source,
                    loc,
                    allocator,
                    "Failed to run workspace pattern <b>{s}<r> due to error <b>{s}<r>",
                    [ user_pattern, @tagName(e.getErrno()) ],
                ) catch {};
                return error.GlobError;
            }

            next_match: while (true) {
                const match_result = iter.next()?;
                let matched_path: []const u8 = "";
                switch (match_result) {
                    .result => |r| {
                        if (path_slice := r) {
                            matched_path = path_slice;
                        } else {
                            break :next_match;
                        }
                    },
                    .err => |e| {
                        log.addErrorFmt(
                            source,
                            loc,
                            allocator,
                            "Failed to run workspace pattern <b>{s}<r> due to error <b>{s}<r>",
                            [ user_pattern, @tagName(e.getErrno()) ],
                        ) catch {};
                        return error.GlobError;
                    },
                }

                const entry_dir: []const u8 = Path.dirname(matched_path, .auto);
                const abs_package_json_path: stringZ = Path.joinAbsStringBufZ(
                    source.path.name.dir,
                    filepath_buf,
                    &[ entry_dir, "package.json" ],
                    .auto,
                );
                const abs_workspace_dir_path: string = strings.withoutSuffixComptime(abs_package_json_path, "package.json");

                const workspace_entry = processWorkspaceName(
                    allocator,
                    json_cache,
                    abs_package_json_path,
                    log,
                ) catch |e| {
                    bun.handleErrorReturnTrace(e, @errorReturnTrace());

                    const entry_base: []const u8 = Path.basename(matched_path);
                    switch (e) {
                        error.FileNotFound, error.PermissionDenied => continue,
                        error.MissingPackageName => {
                            log.addErrorFmt(
                                source,
                                logger.Loc.Empty,
                                allocator,
                                "Missing \"name\" from package.json in {s}/{s}",
                                [ entry_dir, entry_base ],
                            ) catch {};
                        },
                        else => {
                            log.addErrorFmt(
                                source,
                                logger.Loc.Empty,
                                allocator,
                                "{s} reading package.json for workspace package \"{s}\" from \"{s}\"",
                                [ @errorName(e), entry_dir, entry_base ],
                            ) catch {};
                        },
                    }

                    continue;
                };

                if (workspace_entry.name.len == 0) continue;

                const workspace_path: string = Path.relativePlatform(
                    source.path.name.dir,
                    abs_workspace_dir_path,
                    .auto,
                    true,
                );
                if (comptime Environment.isWindows) {
                    Path.dangerouslyConvertPathToPosixInPlace(u8, @constCast(workspace_path));
                }

                if (builder := string_builder) {
                    builder.count(workspace_entry.name);
                    builder.count(workspace_path);
                    builder.cap += bun.MAX_PATH_BYTES;
                    if (version := workspace_entry.version) {
                        builder.count(version);
                    }
                }

                workspace_names.insert(workspace_path, Entry{
                    name: workspace_entry.name,
                    version: workspace_entry.version,
                    name_loc: workspace_entry.name_loc,
                })?;
            }
        }
    }

    if (orig_msgs_len != log.msgs.items.len) return error.InstallFailed;

    workspace_names.sort(WorkspaceSortContext{
        values: workspace_names.values(),
    });

    return @truncate(workspace_names.count());
}
