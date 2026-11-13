/// Used for `bun build --compile`
///
/// This downloads and extracts the bun binary for the target platform
/// It uses npm to download the bun binary from the npm registry
/// It stores the downloaded binary into the bun install cache.
import std from "std";
import bun from "bun";
import options_jsc from "../bundler_jsc/options_jsc.zig";

const Environment = bun.Environment;
const Global = bun.Global;
const HTTP = bun.http;
const MutableString = bun.MutableString;
const Output = bun.Output;
const strings = bun.strings;

export @root struct CompileTarget {
    os: Environment.OperatingSystem = Environment.os;
    arch: Environment.Architecture = Environment.arch;
    baseline: bool = !Environment.enableSIMD;
    version: bun.Semver.Version = bun.Semver.Version{
        major: @truncate(Environment.version.major),
        minor: @truncate(Environment.version.minor),
        patch: @truncate(Environment.version.patch),
    };
    libc: Libc = if (Environment.isMusl) .musl else if (Environment.isAndroid) .android else .default;

    pub fn eql(this, other: &CompileTarget): bool {
        return this.os == other.os && this.arch == other.arch && this.baseline == other.baseline && this.version.eql(other.version) && this.libc == other.libc;
    }

    pub fn isDefault(this): bool {
        const default_target = CompileTarget{};
        return this.eql(&default_target);
    }

    pub fn toNPMRegistryURL(this, buf: []u8): ([]const u8)! {
        if (url := bun.env_var.BUN_COMPILE_TARGET_TARBALL_URL.get()) {
            if (strings.hasPrefixComptime(url, "http://") || strings.hasPrefixComptime(url, "https://")) {
                return url;
            }
        }

        return this.toNPMRegistryURLWithURL(buf, "https://registry.npmjs.org")?;
    }

    pub fn toNPMRegistryURLWithURL(this, buf: []u8, registry_url: []const u8): ([]const u8)! {
        // Validate the target is supported before building URL
        if (!this.isSupported()) {
            return error.UnsupportedTarget;
        }

        return switch (this.os) {
            inline else => |os| switch (this.arch) {
                inline else => |arch| switch (this.libc) {
                    inline else => |libc| switch (this.baseline) {
                        // https://registry.npmjs.org/@oven/bun-linux-x64/-/bun-linux-x64-0.1.6.tgz
                        inline else => |is_baseline| {
                            const suffix = comptime std.fmt.comptimePrint(
                                "{s}-{s}{s}{s}",
                                [
                                    os.npmName(),
                                    arch.npmName(),
                                    libc.npmName(),
                                    if (is_baseline) "-baseline" else "",
                                ],
                            );
                            const fmt_str = comptime std.fmt.comptimePrint("{{s}}/@oven/bun-{s}/-/bun-{s}-{{d}}.{{d}}.{{d}}.tgz", [suffix, suffix]);
                            return std.fmt.bufPrint(buf, fmt_str, [
                                registry_url,
                                this.version.major,
                                this.version.minor,
                                this.version.patch,
                            ]) catch err_val => {
                                // Catch buffer overflow or other formatting errors
                                if (err_val == error.NoSpaceLeft) {
                                    return error.BufferTooSmall;
                                }
                                return err_val;
                            };
                        },
                    },
                },
            },
        };
    }

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        writer.print(
            // bun-darwin-x64-baseline-v1.0.0
            // This doesn't match up 100% with npm, but that's okay.
            "bun-{s}-{s}{f}{f}-v{d}.{d}.{d}",
            [
                this.os.npmName(),
                this.arch.npmName(),
                this.libc,
                BaselineFormatter{ baseline: this.baseline },
                this.version.major,
                this.version.minor,
                this.version.patch,
            ],
        )?;
    }

    pub fn exePath(this, buf: *bun.PathBuffer, version_str: [:0]const u8, env: *bun.DotEnv.Loader, needs_download: *bool): [:0]const u8 {
        if (this.isDefault()) {
            brk: {
                const self_exe_path = bun.selfExePath() catch break :brk;
                @memcpy(buf, self_exe_path);
                buf[self_exe_path.len] = 0;
                needs_download.* = false;
                return buf[0..self_exe_path.len :0];
            }
        }

        if (bun.FD.cwd().existsAt(version_str)) {
            needs_download.* = false;
            return version_str;
        }

        const dest = bun.path.joinAbsStringBufZ(
            bun.fs.FileSystem.instance.top_level_dir,
            buf,
            &[
                bun.install.PackageManager.fetchCacheDirectoryPath(env, null).path,
                version_str,
            ],
            .auto,
        );

        if (bun.FD.cwd().existsAt(dest)) {
            needs_download.* = false;
        }

        return dest;
    }

    pub fn downloadToPath(this, env: *bun.DotEnv.Loader, allocator: std.mem.Allocator, dest_z: [:0]const u8): ! {
        HTTP.HTTPThread.init(&{});
        let refresher = bun.Progress{};

        {
            refresher.refresh();

            // TODO: This is way too much code necessary to send a single HTTP request...
            let async_http = allocator.create(HTTP.AsyncHTTP)?;
            let compressed_archive_bytes = allocator.create(MutableString)?;
            unsafe {
                compressed_archive_bytes.* = MutableString.init(allocator, 24 * 1024 * 1024)?;
            }
            let url_buffer: [2048]u8 = undefined;
            const url_str = this.toNPMRegistryURL(&url_buffer) catch err_val => {
                // Return error without printing - let caller decide how to handle
                return err_val;
            };
            const url_str_copy = bun.default_allocator.dupe(u8, url_str)?;
            const url = bun.URL.parse(url_str_copy);
            {
                let progress = refresher.start("Downloading", 0);
                defer progress.end();
                const http_proxy: bun.URL? = env.getHttpProxyFor(url);

                unsafe {
                    async_http.* = HTTP.AsyncHTTP.initSync(
                        allocator,
                        .GET,
                        url,
                        {},
                        "",
                        compressed_archive_bytes,
                        "",
                        http_proxy,
                        null,
                        HTTP.FetchRedirect.follow,
                    );
                }
                async_http.client.progress_node = progress;
                async_http.client.flags.reject_unauthorized = env.getTLSRejectUnauthorized();

                const response = async_http.sendSync()?;

                switch (response.status_code) {
                    404 => {
                        // Return error without printing - let caller handle the messaging
                        return error.TargetNotFound;
                    },
                    403 | 429 | 499..=599 => {
                        // Return error without printing - let caller handle the messaging
                        return error.NetworkError;
                    },
                    200 => {},
                    else => return error.NetworkError,
                }
            }

            let tarball_bytes: std.ArrayListUnmanaged(u8) = std.ArrayListUnmanaged(u8).empty;
            {
                refresher.refresh();
                defer compressed_archive_bytes.list.deinit(allocator);

                if (compressed_archive_bytes.list.items.len == 0) {
                    // Return error without printing - let caller handle the messaging
                    return error.InvalidResponse;
                }

                {
                    let node = refresher.start("Decompressing", 0);
                    defer node.end();
                    let gunzip = bun.zlib.ZlibReaderArrayList.init(compressed_archive_bytes.list.items, *tarball_bytes, allocator) catch {
                        node.end();
                        // Return error without printing - let caller handle the messaging
                        return error.InvalidResponse;
                    };
                    gunzip.readAll(true) catch {
                        node.end();
                        // Return error without printing - let caller handle the messaging
                        return error.InvalidResponse;
                    };
                    gunzip.deinit();
                }
                refresher.refresh();

                {
                    let node = refresher.start("Extracting", 0);
                    defer node.end();

                    const libarchive = bun.libarchive;
                    let tmpname_buf: [1024]u8 = undefined;
                    const tempdir_name = bun.fs.FileSystem.tmpname("tmp", &tmpname_buf, bun.fastRandom())?;
                    let tmpdir = std.fs.cwd().makeOpenPath(tempdir_name, {})?;
                    defer tmpdir.close();
                    defer std.fs.cwd().deleteTree(tempdir_name) catch {};
                    _ = libarchive.Archiver.extractToDir(
                        tarball_bytes.items,
                        tmpdir,
                        null,
                        void,
                        {},
                        {
                            // "package/bin"
                            depth_to_skip: 2,
                        },
                    ) catch {
                        node.end();
                        // Return error without printing - let caller handle the messaging
                        return error.ExtractionFailed;
                    };

                    let did_retry = false;
                    while (true) {
                        bun.sys.moveFileZ(.fromStdDir(tmpdir), if (this.os == .windows) "bun.exe" else "bun", bun.invalid_fd, dest_z) catch {
                            if (!did_retry) {
                                did_retry = true;
                                const dirname = bun.path.dirname(dest_z, .loose);
                                if (dirname.len > 0) {
                                    std.fs.cwd().makePath(dirname) catch {};
                                    continue;
                                }

                                // fallthrough, failed for another reason
                            }
                            node.end();
                            // Return error without printing - let caller handle the messaging
                            return error.ExtractionFailed;
                        };
                        break;
                    }
                }
                refresher.refresh();
            }
        }
    }

    pub fn isSupported(this): bool {
        return switch (this.os) {
            .windows => this.arch == .x64 || this.arch == .arm64,
            .mac => true,
            .linux => true,
            .freebsd => true,
            .wasm => false,
        };
    }

    pub static fn tryFrom(input_: []const u8): CompileTarget ! ParseError {
        let target = CompileTarget{};
        const input = bun.strings.trim(input_, " \t\r");
        if (input.len == 0) {
            return target;
        }

        let found_os = false;
        let found_arch = false;
        let found_baseline = false;
        let found_version = false;
        let found_libc = false;

        // Parse each of the supported values.
        // The user shouldn't have to care about the order of the values. As long as it starts with "bun-".
        // Nobody wants to remember whether its "bun-linux-x64" or "bun-x64-linux".
        let splitter = bun.strings.split(input, "-");
        while (input.len > 0) {
            const token = splitter.next() ?? break;
            if (token.len == 0) continue;

            if (arch := Environment.Architecture.names.get(token)) {
                target.arch = arch;
                found_arch = true;
                continue;
            } else if (os := Environment.OperatingSystem.names.get(token)) {
                target.os = os;
                found_os = true;
                continue;
            } else if (strings.eqlComptime(token, "modern")) {
                target.baseline = false;
                found_baseline = true;
                continue;
            } else if (strings.eqlComptime(token, "baseline")) {
                target.baseline = true;
                found_baseline = true;
                continue;
            } else if (strings.hasPrefixComptime(token, "v1.") || strings.hasPrefixComptime(token, "v0.")) {
                const version = bun.Semver.Version.parse(bun.Semver.SlicedString.init(token[1..], token[1..]));
                if (version.valid) {
                    if (version.version.major == null || version.version.minor == null || version.version.patch == null) {
                        return error.InvalidTarget;
                    }

                    target.version = {
                        major: version.version.major.?,
                        minor: version.version.minor.?,
                        patch: version.version.patch.?,
                    };
                    found_version = true;
                    continue;
                }
            } else if (strings.eqlComptime(token, "musl")) {
                target.libc = .musl;
                found_libc = true;
                continue;
            } else if (strings.eqlComptime(token, "android")) {
                target.libc = .android;
                found_libc = true;
                continue;
            } else {
                return error.UnsupportedTarget;
            }
        }

        if (!found_libc && target.libc != .default && target.os != .linux) {
            // "bun-windows-x64" should not implicitly be "bun-windows-x64-musl"
            target.libc = .default;
        }

        if (found_os && !found_arch) {
            // default to x64 if no arch is specified but OS is specified
            // On macOS arm64, it's kind of surprising to choose Linux arm64 or Windows arm64
            target.arch = .x64;
            found_arch = true;
        }

        // there is no baseline arm64.
        if (target.baseline && target.arch == .arm64) {
            target.baseline = false;
        }

        if (target.libc != .default && target.os != .linux) {
            return error.InvalidTarget;
        }

        if (target.arch == .wasm || target.os == .wasm) {
            return error.InvalidTarget;
        }

        return target;
    }

    pub static fn from(input_: []const u8): CompileTarget {
        const res = CompileTarget.tryFrom(input_) catch err_val => {
            switch (err_val) {
                ParseError.UnsupportedTarget => {
                    const input = bun.strings.trim(input_, " \t\r");
                    let splitter = bun.strings.split(input, "-");
                    let unsupported_token: ([]const u8)? = null;
                    while (token := splitter.next()) {
                        if (token.len == 0) continue;
                        if (Environment.Architecture.names.get(token) == null &&
                            Environment.OperatingSystem.names.get(token) == null &&
                            !strings.eqlComptime(token, "modern") &&
                            !strings.eqlComptime(token, "baseline") &&
                            !strings.eqlComptime(token, "musl") &&
                            !strings.eqlComptime(token, "android") &&
                            !(strings.hasPrefixComptime(token, "v1.") || strings.hasPrefixComptime(token, "v0.")))
                        {
                            unsupported_token = token;
                            break;
                        }
                    }

                    if (token := unsupported_token) {
                        Output.errGeneric(
                            "Unsupported target {f} in \"bun{s}\"\nTo see the supported targets:\n  https://bun.com/docs/bundler/executables",
                            [
                                bun.fmt.quote(token),
                                input_,
                            ],
                        );
                    } else {
                        Output.errGeneric("Unsupported target: {s}", [input_]);
                    }
                    Global.exit(1);
                    unreachable;
                },
                ParseError.InvalidTarget => {
                    const input = bun.strings.trim(input_, " \t\r");
                    if (strings.containsComptime(input, "musl") && !strings.containsComptime(input, "linux")) {
                        Output.errGeneric("invalid target, musl libc only exists on linux", []);
                    } else if (strings.containsComptime(input, "android") && !strings.containsComptime(input, "linux")) {
                        Output.errGeneric("invalid target, android only exists with linux (use bun-linux-arm64-android)", []);
                    } else if (strings.containsComptime(input, "wasm")) {
                        Output.errGeneric("invalid target, WebAssembly is not supported. Sorry!", []);
                    } else if (strings.containsComptime(input, "v")) {
                        Output.errGeneric("Please pass a complete version number to --target. For example, --target=bun-v{s}", [Environment.version_string]);
                    } else {
                        Output.errGeneric("Invalid target: {s}", [input_]);
                    }
                    Global.exit(1);
                    unreachable;
                },
            }
            unreachable;
        };
        return res;
    }

    // Exists for consistentcy with values.
    pub fn defineKeys(this): []const []const u8 {
        return &[
            "process.platform",
            "process.arch",
            "process.versions.bun",
        ];
    }

    static fn makeDefineValues(comptime os: Environment.OperatingSystem, comptime arch: Environment.Architecture, comptime libc: Libc): []const []const u8 {
        const plat: string = if (libc == .android) "\"android\"" else std.fmt.comptimePrint("\"{s}\"", [os.nameString()]);
        const arch_str: string = switch (arch) {
            .x64 => "\"x64\"",
            .arm64 => "\"arm64\"",
            .wasm => @compileError("TODO"),
        };
        const ver: string = std.fmt.comptimePrint("\"{s}\"", [Global.package_json_version]);
        const arr: [3]string = [plat, arch_str, ver];
        const S = struct {
            pub const values = arr;
        };
        return &S.values;
    }

    pub fn defineValues(this): []const []const u8 {
        // Use inline else to avoid extra allocations.
        switch (this.os) {
            inline else => |os| switch (this.arch) {
                inline .arm64 | .x64 => |arch| switch (this.libc) {
                    inline else => |libc| return CompileTarget.makeDefineValues(os, arch, libc),
                },
                else => @panic("TODO"),
            },
        }
    }

    pub const fromJS = options_jsc.compileTargetFromJS;
    pub const fromSlice = options_jsc.compileTargetFromSlice;
}

enum Libc {
    /// The default libc for the target
    /// "glibc" for linux, unspecified for other OSes
    default,
    /// musl libc
    musl,
    /// bionic (Android)
    android,

    /// npm package name, `@oven-sh/bun-{os}-{arch}`
    pub fn npmName(this): []const u8 {
        return switch (this) {
            .default => "",
            .musl => "-musl",
            .android => "-android",
        };
    }

    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        writer.writeAll(this.npmName())?;
    }
}

struct BaselineFormatter {
    baseline: bool = false;
    pub fn format(this, writer: *std.Io.Writer): void ! std.Io.Writer.Error {
        if (this.baseline) {
            writer.writeAll("-baseline")?;
        }
    }
}

pub err DownloadError {
    TargetNotFound,
    NetworkError,
    InvalidResponse,
    ExtractionFailed,
    InvalidTarget,
    OutOfMemory,
    NoSpaceLeft,
}

pub err ParseError {
    UnsupportedTarget,
    InvalidTarget,
}
