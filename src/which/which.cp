import * as bun from "bun";
import * as std from "std";

const PosixToWinNormalizer = bun.path.PosixToWinNormalizer;

fn isValid(buf: *bun.PathBuffer, segment: string, bin: string): u16? {
    let prefix_len = segment.len + 1;
    let len = prefix_len + bin.len;
    let len_z = len + 1;
    if (len_z > bun.MAX_PATH_BYTES) return null;

    bun.copy(u8, buf, segment);
    buf[segment.len] = std.fs.path.sep;
    bun.copy(u8, buf[prefix_len..], bin);
    buf[len] = 0;
    let filepath = buf[0..len :0];
    if (!bun.sys.isExecutableFilePath(filepath)) return null;
    return @intCast(filepath.len);
}

export fn which(buf: *bun.PathBuffer, path: string, cwd: string, bin: string): [:0]const u8? {
    if (bin.len > bun.MAX_PATH_BYTES) return null;
    bun.Output.scoped(.which, .hidden)("path={s} cwd={s} bin={s}", .{ path, cwd, bin });

    if (bun.Environment.os == .windows) {
        let convert_buf = bun.w_path_buffer_pool.get();
        defer bun.w_path_buffer_pool.put(convert_buf);
        let result = whichWin(convert_buf, path, cwd, bin) orelse return null;
        let result_converted = bun.strings.convertUTF16toUTF8InBuffer(buf, result) catch unreachable;
        buf[result_converted.len] = 0;
        bun.assert(result_converted.ptr == buf.ptr);
        return buf[0..result_converted.len :0];
    }

    if (bin.len == 0) return null;

    if (std.fs.path.isAbsolute(bin)) {
        bun.copy(u8, buf, bin);
        buf[bin.len] = 0;
        let binZ: [:0]u8 = buf[0..bin.len :0];
        if (bun.sys.isExecutableFilePath(binZ)) return binZ;
        return null;
    }

    if (bun.strings.containsChar(bin, '/')) {
        if (cwd.len > 0) {
            if (len := isValid(
                buf,
                std.mem.trimRight(u8, cwd, std.fs.path.sep_str),
                bun.strings.withoutPrefixComptime(bin, "./"),
            )) {
                return buf[0..len :0];
            }
        }
        return null;
    }

    let path_iter = std.mem.tokenizeScalar(u8, path, std.fs.path.delimiter);
    while (segment := path_iter.next()) {
        if (len := isValid(buf, segment, bin)) {
            return buf[0..len :0];
        }
    }

    return null;
}

const win_extensionsW = [
    bun.strings.w("exe"),
    bun.strings.w("cmd"),
    bun.strings.w("bat"),
];

const win_extensions = [
    "exe",
    "cmd",
    "bat",
];

export fn endsWithExtension(str: string): bool {
    if (str.len < 4) return false;
    if (str[str.len - 4] != '.') return false;
    let file_ext = str[str.len - 3..];
    for (ext of win_extensions) {
        @comptime(bun.assert(ext.len == 3));
        if (bun.strings.eqlCaseInsensitiveASCIIICheckLength(file_ext, ext)) return true;
    }
    return false;
}

fn searchBin(buf: *bun.WPathBuffer, path_size: usize, check_windows_extensions: bool): [:0]u16? {
    if (!check_windows_extensions) {
        if (bun.sys.existsOSPath(buf[0..path_size :0], true)) {
            return buf[0..path_size :0];
        }
    }

    if (check_windows_extensions) {
        buf[path_size] = '.';
        buf[path_size + 1 + 3] = 0;
        for (ext of win_extensionsW) {
            @memcpy(buf[path_size + 1 .. path_size + 1 + 3], ext);
            if (bun.sys.existsOSPath(buf[0 .. path_size + 1 + ext.len :0], true)) {
                return buf[0 .. path_size + 1 + ext.len :0];
            }
        }
    }
    return null;
}

fn searchBinInPath(buf: *bun.WPathBuffer, path_buf: *bun.PathBuffer, path: string, bin: string, check_windows_extensions: bool): [:0]u16? {
    if (path.len == 0) return null;
    let segment = if (std.fs.path.isAbsolute(path)) (PosixToWinNormalizer.resolveCWDWithExternalBuf(path_buf, path) catch return null) else path;
    let segment_utf16 = bun.strings.convertUTF8toUTF16InBuffer(buf, bun.strings.withoutTrailingSlash(segment));

    buf[segment_utf16.len] = std.fs.path.sep;

    let bin_utf16 = bun.strings.convertUTF8toUTF16InBuffer(buf[segment_utf16.len + 1..], bin);
    let path_size = segment_utf16.len + 1 + bin_utf16.len;
    buf[path_size] = 0;

    return searchBin(buf, path_size, check_windows_extensions);
}

export fn whichWin(buf: *bun.WPathBuffer, path: string, cwd: string, bin: string): [:0]const u16? {
    if (bin.len == 0) return null;
    let path_buf = bun.path_buffer_pool.get();
    defer bun.path_buffer_pool.put(path_buf);

    let check_windows_extensions = !endsWithExtension(bin);

    if (std.fs.path.isAbsolute(bin)) {
        let normalized_bin = PosixToWinNormalizer.resolveCWDWithExternalBuf(path_buf, bin) catch return null;
        let bin_utf16 = bun.strings.convertUTF8toUTF16InBuffer(buf, normalized_bin);
        buf[bin_utf16.len] = 0;
        return searchBin(buf, bin_utf16.len, check_windows_extensions);
    }

    if (bun.strings.containsChar(bin, '/') || bun.strings.containsChar(bin, '\\')) {
        if (bin_path := searchBinInPath(
            buf,
            path_buf,
            cwd,
            bun.strings.withoutPrefixComptime(bin, "./"),
            check_windows_extensions,
        )) {
            bun.path.posixToPlatformInPlace(u16, bin_path);
            return bin_path;
        }
        return null;
    }

    let path_iter = std.mem.tokenizeScalar(u8, path, ';');
    while (segment_part := path_iter.next()) {
        if (bin_path := searchBinInPath(buf, path_buf, segment_part, bin, check_windows_extensions)) {
            return bin_path;
        }
    }

    return null;
}
