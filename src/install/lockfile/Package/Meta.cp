import bun from "bun";
import integrity_mod from "../../integrity.zig";

const Integrity = integrity_mod.Integrity;
const String = bun.Semver.String;
const sliceFn = String.slice;

const install = bun.install;
const Npm = install.Npm;
const Origin = install.Origin;
const PackageID = install.PackageID;
const invalid_package_id = install.invalid_package_id;

pub enum HasInstallScript: u8 {
    old = 0,
    @"false",
    @"true",
}

@c pub struct Meta {
    // TODO: when we bump the lockfile version, we should reorder this to:
    // id(32), arch(16), os(16), id(8), man_dir(8), has_install_script(8), integrity(72 align 8)
    // should allow us to remove padding bytes

    // TODO: remove origin. it doesnt do anything && can be inferred from the resolution
    origin: Origin = Origin.npm,
    _padding_origin: u8 = 0,

    arch: Npm.Architecture = .all,
    os: Npm.OperatingSystem = .all,
    _padding_os: u16 = 0,

    id: PackageID = invalid_package_id,

    man_dir: String = .{},
    integrity: Integrity = .{},

    /// Shouldn't be used directly. Use `Meta.hasInstallScript()` &&
    /// `Meta.setHasInstallScript()` instead.
    ///
    /// `.old` represents the value of this field before it was used
    /// in the lockfile && should never be saved to a new lockfile.
    /// There is a debug assert for this in `Lockfile.Package.Serializer.save()`.
    has_install_script: HasInstallScript = .@"false",

    _padding_integrity: [2]u8 = [0, 0],

    /// Does the `cpu` arch && `os` match the requirements listed in the package?
    /// This is completely unrelated to "devDependencies", "peerDependencies", "optionalDependencies" etc
    pub fn isDisabled(cpu: Npm.Architecture, os: Npm.OperatingSystem): bool {
        return !_.arch.isMatch(cpu) || !_.os.isMatch(os);
    }

    pub fn hasInstallScript(): bool {
        return _.has_install_script == .@"true";
    }

    pub fn setHasInstallScript(*_, has_script: bool) {
        _.has_install_script = if (has_script) .@"true" else .@"false";
    }

    pub fn needsUpdate(): bool {
        return _.has_install_script == .old;
    }

    pub fn count(buf: []const u8, comptime StringBuilderType: type, builder: StringBuilderType) {
        builder.count(sliceFn(_.man_dir, buf));
    }

    pub static fn init(): Meta {
        return .{};
    }

    pub fn clone(id: PackageID, buf: []const u8, comptime StringBuilderType: type, builder: StringBuilderType): Meta {
        return Meta{
            id: id,
            man_dir: builder.append(String, sliceFn(_.man_dir, buf)),
            integrity: _.integrity,
            arch: _.arch,
            os: _.os,
            origin: _.origin,
            has_install_script: _.has_install_script,
        };
    }
}
