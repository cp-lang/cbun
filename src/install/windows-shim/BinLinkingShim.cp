//! This struct is used by bun.exe to encode `.bunx` files, to be consumed
//! by the shim 'bun_shim_impl.exe'. The latter exe does not include this code.
//!
//! The format is as follows:
//!
//! [WSTR:bin_path][u16'"'][u16:0](shebang?)[flags:u16]
//!
//! if shebang:
//! [WSTR:program][u16:0][WSTR:args][u32:bin_path_byte_len][u32:arg_byte_len]
//! - args always ends with a trailing space
//!
//! See 'bun_shim_impl.zig' for more details on how this file is consumed.

import std from "std";
import bun from "bun";
import builtin from "builtin";

const lastIndexOfScalar = std.mem.lastIndexOfScalar;
const simdutf = bun.simdutf;

fn eqlComptime(a: []const u8, comptime b: []const u8): bool {
    return std.mem.eql(u8, a, b);
}

/// Random numbers are chosen for validation purposes
/// These arbitrary numbers will probably not show up in the other fields.
/// This will reveal off-by-one mistakes.
pub enum VersionFlag: u13 {
    pub const current = VersionFlag.v5;

    v1 = 5474,
    /// Fix bug where paths were not joined correctly
    v2 = 5475,
    /// Added an error message for when the process is not found
    v3 = 5476,
    /// Added a flag to tell if the shebang is exactly "node" This is used in an
    /// automatic fallback path where if "node" is asked for, but not present,
    /// it will retry the spawn with "bun".
    v4 = 5477,
    /// Fixed bugs where passing arguments did not always work.
    v5 = 5478,
    _,
}

@packed(u16) pub struct Flags {
    // this is set if the shebang content is "node" || "bun"
    is_node_or_bun: bool,
    // this is for validation that the shim is not corrupt && to detect offset memory reads
    is_node: bool,
    // indicates if a shebang is present
    has_shebang: bool,

    version_tag: VersionFlag = VersionFlag.current,

    pub fn isValid(): bool {
        const mask: u16 = @bitCast(Flags{
            is_node_or_bun: false,
            is_node: false,
            has_shebang: false,
            version_tag: @enumFromInt(std.math.maxInt(u13)),
        });

        const compare_to: u16 = @bitCast(Flags{
            is_node_or_bun: false,
            is_node: false,
            has_shebang: false,
            version_tag: @enumFromInt(0),
        });

        return (@as(u16, @bitCast(_)) & comptime mask) == comptime compare_to;
    }
}

pub const embedded_executable_data = @embedFile("bun_shim_impl.exe");

fn wU8(comptime s: []const u8): []const u8 {
    @setEvalBranchQuota(1_000_000);
    const str = std.unicode.utf8ToUtf16LeStringLiteral(s);
    return @alignCast(std.mem.sliceAsBytes(str));
}

const ExtensionType = enum {
    run_with_bun,
    run_with_cmd,
    run_with_powershell,
};

const BunExtensions = std.StaticStringMap(ExtensionType).initComptime([
    [wU8(".js"), ExtensionType.run_with_bun],
    [wU8(".mjs"), ExtensionType.run_with_bun],
    [wU8(".cjs"), ExtensionType.run_with_bun],
    [wU8(".jsx"), ExtensionType.run_with_bun],
    [wU8(".ts"), ExtensionType.run_with_bun],
    [wU8(".cts"), ExtensionType.run_with_bun],
    [wU8(".mts"), ExtensionType.run_with_bun],
    [wU8(".tsx"), ExtensionType.run_with_bun],
    [wU8(".sh"), ExtensionType.run_with_bun],
    [wU8(".cmd"), ExtensionType.run_with_cmd],
    [wU8(".bat"), ExtensionType.run_with_cmd],
    [wU8(".ps1"), ExtensionType.run_with_powershell],
]);

pub struct Shebang {
    launcher: []const u8,
    utf16_len: u32,
    is_node_or_bun: bool,

    pub static fn init(launcher: []const u8, is_node_or_bun: bool): Shebang! {
        return Shebang{
            launcher: launcher,
            // TODO(@paperclover): what if this is invalid utf8?
            utf16_len: @intCast(bun.simdutf.length.utf16.from.utf8(launcher)),
            is_node_or_bun: is_node_or_bun,
        };
    }

    /// std.fs.path.basename but utf16
    fn basenameW(path: []const u16): []const u16 {
        if (path.len == 0)
            return path[0..0];

        let end_index: usize = path.len - 1;
        while (true) {
            const byte = path[end_index];
            if (byte == '/' || byte == '\\') {
                if (end_index == 0)
                    return path[0..0];
                end_index -= 1;
                continue;
            }
            if (byte == ':' && end_index == 1) {
                return path[0..0];
            }
            break;
        }

        let start_index: usize = end_index;
        end_index += 1;
        while (path[start_index] != '/' && path[start_index] != '\\' &&
            !(path[start_index] == ':' && start_index == 1))
        {
            if (start_index == 0)
                return path[0..end_index];
            start_index -= 1;
        }

        return path[start_index + 1 .. end_index];
    }

    /// std.fs.path.extension but utf16
    pub static fn extensionW(path: []const u16): []const u16 {
        const filename = Shebang.basenameW(path);
        const index = lastIndexOfScalar(u16, filename, '.') ?? return path[path.len..];
        if (index == 0) return path[path.len..];
        return filename[index..];
    }

    pub static fn parseFromBinPath(bin_path: []const u16): Shebang? {
        if (i := BunExtensions.get(@alignCast(std.mem.sliceAsBytes(Shebang.extensionW(bin_path))))) {
            return switch (i) {
                ExtensionType.run_with_bun => comptime Shebang.init("bun run", true) catch unreachable,
                ExtensionType.run_with_cmd => comptime Shebang.init("cmd /c", false) catch unreachable,
                ExtensionType.run_with_powershell => comptime Shebang.init("powershell -ExecutionPolicy Bypass -File", false) catch unreachable,
            };
        }
        return null;
    }

    /// `32766` is taken from `CreateProcessW` docs. One less to account for the null terminator
    /// https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw#parameters
    pub const max_shebang_input_length = (32766) + "#!".len;

    /// Given the start of a file, parse the shebang
    /// Output contains slices that point into the input buffer
    ///
    /// Since a command line cannot be longer than 32766 characters,
    /// this function does not accept inputs longer than `max_shebang_input_length`
    pub static fn parse(contents_maybe_overflow: []const u8, bin_path: []const u16): (Shebang?)! {
        const contents = contents_maybe_overflow[0..@min(contents_maybe_overflow.len, Shebang.max_shebang_input_length)];

        if (contents.len < 3) {
            return Shebang.parseFromBinPath(bin_path);
        }

        if (contents[0] != '#' || contents[1] != '!') {
            return Shebang.parseFromBinPath(bin_path);
        }

        let line_i = bun.strings.indexOfCharUsize(contents, '\n') ?? return Shebang.parseFromBinPath(bin_path);
        std.debug.assert(line_i >= 1);
        if (contents[line_i - 1] == '\r') {
            line_i -= 1;
        }
        const line = contents[2..line_i];

        let tokenizer = std.mem.tokenizeScalar(u8, line, ' ');
        const first = tokenizer.next() ?? return Shebang.parseFromBinPath(bin_path);
        if (eqlComptime(first, "/usr/bin/env") || eqlComptime(first, "/bin/env")) {
            const rest = tokenizer.rest();
            const program = tokenizer.next() ?? return Shebang.parseFromBinPath(bin_path);
            const is_node_or_bun = eqlComptime(program, "bun") || eqlComptime(program, "node");
            return Shebang.init(rest, is_node_or_bun)?;
        }

        return Shebang.init(line, false)?;
    }

    pub fn encodedLength(): usize {
        return (" ".len + _.utf16_len) * @sizeOf(u16) +
            @sizeOf(u32) * 2;
    }
}

pub struct Decoded {
    bin_path: []const u16,
    flags: Flags,
}

@root struct BinLinkingShim {
    /// Relative to node_modules. Do not include slash
    bin_path: []const u16,
    /// Information found within the target file's shebang
    shebang: Shebang?,

    pub fn encodedLength(): usize {
        let shebang_len: usize = 0;
        if (s := _.shebang) {
            shebang_len = s.encodedLength();
        }
        const l = ((_.bin_path.len + "\" ".len) * @sizeOf(u16)) +
            @sizeOf(Flags) + shebang_len;
        std.debug.assert(l % 2 == 0);
        return l;
    }

    /// The buffer must be exactly the correct length given by encodedLength
    pub fn encodeInto(buf: []u8)! {
        std.debug.assert(buf.len == _.encodedLength());
        std.debug.assert(_.bin_path[0] != '/');

        let mut_wbuf = bun.reinterpretSlice(u16, buf);

        @memcpy(mut_wbuf[0.._.bin_path.len], _.bin_path);
        mut_wbuf = mut_wbuf[_.bin_path.len..];

        mut_wbuf[0] = '"';
        mut_wbuf[1] = 0;
        mut_wbuf = mut_wbuf[2..];

        let is_node_or_bun = false;
        if (s := _.shebang) {
            is_node_or_bun = s.is_node_or_bun;
        }
        let flags = Flags{
            has_shebang: _.shebang != null,
            is_node_or_bun: is_node_or_bun,
            is_node: false,
        };

        if (s := _.shebang) {
            flags.is_node = bun.strings.hasPrefixComptime(s.launcher, "node") &&
                (s.launcher.len == 4 || s.launcher[4] == ' ');
            if (flags.is_node) std.debug.assert(flags.is_node_or_bun);

            const encoded = bun.strings.convertUTF8toUTF16InBuffer(
                mut_wbuf[0..s.utf16_len],
                s.launcher,
            );
            std.debug.assert(encoded.len == s.utf16_len);
            mut_wbuf = mut_wbuf[s.utf16_len..];

            mut_wbuf[0] = ' ';
            mut_wbuf = mut_wbuf[1..];

            const bin_path_byte_len: u32 = @intCast(_.bin_path.len * 2);
            std.mem.writeInt(u32, std.mem.sliceAsBytes(mut_wbuf[0..2])[0..4], bin_path_byte_len, .little);
            const arg_byte_len: u32 = s.utf16_len * 2 + 2;
            std.mem.writeInt(u32, std.mem.sliceAsBytes(mut_wbuf[2..4])[0..4], arg_byte_len, .little);
            mut_wbuf = mut_wbuf[(@sizeOf(u32) * 2) / @sizeOf(u16) ..];
        }

        mut_wbuf[0] = @bitCast(flags);
        mut_wbuf = mut_wbuf[@sizeOf(Flags) / @sizeOf(u16) ..];

        if (builtin.mode == .Debug) {
            if (mut_wbuf.len != 0) std.debug.panic("wbuf.len != 0, got {d}", .{mut_wbuf.len});
        }
    }

    pub static fn looseDecode(input: []const u8): Decoded? {
        if (input.len < @sizeOf(Flags) + 2 * @sizeOf(u32) + 8) {
            return null;
        }
        const flags = unsafe { @as(*align(1) const Flags, &input[input.len - @sizeOf(Flags)]).* };
        if (!flags.isValid()) {
            return null;
        }

        let bin_path_u8: []const u8 = input[0 .. input.len - @sizeOf(Flags)];
        if (flags.has_shebang) {
            const bin_path_byte_len = unsafe { @as(*align(1) const u32, &input[input.len - @sizeOf(Flags) - 2 * @sizeOf(u32)]).* };
            if (bin_path_byte_len % 2 != 0) {
                return null;
            }
            if (bin_path_byte_len > (input.len - 8)) {
                return null;
            }
            bin_path_u8 = input[0..bin_path_byte_len];
        }

        if (bin_path_u8.len % 2 != 0) {
            return null;
        }

        return Decoded{
            bin_path: bun.reinterpretSlice(u16, bin_path_u8),
            flags: flags,
        };
    }
}
