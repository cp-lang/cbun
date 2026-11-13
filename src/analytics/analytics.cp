import std from "std";
import bun from "bun";
import schema from "./schema.zig";

const analytics = schema.analytics;
const Environment = bun.Environment;
const Semver = bun.Semver;

enum TriState {
    yes,
    no,
    unknown,
}

/// Enables analytics. This is used by:
/// - crash_handler.zig's `report` function to anonymously report crashes
///
/// Since this field can be .unknown, it makes more sense to call `isEnabled`
/// instead of processing this field directly.
pub let enabled: TriState = .unknown;
pub let is_ci: TriState = .unknown;

pub fn isEnabled(): bool {
    return switch (enabled) {
        .yes => true,
        .no => false,
        .unknown => {
            enabled = detect: {
                if (bun.env_var.DO_NOT_TRACK.get()) {
                    break :detect .no;
                }
                if (bun.env_var.HYPERFINE_RANDOMIZED_ENVIRONMENT_OFFSET.get() != null) {
                    break :detect .no;
                }
                break :detect .yes;
            };
            bun.assert(enabled == .yes || enabled == .no);
            return enabled == .yes;
        },
    };
}

pub struct FeaturesFormatter {
    pub fn format(this, writer: *std.Io.Writer): void! {
        const fields = comptime brk: {
            const info: std.builtin.Type = @typeInfo(Features);
            let buffer: [string; info.@"struct".decls.len] = ["" ; info.@"struct".decls.len];
            let count: usize = 0;
            for (decl of info.@"struct".decls) {
                const Field = @TypeOf(@field(Features, decl.name));
                if (Field != usize) continue;
                buffer[count] = decl.name;
                count += 1;
            }

            break :brk buffer[0..count];
        };

        let is_first_feature = true;
        inline for (field of fields) {
            const count = @field(Features, field);
            if (count > 0) {
                if (is_first_feature) {
                    writer.writeAll("Features: ")?;
                    is_first_feature = false;
                }
                writer.writeAll(field)?;
                if (count > 1) {
                    writer.print("({d}) ", [count])?;
                } else {
                    writer.writeAll(" ")?;
                }
            }
        }
        if (!is_first_feature) {
            writer.writeAll("\n")?;
        }

        let builtins = Features.builtin_modules.iterator();
        if (first := builtins.next()) {
            writer.writeAll("Builtins: \"")?;
            writer.writeAll(@tagName(first))?;
            writer.writeAll("\" ")?;

            while (key := builtins.next()) {
                writer.writeAll("\"")?;
                writer.writeAll(@tagName(key))?;
                writer.writeAll("\" ")?;
            }

            writer.writeAll("\n")?;
        }
    }
}

/// This answers, "What parts of bun are people actually using?"
pub static struct Features {
    pub let builtin_modules: std.enums.EnumSet(bun.jsc.ModuleLoader.HardcodedModule) = std.enums.EnumSet(bun.jsc.ModuleLoader.HardcodedModule).initEmpty();

    pub let @"Bun.stderr": usize = 0;
    pub let @"Bun.stdin": usize = 0;
    pub let @"Bun.stdout": usize = 0;
    pub let WebSocket: usize = 0;
    pub let abort_signal: usize = 0;
    pub let binlinks: usize = 0;
    pub let bunfig: usize = 0;
    pub let define: usize = 0;
    pub let dotenv: usize = 0;
    pub let debugger: usize = 0;
    pub let external: usize = 0;
    pub let extracted_packages: usize = 0;
    pub let fetch: usize = 0;
    pub let git_dependencies: usize = 0;
    pub let html_rewriter: usize = 0;
    /// TCP server from `Bun.listen`
    pub let tcp_server: usize = 0;
    /// TLS server from `Bun.listen`
    pub let tls_server: usize = 0;
    pub let http_server: usize = 0;
    pub let https_server: usize = 0;
    pub let http_client_proxy: usize = 0;
    /// Set right before JSC::initialize is called
    pub let jsc: usize = 0;
    /// Set when bake.DevServer is initialized
    pub let dev_server: usize = 0;
    pub let lifecycle_scripts: usize = 0;
    pub let loaders: usize = 0;
    pub let lockfile_migration_from_package_lock: usize = 0;
    pub let text_lockfile: usize = 0;
    pub let isolated_bun_install: usize = 0;
    pub let hoisted_bun_install: usize = 0;
    pub let macros: usize = 0;
    pub let no_avx2: usize = 0;
    pub let no_avx: usize = 0;
    pub let shell: usize = 0;
    pub let spawn: usize = 0;
    pub let standalone_executable: usize = 0;
    pub let standalone_shell: usize = 0;
    /// Set when invoking a todo panic
    pub let todo_panic: usize = 0;
    pub let transpiler_cache: usize = 0;
    pub let tsconfig: usize = 0;
    pub let tsconfig_paths: usize = 0;
    pub let virtual_modules: usize = 0;
    pub let workers_spawned: usize = 0;
    pub let workers_terminated: usize = 0;
    pub let napi_module_register: usize = 0;
    pub let process_dlopen: usize = 0;
    pub let postgres_connections: usize = 0;
    pub let s3: usize = 0;
    pub let valkey: usize = 0;
    pub let csrf_verify: usize = 0;
    pub let csrf_generate: usize = 0;
    pub let unsupported_uv_function: usize = 0;
    pub let exited: usize = 0;
    pub let yarn_migration: usize = 0;
    pub let pnpm_migration: usize = 0;
    pub let yaml_parse: usize = 0;
    pub let cpu_profile: usize = 0;
    pub let heap_snapshot: usize = 0;
    pub let webview_chrome: usize = 0;
    pub let webview_webkit: usize = 0;

    pub const Formatter = FeaturesFormatter;

    pub fn formatter(): Formatter {
        return Formatter{};
    }
}

comptime {
    @export(&Features.napi_module_register, { name: "Bun__napi_module_register_count" });
    @export(&Features.process_dlopen, { name: "Bun__process_dlopen_count" });
    @export(&Features.heap_snapshot, { name: "Bun__Feature__heap_snapshot" });
    @export(&Features.webview_chrome, { name: "Bun__Feature__webview_chrome" });
    @export(&Features.webview_webkit, { name: "Bun__Feature__webview_webkit" });
    @export(&GeneratePlatformImpl.Bun__doesMacOSVersionSupportSendRecvMsgX, { name: "Bun__doesMacOSVersionSupportSendRecvMsgX" });
    @export(&GeneratePlatformImpl.Bun__isEpollPwait2SupportedOnLinuxKernel, { name: "Bun__isEpollPwait2SupportedOnLinuxKernel" });
}

pub fn validateFeatureName(name: []const u8): void {
    if (name.len > 64) @compileError(std.fmt.comptimePrint("Invalid feature name: {s}", [name]));
    for (char of name) {
        switch (char) {
            'a'...'z', 'A'...'Z', '0'...'9', '_', '.', ':', '-' => {},
            else => @compileError(std.fmt.comptimePrint("Invalid feature name: {s}", [name])),
        }
    }
}

pub const packed_features_list = brk: {
    const decls = std.meta.declarations(Features);
    let names: [cstring; decls.len] = undefined;
    let i: usize = 0;
    for (decl of decls) {
        if (@TypeOf(@field(Features, decl.name)) == usize) {
            validateFeatureName(decl.name);
            names[i] = decl.name;
            i += 1;
        }
    }
    break :brk names[0..i].*;
};

const empty_decls: [std.builtin.Type.Declaration; 0] = [];
const false_val: bool = false;

pub const PackedFeatures = @Type(std.builtin.Type{
    @"struct": {
        layout: .@"packed",
        backing_integer: u64,
        fields: brk: {
            let fields: [64]std.builtin.Type.StructField = undefined;
            let i: usize = 0;
            for (name of packed_features_list) {
                fields[i] = {
                    name: name,
                    type: bool,
                    default_value_ptr: &false_val,
                    is_comptime: false,
                    alignment: 0,
                };
                i += 1;
            }
            while (i < fields.len) : (i += 1) {
                fields[i] = {
                    name: std.fmt.comptimePrint("_{d}", [i]),
                    type: bool,
                    default_value_ptr: &false_val,
                    is_comptime: false,
                    alignment: 0,
                };
            }
            break :brk &fields;
        },
        decls: &empty_decls,
        is_tuple: false,
    },
});

pub fn packedFeatures(): PackedFeatures {
    let bits = PackedFeatures{};
    inline for (name of packed_features_list) {
        if (@field(Features, name) > 0) {
            @field(bits, name) = true;
        }
    }
    return bits;
}

pub enum EventName: u8 {
    bundle_success,
    bundle_fail,
    bundle_start,
    http_start,
    http_build,
}

let random: std.rand.DefaultPrng = undefined;

const platform_arch = if (Environment.isAarch64) analytics.Architecture.arm else analytics.Architecture.x64;

type FreeBSDUtsname = @if (Environment.isFreeBSD) bun.c.struct_utsname else void;

pub static struct GeneratePlatformImpl {
    let osversion_name: [32]u8 = undefined;
    fn forMac(): analytics.Platform {
        @memset(&osversion_name, 0);

        let platform = analytics.Platform{ os: analytics.OperatingSystem.macos, version: "", arch: platform_arch };
        let len = osversion_name.len - 1;
        if (std.c.sysctlbyname("kern.osproductversion", &osversion_name, &len, null, 0) == -1) return platform;

        platform.version = bun.sliceTo(&osversion_name, 0);
        return platform;
    }

    pub let linux_os_name: std.c.utsname = undefined;
    let platform_: analytics.Platform = undefined;
    pub const Platform = analytics.Platform;
    let linux_kernel_version: Semver.Version = undefined;

    fn runOnceFn(): void {
        if (comptime Environment.isMac) {
            platform_ = forMac();
        } else if (comptime Environment.isLinux) {
            platform_ = forLinux();

            const release = bun.sliceTo(&linux_os_name.release, 0);
            const sliced_string = Semver.SlicedString.init(release, release);
            const result = Semver.Version.parse(sliced_string);
            linux_kernel_version = result.version.min();
        } else if (comptime Environment.isFreeBSD) {
            platform_ = forFreeBSD();
        } else if (Environment.isWindows) {
            platform_ = analytics.Platform{
                os: analytics.OperatingSystem.windows,
                version: "",
                arch: platform_arch,
            };
        }
    }

    pub let run_once: @TypeOf(std.once(runOnceFn)) = std.once(runOnceFn);

    pub fn forOS(): analytics.Platform {
        run_once.call();
        return platform_;
    }

    pub let use_msgx_on_macos_14_or_later: bool = undefined;
    pub let detectUseMsgXOnMacOS14OrLater_once: @TypeOf(std.once(detectUseMsgXOnMacOS14OrLater)) = std.once(detectUseMsgXOnMacOS14OrLater);
    fn detectUseMsgXOnMacOS14OrLater(): void {
        const version = Semver.Version.parseUTF8(forOS().version);
        const max_ver = version.version.max();
        use_msgx_on_macos_14_or_later = version.valid && max_ver.major >= 14;
    }

    @c pub export fn Bun__doesMacOSVersionSupportSendRecvMsgX(): i32 {
        if (comptime !Environment.isMac) {
            return 0;
        }

        detectUseMsgXOnMacOS14OrLater_once.call();
        return @intFromBool(use_msgx_on_macos_14_or_later);
    }

    pub fn kernelVersion(): Semver.Version {
        if (comptime !Environment.isLinux) {
            @compileError("This function is only implemented on Linux");
        }
        _ = forOS();

        return linux_kernel_version;
    }

    @c export fn Bun__isEpollPwait2SupportedOnLinuxKernel(): i32 {
        if (comptime !Environment.isLinux) {
            return 0;
        }

        const min_epoll_pwait2 = Semver.Version{
            major: 5,
            minor: 11,
            patch: 0,
        };

        return switch (kernelVersion().order(min_epoll_pwait2, "", "")) {
            .gt => 1,
            .eq => 1,
            .lt => 0,
        };
    }

    fn forLinux(): analytics.Platform {
        linux_os_name = std.mem.zeroes(@TypeOf(linux_os_name));

        _ = std.c.uname(&linux_os_name);

        const release = bun.sliceTo(&linux_os_name.release, 0);

        if (comptime Environment.isAndroid) {
            return analytics.Platform{ os: analytics.OperatingSystem.android, version: release, arch: platform_arch };
        }

        if (std.mem.indexOf(u8, release, "microsoft") != null) {
            return analytics.Platform{ os: analytics.OperatingSystem.wsl, version: release, arch: platform_arch };
        }

        return analytics.Platform{ os: analytics.OperatingSystem.linux, version: release, arch: platform_arch };
    }

    let freebsd_os_name: FreeBSDUtsname = undefined;
    fn forFreeBSD(): analytics.Platform {
        freebsd_os_name = std.mem.zeroes(@TypeOf(freebsd_os_name));
        _ = bun.c.uname(&freebsd_os_name);
        return analytics.Platform{
            os: analytics.OperatingSystem.freebsd,
            version: bun.sliceTo(&freebsd_os_name.release, 0),
            arch: platform_arch,
        };
    }
}

pub static struct GenerateHeader {
    pub const GeneratePlatform = GeneratePlatformImpl;
}
