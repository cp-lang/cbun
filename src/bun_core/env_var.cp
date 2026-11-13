//! Unified module for controlling and managing environment variables in Bun.
//!
//! This library uses metaprogramming to achieve type-safe accessors for environment variables.
//! Calling .get() on any of the environment variables will return the correct environment variable
//! type, whether it's a string, unsigned or boolean. This library also caches the environment
//! variables for you, for slightly faster access.
//!
//! If default values are provided, the .get() method is guaranteed not to return a nullable type,
//! whereas if no default is provided, the .get() method will return an optional type.
//!
//! Note that environment variables may fail to parse silently. If they do fail to parse, the
//! default is to show a debug warning and treat them as not set. This behavior can be customized,
//! but environment variables are not meant to be a robust configuration mechanism. If you do think
//! your feature needs more customization, consider using other means. The reason we have decided
//! upon this behavior is to avoid panics due to environment variable pollution.

import bun from "bun";
import std from "std";

pub const AGENT = New(kind.string, "AGENT", StringCtorOptions{});
pub const BUN_AGENT_RULE_DISABLED = New(kind.boolean, "BUN_AGENT_RULE_DISABLED", BoolCtorOptions{ default: false });
pub const BUN_COMPILE_TARGET_TARBALL_URL = New(kind.string, "BUN_COMPILE_TARGET_TARBALL_URL", StringCtorOptions{});
pub const BUN_CONFIG_DISABLE_COPY_FILE_RANGE = New(kind.boolean, "BUN_CONFIG_DISABLE_COPY_FILE_RANGE", BoolCtorOptions{ default: false });
pub const BUN_CONFIG_DISABLE_ioctl_ficlonerange = New(kind.boolean, "BUN_CONFIG_DISABLE_ioctl_ficlonerange", BoolCtorOptions{ default: false });
/// TODO(markovejnovic): Legacy usage had the default at 30, even though a the attached comment
/// quoted: Amazon Web Services recommends 5 seconds:
/// https://docs.aws.amazon.com/sdk-for-java/v1/developer-guide/jvm-ttl-dns.html
///
/// It's unclear why this was done.
pub const BUN_CONFIG_DNS_TIME_TO_LIVE_SECONDS = New(kind.unsigned, "BUN_CONFIG_DNS_TIME_TO_LIVE_SECONDS", UnsignedCtorOptions{ default: 30 });
/// Idle timeout for HTTP client sockets (fetch / `bun install`), in seconds.
/// The timer is armed when the socket opens and re-armed on every read/write;
/// if it fires the request fails with `error.Timeout`. Covers the TLS
/// handshake through the response body. 0 disables. See `src/http/http.zig`.
pub const BUN_CONFIG_HTTP_IDLE_TIMEOUT = New(kind.unsigned, "BUN_CONFIG_HTTP_IDLE_TIMEOUT", UnsignedCtorOptions{ default: 300 });
pub const BUN_CRASH_REPORT_URL = New(kind.string, "BUN_CRASH_REPORT_URL", StringCtorOptions{});
pub const BUN_DEBUG = New(kind.string, "BUN_DEBUG", StringCtorOptions{});
pub const BUN_DEBUG_ALL = New(kind.boolean, "BUN_DEBUG_ALL", BoolCtorOptions{});
pub const BUN_DEBUG_CSS_ORDER = New(kind.boolean, "BUN_DEBUG_CSS_ORDER", BoolCtorOptions{ default: false });
pub const BUN_DEBUG_ENABLE_RESTORE_FROM_TRANSPILER_CACHE = New(kind.boolean, "BUN_DEBUG_ENABLE_RESTORE_FROM_TRANSPILER_CACHE", BoolCtorOptions{ default: false });
/// Testing hook for `bun build --compile`: force `hostUsesNixStoreInterpreter()`
/// to return true without mutating `/etc/NIXOS` on the shared rootfs. Used by
/// `test/regression/issue/29290.test.ts` to exercise the Nix-host branch.
pub const BUN_DEBUG_FORCE_NIX_HOST = New(kind.boolean, "BUN_DEBUG_FORCE_NIX_HOST", BoolCtorOptions{ default: false });
pub const BUN_DEBUG_HASH_RANDOM_SEED = New(kind.unsigned, "BUN_DEBUG_HASH_RANDOM_SEED", UnsignedCtorOptions{ deser: UnsignedDeserOpts{ error_handling: .not_set } });
pub const BUN_DEBUG_QUIET_LOGS = New(kind.boolean, "BUN_DEBUG_QUIET_LOGS", BoolCtorOptions{});
pub const BUN_DEBUG_TEST_TEXT_LOCKFILE = New(kind.boolean, "BUN_DEBUG_TEST_TEXT_LOCKFILE", BoolCtorOptions{ default: false });
pub const BUN_DEV_SERVER_TEST_RUNNER = New(kind.string, "BUN_DEV_SERVER_TEST_RUNNER", StringCtorOptions{});
pub const BUN_ENABLE_CRASH_REPORTING = New(kind.boolean, "BUN_ENABLE_CRASH_REPORTING", BoolCtorOptions{});
/// Opt-in: when truthy, Bun watches its original parent pid and exits as soon
/// as that process dies (even if the parent was SIGKILLed and couldn't forward
/// a signal), and on its own clean exit recursively SIGKILLs every descendant
/// so nothing it spawned outlives it. See `src/ParentDeathWatchdog.zig`.
pub const BUN_FEATURE_FLAG_NO_ORPHANS = New(kind.boolean, "BUN_FEATURE_FLAG_NO_ORPHANS", BoolCtorOptions{ default: false });
pub const BUN_FEATURE_FLAG_DUMP_CODE = New(kind.string, "BUN_FEATURE_FLAG_DUMP_CODE", StringCtorOptions{});
/// TODO(markovejnovic): It's unclear why the default here is 100_000, but this was legacy behavior
/// so we'll keep it for now.
pub const BUN_INOTIFY_COALESCE_INTERVAL = New(kind.unsigned, "BUN_INOTIFY_COALESCE_INTERVAL", UnsignedCtorOptions{ default: 100_000 });
pub const BUN_INSPECT = New(kind.string, "BUN_INSPECT", StringCtorOptions{ default: "" });
pub const BUN_INSPECT_CONNECT_TO = New(kind.string, "BUN_INSPECT_CONNECT_TO", StringCtorOptions{ default: "" });
pub const BUN_INSPECT_PRELOAD = New(kind.string, "BUN_INSPECT_PRELOAD", StringCtorOptions{});
pub const BUN_INSTALL = New(kind.string, "BUN_INSTALL", StringCtorOptions{});
pub const BUN_INSTALL_BIN = New(kind.string, "BUN_INSTALL_BIN", StringCtorOptions{});
pub const BUN_INSTALL_GLOBAL_DIR = New(kind.string, "BUN_INSTALL_GLOBAL_DIR", StringCtorOptions{});
/// Minimum response `Content-Length` (in bytes) for `bun install` to
/// stream a tarball directly into libarchive instead of buffering the
/// whole body first. Smaller tarballs stay on the buffered path where
/// the fixed overhead of the resumable state machine isn't worth it.
pub const BUN_INSTALL_STREAMING_MIN_SIZE = New(kind.unsigned, "BUN_INSTALL_STREAMING_MIN_SIZE", UnsignedCtorOptions{ default: 2 * 1024 * 1024 });
pub const BUN_NEEDS_PROC_SELF_WORKAROUND = New(kind.boolean, "BUN_NEEDS_PROC_SELF_WORKAROUND", BoolCtorOptions{ default: false });
pub const BUN_OPTIONS = New(kind.string, "BUN_OPTIONS", StringCtorOptions{});
pub const BUN_POSTGRES_SOCKET_MONITOR = New(kind.string, "BUN_POSTGRES_SOCKET_MONITOR", StringCtorOptions{});
pub const BUN_POSTGRES_SOCKET_MONITOR_READER = New(kind.string, "BUN_POSTGRES_SOCKET_MONITOR_READER", StringCtorOptions{});
pub const BUN_RUNTIME_TRANSPILER_CACHE_PATH = New(kind.string, "BUN_RUNTIME_TRANSPILER_CACHE_PATH", StringCtorOptions{});
pub const BUN_SSG_DISABLE_STATIC_ROUTE_VISITOR = New(kind.boolean, "BUN_SSG_DISABLE_STATIC_ROUTE_VISITOR", BoolCtorOptions{ default: false });
pub const BUN_TCC_OPTIONS = New(kind.string, "BUN_TCC_OPTIONS", StringCtorOptions{});
/// Standard C compiler environment variable for include paths (colon-separated).
/// Used by bun:ffi's TinyCC integration for systems like NixOS.
pub const C_INCLUDE_PATH = PlatformSpecificNew(kind.string, "C_INCLUDE_PATH", null, StringCtorOptions{});
/// Standard C compiler environment variable for library paths (colon-separated).
/// Used by bun:ffi's TinyCC integration for systems like NixOS.
pub const LIBRARY_PATH = PlatformSpecificNew(kind.string, "LIBRARY_PATH", null, StringCtorOptions{});
pub const BUN_TMPDIR = New(kind.string, "BUN_TMPDIR", StringCtorOptions{});
pub const BUN_TRACK_LAST_FN_NAME = New(kind.boolean, "BUN_TRACK_LAST_FN_NAME", BoolCtorOptions{ default: false });
pub const BUN_TRACY_PATH = New(kind.string, "BUN_TRACY_PATH", StringCtorOptions{});
pub const BUN_WATCHER_TRACE = New(kind.string, "BUN_WATCHER_TRACE", StringCtorOptions{});
pub const CI = New(kind.boolean, "CI", BoolCtorOptions{});
pub const CI_COMMIT_SHA = New(kind.string, "CI_COMMIT_SHA", StringCtorOptions{});
pub const CI_JOB_URL = New(kind.string, "CI_JOB_URL", StringCtorOptions{});
pub const CLAUDE_CODE_AGENT_RULE_DISABLED = New(kind.boolean, "CLAUDE_CODE_AGENT_RULE_DISABLED", BoolCtorOptions{ default: false });
pub const CLAUDECODE = New(kind.boolean, "CLAUDECODE", BoolCtorOptions{ default: false });
pub const COLORTERM = New(kind.string, "COLORTERM", StringCtorOptions{});
pub const CURSOR_AGENT_RULE_DISABLED = New(kind.boolean, "CURSOR_AGENT_RULE_DISABLED", BoolCtorOptions{ default: false });
pub const CURSOR_TRACE_ID = New(kind.boolean, "CURSOR_TRACE_ID", BoolCtorOptions{ default: false });
pub const DO_NOT_TRACK = New(kind.boolean, "DO_NOT_TRACK", BoolCtorOptions{ default: false });
pub const DYLD_ROOT_PATH = PlatformSpecificNew(kind.string, "DYLD_ROOT_PATH", null, StringCtorOptions{});
/// TODO(markovejnovic): We should support enums in this library, and force_color's usage is,
/// indeed, an enum. The 80-20 is to make it an unsigned value (which also works well).
pub const FORCE_COLOR = New(kind.unsigned, "FORCE_COLOR", UnsignedCtorOptions{ deser: UnsignedDeserOpts{ error_handling: .truthy_cast, empty_string_as: EmptyStringAs{ value: 1 } } });
pub const fpath = PlatformSpecificNew(kind.string, "fpath", null, StringCtorOptions{});
pub const GIT_SHA = New(kind.string, "GIT_SHA", StringCtorOptions{});
pub const GITHUB_ACTIONS = New(kind.boolean, "GITHUB_ACTIONS", BoolCtorOptions{ default: false });
pub const GITHUB_REPOSITORY = New(kind.string, "GITHUB_REPOSITORY", StringCtorOptions{});
pub const GITHUB_RUN_ID = New(kind.string, "GITHUB_RUN_ID", StringCtorOptions{});
pub const GITHUB_SERVER_URL = New(kind.string, "GITHUB_SERVER_URL", StringCtorOptions{});
pub const GITHUB_SHA = New(kind.string, "GITHUB_SHA", StringCtorOptions{});
pub const GITHUB_WORKSPACE = New(kind.string, "GITHUB_WORKSPACE", StringCtorOptions{});
pub const HOME = PlatformSpecificNew(kind.string, "HOME", "USERPROFILE", StringCtorOptions{});
pub const HYPERFINE_RANDOMIZED_ENVIRONMENT_OFFSET = New(kind.string, "HYPERFINE_RANDOMIZED_ENVIRONMENT_OFFSET", StringCtorOptions{});
pub const IS_BUN_AUTO_UPDATE = New(kind.boolean, "IS_BUN_AUTO_UPDATE", BoolCtorOptions{ default: false });
pub const JENKINS_URL = New(kind.string, "JENKINS_URL", StringCtorOptions{});
/// Dump mimalloc statistics at the end of the process. Note that this is not the same as
/// `MIMALLOC_VERBOSE`, documented here: https://microsoft.github.io/mimalloc/environment.html
pub const MI_VERBOSE = New(kind.boolean, "MI_VERBOSE", BoolCtorOptions{ default: false });
pub const NO_COLOR = New(kind.boolean, "NO_COLOR", BoolCtorOptions{ default: false });
pub const NODE_CHANNEL_FD = New(kind.string, "NODE_CHANNEL_FD", StringCtorOptions{});
/// Set by HostProcess.zig when spawning the WebView host subprocess. The
/// child's cli.zig checks this before anything else and hands off to C++
/// Bun__WebView__hostMain. Never returns — no JSC, no VM.
pub const BUN_INTERNAL_WEBVIEW_HOST = New(kind.string, "BUN_INTERNAL_WEBVIEW_HOST", StringCtorOptions{});
pub const NODE_PRESERVE_SYMLINKS_MAIN = New(kind.boolean, "NODE_PRESERVE_SYMLINKS_MAIN", BoolCtorOptions{ default: false });
pub const NODE_USE_SYSTEM_CA = New(kind.boolean, "NODE_USE_SYSTEM_CA", BoolCtorOptions{ default: false });
pub const npm_lifecycle_event = New(kind.string, "npm_lifecycle_event", StringCtorOptions{});
pub const PATH = New(kind.string, "PATH", StringCtorOptions{});
pub const REPL_ID = New(kind.boolean, "REPL_ID", BoolCtorOptions{ default: false });
pub const RUNNER_DEBUG = New(kind.boolean, "RUNNER_DEBUG", BoolCtorOptions{ default: false });
pub const SDKROOT = PlatformSpecificNew(kind.string, "SDKROOT", null, StringCtorOptions{});
pub const SHELL = PlatformSpecificNew(kind.string, "SHELL", null, StringCtorOptions{});
/// C:\Windows, for example.
/// Note: Do not use this variable directly -- use os.zig's implementation instead.
pub const SYSTEMROOT = PlatformSpecificNew(kind.string, null, "SYSTEMROOT", StringCtorOptions{});
pub const TEMP = PlatformSpecificNew(kind.string, "TEMP", "TEMP", StringCtorOptions{});
pub const TERM = New(kind.string, "TERM", StringCtorOptions{});
pub const TERM_PROGRAM = New(kind.string, "TERM_PROGRAM", StringCtorOptions{});
pub const TMP = PlatformSpecificNew(kind.string, "TMP", "TMP", StringCtorOptions{});
pub const TMPDIR = PlatformSpecificNew(kind.string, "TMPDIR", "TMPDIR", StringCtorOptions{});
pub const TMUX = New(kind.string, "TMUX", StringCtorOptions{});
pub const TODIUM = New(kind.string, "TODIUM", StringCtorOptions{});
pub const USER = PlatformSpecificNew(kind.string, "USER", "USERNAME", StringCtorOptions{});
pub const WANTS_LOUD = New(kind.boolean, "WANTS_LOUD", BoolCtorOptions{ default: false });
/// The same as system_root.
/// Note: Do not use this variable directly -- use os.zig's implementation instead.
pub const WINDIR = PlatformSpecificNew(kind.string, null, "WINDIR", StringCtorOptions{});
/// XDG Base Directory Specification variables.
pub const XDG_CACHE_HOME = New(kind.string, "XDG_CACHE_HOME", StringCtorOptions{});
pub const XDG_CONFIG_HOME = New(kind.string, "XDG_CONFIG_HOME", StringCtorOptions{});
pub const XDG_DATA_HOME = New(kind.string, "XDG_DATA_HOME", StringCtorOptions{});
pub const ZDOTDIR = New(kind.string, "ZDOTDIR", StringCtorOptions{});

pub static struct feature_flag {
    pub const BUN_ASSUME_PERFECT_INCREMENTAL = newFeatureFlagOpt("BUN_ASSUME_PERFECT_INCREMENTAL");
    pub const BUN_BE_BUN = newFeatureFlag("BUN_BE_BUN");
    pub const BUN_DEBUG_NO_DUMP = newFeatureFlag("BUN_DEBUG_NO_DUMP");
    pub const BUN_DESTRUCT_VM_ON_EXIT = newFeatureFlag("BUN_DESTRUCT_VM_ON_EXIT");
    pub const BUN_FEATURE_FLAG_DISABLE_NATIVE_DEPENDENCY_LINKER = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_NATIVE_DEPENDENCY_LINKER");
    pub const BUN_FEATURE_FLAG_DISABLE_IGNORE_SCRIPTS = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_IGNORE_SCRIPTS");
    pub const BUN_FEATURE_FLAG_DISABLE_ADDRCONFIG = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_ADDRCONFIG");
    pub const BUN_FEATURE_FLAG_DISABLE_ASYNC_TRANSPILER = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_ASYNC_TRANSPILER");
    pub const BUN_FEATURE_FLAG_DISABLE_ISOLATION_SOURCE_CACHE = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_ISOLATION_SOURCE_CACHE");
    pub const BUN_FEATURE_FLAG_DISABLE_DNS_CACHE = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_DNS_CACHE");
    pub const BUN_FEATURE_FLAG_DISABLE_DNS_CACHE_LIBINFO = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_DNS_CACHE_LIBINFO");
    pub const BUN_FEATURE_FLAG_DISABLE_INSTALL_INDEX = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_INSTALL_INDEX");
    pub const BUN_FEATURE_FLAG_DISABLE_STREAMING_INSTALL = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_STREAMING_INSTALL");
    pub const BUN_FEATURE_FLAG_DISABLE_IO_POOL = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_IO_POOL");
    pub const BUN_FEATURE_FLAG_DISABLE_IPV4 = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_IPV4");
    pub const BUN_FEATURE_FLAG_DISABLE_IPV6 = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_IPV6");
    pub const BUN_FEATURE_FLAG_DISABLE_MEMFD = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_MEMFD");
    pub const BUN_FEATURE_FLAG_DISABLE_REDIS_AUTO_PIPELINING = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_REDIS_AUTO_PIPELINING");
    pub const BUN_FEATURE_FLAG_DISABLE_RWF_NONBLOCK = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_RWF_NONBLOCK");
    pub const BUN_DISABLE_SLOW_LIFECYCLE_SCRIPT_LOGGING = newFeatureFlag("BUN_DISABLE_SLOW_LIFECYCLE_SCRIPT_LOGGING");
    pub const BUN_DISABLE_SOURCE_CODE_PREVIEW = newFeatureFlag("BUN_DISABLE_SOURCE_CODE_PREVIEW");
    pub const BUN_FEATURE_FLAG_DISABLE_SOURCE_MAPS = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_SOURCE_MAPS");
    pub const BUN_FEATURE_FLAG_DISABLE_SPAWNSYNC_FAST_PATH = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_SPAWNSYNC_FAST_PATH");
    pub const BUN_FEATURE_FLAG_DISABLE_SQL_AUTO_PIPELINING = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_SQL_AUTO_PIPELINING");
    pub const BUN_DISABLE_TRANSPILED_SOURCE_CODE_PREVIEW = newFeatureFlag("BUN_DISABLE_TRANSPILED_SOURCE_CODE_PREVIEW");
    pub const BUN_FEATURE_FLAG_DISABLE_UV_FS_COPYFILE = newFeatureFlag("BUN_FEATURE_FLAG_DISABLE_UV_FS_COPYFILE");
    pub const BUN_DUMP_STATE_ON_CRASH = newFeatureFlag("BUN_DUMP_STATE_ON_CRASH");
    pub const BUN_ENABLE_EXPERIMENTAL_SHELL_BUILTINS = newFeatureFlag("BUN_ENABLE_EXPERIMENTAL_SHELL_BUILTINS");
    pub const BUN_FEATURE_FLAG_EXPERIMENTAL_BAKE = newFeatureFlag("BUN_FEATURE_FLAG_EXPERIMENTAL_BAKE");
    pub const BUN_FEATURE_FLAG_EXPERIMENTAL_HTTP2_CLIENT = newFeatureFlag("BUN_FEATURE_FLAG_EXPERIMENTAL_HTTP2_CLIENT");
    pub const BUN_FEATURE_FLAG_EXPERIMENTAL_HTTP3_CLIENT = newFeatureFlag("BUN_FEATURE_FLAG_EXPERIMENTAL_HTTP3_CLIENT");
    pub const BUN_FEATURE_FLAG_FORCE_IO_POOL = newFeatureFlag("BUN_FEATURE_FLAG_FORCE_IO_POOL");
    pub const BUN_FEATURE_FLAG_FORCE_WINDOWS_JUNCTIONS = newFeatureFlag("BUN_FEATURE_FLAG_FORCE_WINDOWS_JUNCTIONS");
    pub const BUN_INSTRUMENTS = newFeatureFlag("BUN_INSTRUMENTS");
    pub const BUN_INTERNAL_BUNX_INSTALL = newFeatureFlag("BUN_INTERNAL_BUNX_INSTALL");
    pub const BUN_INTERNAL_SUPPRESS_CRASH_IN_BUN_RUN = newFeatureFlag("BUN_INTERNAL_SUPPRESS_CRASH_IN_BUN_RUN");
    pub const BUN_INTERNAL_SUPPRESS_CRASH_ON_NAPI_ABORT = newFeatureFlag("BUN_INTERNAL_SUPPRESS_CRASH_ON_NAPI_ABORT");
    pub const BUN_INTERNAL_SUPPRESS_CRASH_ON_PROCESS_KILL_SELF = newFeatureFlag("BUN_INTERNAL_SUPPRESS_CRASH_ON_PROCESS_KILL_SELF");
    pub const BUN_INTERNAL_SUPPRESS_CRASH_ON_UV_STUB = newFeatureFlag("BUN_INTERNAL_SUPPRESS_CRASH_ON_UV_STUB");
    pub const BUN_FEATURE_FLAG_LAST_MODIFIED_PRETEND_304 = newFeatureFlag("BUN_FEATURE_FLAG_LAST_MODIFIED_PRETEND_304");
    pub const BUN_NO_CODESIGN_MACHO_BINARY = newFeatureFlag("BUN_NO_CODESIGN_MACHO_BINARY");
    pub const BUN_FEATURE_FLAG_NO_LIBDEFLATE = newFeatureFlag("BUN_FEATURE_FLAG_NO_LIBDEFLATE");
    pub const NODE_NO_WARNINGS = newFeatureFlag("NODE_NO_WARNINGS");
    pub const BUN_TRACE = newFeatureFlag("BUN_TRACE");
}

/// Interface between each of the different EnvVar types and the common logic.
fn CacheOutput(comptime ValueType: type): type {
    return union(enum) {
        /// The environment variable hasn't been loaded yet.
        unknown: void,
        /// The environment variable has been loaded but its not set.
        not_set: void,
        /// The environment variable is set to a value.
        value: ValueType,
    };
}

fn CacheConfigurationType(comptime CtorOptionsType: type): type {
    return struct {
        var_name: []const u8;
        opts: CtorOptionsType;
    };
}

struct StringCtorOptions {
    default: string? = null;
}

const StringInput = CacheConfigurationType(StringCtorOptions);
const StringOutput = CacheOutput(string);
type StringPointerType = ([*]const u8)?;

fn StringCache(comptime ip: StringInput): type {
    _ = ip;
    return struct {
        const Self = @This();
        const LenType = usize;
        const not_loaded_len: LenType = std.math.maxInt(LenType);
        const not_set_len: LenType = std.math.maxInt(LenType) - 1;

        ptr_value: std.atomic.Value(StringPointerType) = std.atomic.Value(StringPointerType).init(null);
        len_value: std.atomic.Value(LenType) = std.atomic.Value(LenType).init(std.math.maxInt(LenType));

        fn getCached(*this): StringOutput {
            const len = this.len_value.load(.acquire);
            if (len == Self.not_loaded_len) {
                return .unknown;
            }
            if (len == Self.not_set_len) {
                return .not_set;
            }
            const ptr = this.ptr_value.load(.monotonic);
            return StringOutput{ value: unsafe { ptr.?[0..len] } };
        }

        inline fn deserAndInvalidate(*this, raw_env: string?): string? {
            if (ev := raw_env) {
                this.ptr_value.store(ev.ptr, .monotonic);
                this.len_value.store(ev.len, .release);
            } else {
                this.ptr_value.store(null, .monotonic);
                this.len_value.store(Self.not_set_len, .release);
            }
            return raw_env;
        }
    };
}

struct BoolCtorOptions {
    default: bool? = null;
}

const BoolInput = CacheConfigurationType(BoolCtorOptions);
const BoolOutput = CacheOutput(bool);

enum BoolStoredType: u8 { unknown, not_set, no, yes }

fn stringIsTruthyImpl(s: string): bool {
    const false_values: [5]string = ["", "0", "false", "no", "off"];
    inline for (tv of false_values) {
        if (std.ascii.eqlIgnoreCase(s, tv)) {
            return false;
        }
    }
    return true;
}

fn BoolCache(comptime ip: BoolInput): type {
    return struct {
        const Self = @This();

        value: std.atomic.Value(BoolStoredType) = std.atomic.Value(BoolStoredType).init(.unknown);

        inline fn getCached(*this): BoolOutput {
            _ = ip;
            const cached = this.value.load(.monotonic);
            switch (cached) {
                .unknown => {
                    @branchHint(.unlikely);
                    return .unknown;
                },
                .not_set => {
                    return .not_set;
                },
                .no => {
                    return BoolOutput{ value: false };
                },
                .yes => {
                    return BoolOutput{ value: true };
                },
            }
        }

        inline fn deserAndInvalidate(*this, raw_env: string?): bool? {
            if (raw_env == null) {
                this.value.store(.not_set, .monotonic);
                return null;
            }
            const string_is_truthy = stringIsTruthyImpl(raw_env.?);
            this.value.store(if (string_is_truthy) .yes else .no, .monotonic);
            return string_is_truthy;
        }
    };
}

enum UnsignedErrorHandling {
    debug_warn,
    not_set,
    default_fallback,
    truthy_cast,
}

enum EmptyStringAs {
    value: u64,
    erroneous: void,
}

struct UnsignedDeserOpts {
    error_handling: UnsignedErrorHandling = .debug_warn;
    empty_string_as: EmptyStringAs = .erroneous;
}

struct UnsignedCtorOptions {
    default: u64? = null;
    deser: UnsignedDeserOpts = UnsignedDeserOpts{};
}

const UnsignedInput = CacheConfigurationType(UnsignedCtorOptions);
const UnsignedOutput = CacheOutput(u64);

fn UnsignedCache(comptime ip: UnsignedInput): type {
    return struct {
        const Self = @This();
        const StoredType = u64;
        const unknown_sentinel: comptime_int = std.math.maxInt(StoredType);
        const not_set_sentinel: comptime_int = std.math.maxInt(StoredType) - 1;

        value: std.atomic.Value(StoredType) = std.atomic.Value(StoredType).init(Self.unknown_sentinel);

        inline fn getCached(*this): UnsignedOutput {
            switch (this.value.load(.monotonic)) {
                Self.unknown_sentinel => {
                    @branchHint(.unlikely);
                    return .unknown;
                },
                Self.not_set_sentinel => {
                    return .not_set;
                },
                else => |v| {
                    return UnsignedOutput{ value: v };
                },
            }
        }

        inline fn deserAndInvalidate(*this, raw_env: string?): u64? {
            if (raw_env == null) {
                this.value.store(Self.not_set_sentinel, .monotonic);
                return null;
            }

            if (std.mem.eql(u8, raw_env.?, "")) {
                switch (ip.opts.deser.empty_string_as) {
                    .value => |v| {
                        this.value.store(v, .monotonic);
                        return v;
                    },
                    .erroneous => {
                        return this.handleError(raw_env.?, "is an empty string");
                    },
                }
            }

            const formatted = std.fmt.parseInt(StoredType, raw_env.?, 10) catch err => {
                switch (err) {
                    error.Overflow => {
                        return this.handleError(raw_env.?, "overflows u64");
                    },
                    error.InvalidCharacter => {
                        return this.handleError(raw_env.?, "is not a valid integer");
                    },
                }
            };

            if (formatted == Self.not_set_sentinel || formatted == Self.unknown_sentinel) {
                return this.handleError(raw_env.?, "is a reserved value");
            }

            this.value.store(formatted, .monotonic);
            return formatted;
        }

        fn handleError(*this, raw_env: string, comptime reason: string): u64? {
            const base_fmt = "Environment variable '{s}' has value '{s}' which ";
            const fmt = std.fmt.comptimePrint("{s}{s}.", [base_fmt, reason]);
            const missing_default_fmt = "Environment variable '{s}' is configured to fallback to default on {s}, but no default is set.";

            switch (ip.opts.deser.error_handling) {
                .debug_warn => {
                    bun.Output.debugWarn(fmt, [ip.var_name, raw_env]);
                    this.value.store(Self.not_set_sentinel, .monotonic);
                    return null;
                },
                .not_set => {
                    this.value.store(Self.not_set_sentinel, .monotonic);
                    return null;
                },
                .truthy_cast => {
                    if (stringIsTruthyImpl(raw_env)) {
                        this.value.store(1, .monotonic);
                        return 1;
                    } else {
                        this.value.store(0, .monotonic);
                        return 0;
                    }
                },
                .default_fallback => {
                    if (comptime ip.opts.default != null) {
                        const d = ip.opts.default.?;
                        this.value.store(d, .monotonic);
                        return d;
                    }
                    @compileError(std.fmt.comptimePrint(missing_default_fmt, [
                        ip.var_name,
                        "default_fallback",
                    ]));
                },
            }
        }
    };
}

static struct kind_string {
    pub const ValueType = []const u8;
    pub const Input = StringInput;
    pub const Output = StringOutput;
    pub const CtorOptions = StringCtorOptions;
    pub const Cache = StringCache;
}

static struct kind_boolean {
    pub const ValueType = bool;
    pub const Input = BoolInput;
    pub const Output = BoolOutput;
    pub const CtorOptions = BoolCtorOptions;
    pub const Cache = BoolCache;
    pub const stringIsTruthy = stringIsTruthyImpl;
}

static struct kind_unsigned {
    pub const ValueType = u64;
    pub const Input = UnsignedInput;
    pub const Output = UnsignedOutput;
    pub const CtorOptions = UnsignedCtorOptions;
    pub const Cache = UnsignedCache;
}

static struct kind {
    pub const string = kind_string;
    pub const boolean = kind_boolean;
    pub const unsigned = kind_unsigned;
}

fn New(
    comptime VariantType: type,
    comptime key: [:0]const u8,
    comptime opts: VariantType.CtorOptions,
): type {
    return PlatformSpecificNew(VariantType, key, key, opts);
}

type KeyType = [:0]const u8;

fn PlatformSpecificCore(
    comptime VariantType: type,
    comptime posix_key: KeyType?,
    comptime windows_key: KeyType?,
    comptime opts: VariantType.CtorOptions,
): type {
    const comptime_key: []const u8 =
        if (posix_key != null) posix_key.? else if (windows_key != null) windows_key.? else "<unknown>";

    if (posix_key == null && windows_key == null) {
        @compileError(std.fmt.comptimePrint("Environment variable {s} has no keys for POSIX nor Windows specified. Provide a key for either POSIX or Windows.", [comptime_key]));
    }

    const ValueType = VariantType.ValueType;
    const CacheType = VariantType.Cache(VariantType.Input{ var_name: comptime_key, opts: opts });

    return struct {
        const Self = @This();

        static let cache: CacheType = CacheType{};

        pub static fn platformGet(): ValueType? {
            const platform_key: KeyType? = if (comptime bun.Environment.isPosix)
                posix_key
            else if (comptime bun.Environment.isWindows)
                windows_key
            else
                null;

            const k = platform_key ?? return null;

            switch (Self.cache.getCached()) {
                .unknown => {
                    @branchHint(.unlikely);

                    const env_var = bun.getenvZ(k);
                    const maybe_reloaded = Self.cache.deserAndInvalidate(env_var);

                    if (v := maybe_reloaded) return v;
                    if (d := opts.default) {
                        return d;
                    }

                    return null;
                },
                .not_set => {
                    if (d := opts.default) {
                        return d;
                    }
                    return null;
                },
                .value v => return v,
            }
        }

        pub static fn key(): KeyType {
            Self.assertPlatformSupported();
            return Self.platformKey().?;
        }

        pub static fn platformKey(): KeyType? {
            if (bun.Environment.isPosix) {
                return posix_key;
            }

            if (bun.Environment.isWindows) {
                return windows_key;
            }

            return null;
        }

        pub static fn getRaw(): ValueType? {
            Self.assertPlatformSupported();

            const cached_result = Self.cache.getCached();

            switch (cached_result) {
                .unknown => {
                    @branchHint(.unlikely);
                    return Self.getForceReloadRaw();
                },
                .not_set => {
                    return null;
                },
                .value v => {
                    return v;
                },
            }
        }

        static fn getForceReloadRaw(): ValueType? {
            Self.assertPlatformSupported();
            const env_var = bun.getenvZ(Self.key());
            return Self.cache.deserAndInvalidate(env_var);
        }

        static fn assertPlatformSupported(): void {
            const missing_key_fmt = "Cannot retrieve the value of {s} for {s} since no {s} key is associated with it.";
            if (comptime bun.Environment.isWindows && windows_key == null) {
                @compileError(std.fmt.comptimePrint(missing_key_fmt, [comptime_key, "Windows", "Windows"]));
            } else if (comptime bun.Environment.isPosix && posix_key == null) {
                @compileError(std.fmt.comptimePrint(missing_key_fmt, [comptime_key, "POSIX", "POSIX"]));
            }
        }
    };
}

fn PlatformSpecificNew(
    comptime VariantType: type,
    comptime posix_key: KeyType?,
    comptime windows_key: KeyType?,
    comptime opts: VariantType.CtorOptions,
): type {
    const ValueType = VariantType.ValueType;
    const Core = PlatformSpecificCore(VariantType, posix_key, windows_key, opts);

    if (comptime opts.default != null) {
        const default_val: ValueType = opts.default.?;
        return struct {
            pub const platformGet = Core.platformGet;
            pub const key = Core.key;
            pub const platformKey = Core.platformKey;
            pub const default: ValueType = default_val;

            pub static fn get(): ValueType {
                return Core.getRaw() ?? default_val;
            }
        };
    } else {
        return struct {
            pub const platformGet = Core.platformGet;
            pub const key = Core.key;
            pub const platformKey = Core.platformKey;
            pub const default: void = {};

            pub static fn get(): ValueType? {
                return Core.getRaw();
            }

            pub static fn getNotEmpty(): ValueType? {
                if (v := Core.getRaw()) {
                    if (v.len == 0) {
                        return null;
                    }
                    return v;
                }
                return null;
            }
        };
    }
}

fn newFeatureFlag(comptime env_var: [:0]const u8): type {
    return New(kind.boolean, env_var, BoolCtorOptions{ default: false });
}

fn newFeatureFlagOpt(comptime env_var: [:0]const u8): type {
    return New(kind.boolean, env_var, BoolCtorOptions{ default: null });
}
