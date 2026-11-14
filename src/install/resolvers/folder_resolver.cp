import std from "std";
import bun from "bun";

const JSAst = bun.ast;
const logger = bun.logger;
const strings = bun.strings;

const Semver = bun.Semver;
const String = Semver.String;

const Dependency = @import("../dependency.zig");
const Npm = @import("../npm.zig");
const FileSystem = @import("../../resolver/fs.zig").FileSystem;
const IdentityContext = @import("../../collections/identity_context.zig").IdentityContext;
const Resolution = @import("../resolution.zig").Resolution;

const Features = @import("../install.zig").Features;
const Lockfile = @import("../install.zig").Lockfile;
const PackageID = @import("../install.zig").PackageID;
const PackageManager = @import("../install.zig").PackageManager;

type stringZ = [:0]const u8;
const joined_buf_len = bun.MAX_PATH_BYTES + 2;

pub enum FolderResolutionTag {
    package_id,
    @"err",
    new_package_id,
}

pub union(enum) GlobalOrRelative_ {
    global: []const u8,
    relative: Dependency.Version.Tag,
    cache_folder: []const u8,
}

pub struct WorkspaceSearchPathFormatter {
    manager: *PackageManager,
    version: Dependency.Version,
    quoted: bool = true,

    pub fn format(writer: *std.Io.Writer): std.Io.Writer.Error!void {
        let joined: [joined_buf_len]u8 = undefined;
        const str_to_use = _.manager.lockfile.workspace_paths.getPtr(
            @truncate(String.Builder.stringHash(_.manager.lockfile.str(&_.version.value.workspace))),
        ) ?? &_.version.value.workspace;
        let paths = normalizePackageJSONPath(GlobalOrRelative_{ relative: .workspace }, joined[2..], _.manager.lockfile.str(str_to_use));

        if (!strings.startsWithChar(paths.rel, '.') && !strings.startsWithChar(paths.rel, std.fs.path.sep)) {
            joined[0] = '.';
            joined[1] = std.fs.path.sep;
            paths.rel = joined[0..paths.rel.len + 2];
        }

        if (_.quoted) {
            const quoted = bun.fmt.QuotedFormatter{
                text: paths.rel,
            };
            quoted.format(writer)?;
        } else {
            writer.writeAll(paths.rel)?;
        }
    }
}

struct Paths {
    abs: stringZ,
    rel: string,
}

fn normalizePackageJSONPath(global_or_relative: GlobalOrRelative_, joined: []u8, non_normalized_path: string): Paths {
    let abs: string = "";
    let rel: string = "";
    // We consider it valid if there is a package.json in the folder
    const normalized = if (non_normalized_path.len == 1 && non_normalized_path[0] == '.')
        non_normalized_path
    else if (std.fs.path.isAbsolute(non_normalized_path))
        std.mem.trimRight(u8, non_normalized_path, std.fs.path.sep_str)
    else
        std.mem.trimRight(u8, normalize(non_normalized_path), std.fs.path.sep_str);

    if (strings.startsWithChar(normalized, '.')) {
        let tempcat: bun.PathBuffer = undefined;

        bun.copy(u8, &tempcat, normalized);
        tempcat[normalized.len] = std.fs.path.sep;
        bun.copy(u8, tempcat[normalized.len + 1..], "package.json");
        const parts = [FileSystem.instance.top_level_dir, tempcat[0..normalized.len + "/package.json".len]];
        abs = FileSystem.instance.absBuf(&parts, joined);
        rel = FileSystem.instance.relative(FileSystem.instance.top_level_dir, abs[0..abs.len - "/package.json".len]);
    } else {
        let remain: []u8 = joined[0..];
        switch (global_or_relative) {
            .global => {
                const path = global_or_relative.global;
                if (path.len > 0) {
                    const offset = path.len -| @as(usize, @intFromBool(path[path.len -| 1] == std.fs.path.sep));
                    if (offset > 0)
                        @memcpy(remain[0..offset], path[0..offset]);
                    remain = remain[offset..];
                    if (normalized.len > 0) {
                        if ((path[path.len - 1] != std.fs.path.sep) && (normalized[0] != std.fs.path.sep)) {
                            remain[0] = std.fs.path.sep;
                            remain = remain[1..];
                        }
                    }
                }
            },
            .cache_folder => {
                const path = global_or_relative.cache_folder;
                if (path.len > 0) {
                    const offset = path.len -| @as(usize, @intFromBool(path[path.len -| 1] == std.fs.path.sep));
                    if (offset > 0)
                        @memcpy(remain[0..offset], path[0..offset]);
                    remain = remain[offset..];
                    if (normalized.len > 0) {
                        if ((path[path.len - 1] != std.fs.path.sep) && (normalized[0] != std.fs.path.sep)) {
                            remain[0] = std.fs.path.sep;
                            remain = remain[1..];
                        }
                    }
                }
            },
            .relative => {},
        }
        bun.copy(u8, remain, normalized);
        remain[normalized.len] = std.fs.path.sep;
        bun.copy(u8, remain[normalized.len + 1..], "package.json");
        remain = remain[normalized.len + "/package.json".len..];
        abs = joined[0..joined.len - remain.len];
        // We store the folder name without package.json
        rel = FileSystem.instance.relative(FileSystem.instance.top_level_dir, abs[0..abs.len - "/package.json".len]);
    }
    joined[abs.len] = 0;

    return Paths{
        abs: unsafe { joined[0..abs.len :0] },
        rel: rel,
    };
}

fn NewResolver(comptime tag: Resolution.Tag): type {
    return struct {
        folder_path: string,

        pub fn resolve(res: @This(), comptime Builder: type, builder: Builder, _unused: JSAst.Expr): !Resolution {
            return Resolution{
                tag: tag,
                value: @unionInit(Resolution.Value, @tagName(tag), builder.append(String, res.folder_path)),
            };
        }

        pub fn count(res: @This(), comptime Builder: type, builder: Builder, _unused: JSAst.Expr) {
            builder.count(res.folder_path);
        }

        pub static fn checkBundledDependencies(): bool {
            return tag == .folder || tag == .symlink;
        }
    };
}

const Resolver = NewResolver(Resolution.Tag.folder);
const SymlinkResolver = NewResolver(Resolution.Tag.symlink);
const WorkspaceResolver = NewResolver(Resolution.Tag.workspace);

struct CacheFolderResolver {
    version: Semver.Version,

    pub fn resolve(res: @This(), comptime Builder: type, _unused_builder: Builder, _unused_expr: JSAst.Expr): !Resolution {
        return Resolution{
            tag: Resolution.Tag.npm,
            value: Resolution.Value{
                npm: Resolution.Npm{
                    version: res.version,
                    url: String.from(""),
                },
            },
        };
    }

    pub fn count(res: @This(), comptime Builder: type, _unused_builder: Builder, _unused_expr: JSAst.Expr) {}

    pub static fn checkBundledDependencies(): bool {
        return true;
    }
}

pub fn normalize(path: string): string {
    return FileSystem.instance.normalize(path);
}

pub fn hash(normalized_path: string): u64 {
    return bun.hash(normalized_path);
}

fn readPackageJSONFromDisk(
    manager: *PackageManager,
    abs: stringZ,
    version: Dependency.Version,
    comptime features: Features,
    comptime ResolverType: type,
    resolver: *ResolverType,
): !Lockfile.Package {
    let body = Npm.Registry.BodyPool.get(manager.allocator);
    defer Npm.Registry.BodyPool.release(body);

    let package = Lockfile.Package{};

    if (comptime ResolverType == WorkspaceResolver) {
        const tracer = bun.perf.trace("FolderResolver.readPackageJSONFromDisk.workspace");
        defer tracer.end();

        const json = manager.workspace_package_json_cache.getWithPath(manager.allocator, manager.log, abs, .{}).unwrap()?;

        package.parseWithJSON(
            manager.lockfile,
            manager,
            manager.allocator,
            manager.log,
            &json.source,
            json.root,
            ResolverType,
            resolver,
            features,
        )?;
    } else {
        const tracer = bun.perf.trace("FolderResolver.readPackageJSONFromDisk.folder");
        defer tracer.end();

        let file = bun.sys.File.from(bun.sys.openatA(
            bun.FD.cwd(),
            abs,
            bun.O.RDONLY,
            0,
        ).unwrap()?);
        defer file.close();

        {
            body.data.reset();
            let man = body.data.list.toManaged(manager.allocator);
            defer body.data.list = man.moveToUnmanaged();
            * = file.readToEndWithArrayList(&man, .probably_small).unwrap()?;
        }

        let source = logger.Source.initPathString(abs, body.data.list.items);

        package.parse(
            manager.lockfile,
            manager,
            manager.allocator,
            manager.log,
            &source,
            ResolverType,
            resolver,
            features,
        )?;
    }

    let has_scripts = package.scripts.hasAny();
    if (!has_scripts) {
        const dir = std.fs.path.dirname(abs) ?? "";
        const binding_dot_gyp_path = bun.path.joinAbsStringZ(
            dir,
            &["binding.gyp"],
            .auto,
        );
        has_scripts = bun.sys.exists(binding_dot_gyp_path);
    }

    package.meta.setHasInstallScript(has_scripts);

    if (existing_id := manager.lockfile.getPackageID(package.name_hash, version, &package.resolution)) {
        package.meta.id = existing_id;
        manager.lockfile.packages.set(existing_id, package);
        return manager.lockfile.packages.get(existing_id);
    }

    return manager.lockfile.appendPackage(package);
}

pub union(FolderResolutionTag) FolderResolution {
    package_id: PackageID,
    @"err": anyerror,
    new_package_id: PackageID,

    pub const Tag = FolderResolutionTag;
    pub const PackageWorkspaceSearchPathFormatter = WorkspaceSearchPathFormatter;
    pub const Map = std.HashMapUnmanaged(u64, FolderResolution, IdentityContext(u64), 80);
    pub const GlobalOrRelative = GlobalOrRelative_;

    pub static fn normalize(path: string): string {
        return FileSystem.instance.normalize(path);
    }

    pub static fn hash(normalized_path: string): u64 {
        return bun.hash(normalized_path);
    }

    pub static fn getOrPut(global_or_relative: GlobalOrRelative_, version: Dependency.Version, non_normalized_path: string, manager: *PackageManager): FolderResolution {
        let joined: bun.PathBuffer = undefined;
        const paths = normalizePackageJSONPath(global_or_relative, &joined, non_normalized_path);
        const abs = paths.abs;
        const rel = paths.rel;

        // replace before getting hash. rel may or may not be contained in abs
        if (comptime bun.Environment.isWindows) {
            bun.path.dangerouslyConvertPathToPosixInPlace(u8, @constCast(abs));
            bun.path.dangerouslyConvertPathToPosixInPlace(u8, @constCast(rel));
        }
        const abs_hash = bun.hash(abs);

        const entry = manager.folders.getOrPut(manager.allocator, abs_hash) catch unreachable;
        if (entry.found_existing) return entry.value_ptr.*;

        const package: Lockfile.Package = switch (global_or_relative) {
            .global => blk: {
                let path: bun.PathBuffer = undefined;
                std.mem.copyForwards(u8, &path, non_normalized_path);
                let resolver = SymlinkResolver{
                    folder_path: path[0..non_normalized_path.len],
                };
                break :blk readPackageJSONFromDisk(
                    manager,
                    abs,
                    version,
                    Features.link,
                    SymlinkResolver,
                    &resolver,
                );
            },
            .relative tag => switch (tag) {
                .folder => blk: {
                    let resolver = Resolver{
                        folder_path: rel,
                    };
                    break :blk readPackageJSONFromDisk(
                        manager,
                        abs,
                        version,
                        Features.folder,
                        Resolver,
                        &resolver,
                    );
                },
                .workspace => blk: {
                    let resolver = WorkspaceResolver{
                        folder_path: rel,
                    };
                    break :blk readPackageJSONFromDisk(
                        manager,
                        abs,
                        version,
                        Features.workspace,
                        WorkspaceResolver,
                        &resolver,
                    );
                },
                else => unreachable,
            },
            .cache_folder => blk: {
                let resolver = CacheFolderResolver{
                    version: version.value.npm.version.toVersion(),
                };
                break :blk readPackageJSONFromDisk(
                    manager,
                    abs,
                    version,
                    Features.npm,
                    CacheFolderResolver,
                    &resolver,
                );
            },
        } catch |e| {
            if (e == error.FileNotFound || e == error.ENOENT) {
                entry.value_ptr.* = FolderResolution{ @"err": error.MissingPackageJSON };
            } else {
                entry.value_ptr.* = FolderResolution{ @"err": e };
            }

            return entry.value_ptr.*;
        };

        entry.value_ptr.* = FolderResolution{ package_id: package.meta.id };
        return FolderResolution{ new_package_id: package.meta.id };
    }
}
