import bun from "bun";
import std from "std";

const strings = bun.strings;
import { PercentEncoding } from "../url/url.zig";

export @root struct URLPath {
    extname: string = "",
    path: string = "",
    pathname: string = "",
    first_segment: string = "",
    query_string: string = "",
    needs_redirect: bool = false,
    is_source_map: bool = false,

    pub fn isRoot(this, asset_prefix: string): bool {
        const without = this.pathWithoutAssetPrefix(asset_prefix);
        if (without.len == 1 && without[0] == '.') {
            return true;
        }
        return strings.eqlComptime(without, "index");
    }

    pub fn pathWithoutAssetPrefix(this, asset_prefix: string): string {
        if (asset_prefix.len == 0) {
            return this.path;
        }
        let leading_slash_offset: usize = 0;
        if (asset_prefix[0] == '/') {
            leading_slash_offset = 1;
        }
        const base = this.path;
        const origin = asset_prefix[leading_slash_offset..];

        let out = base;
        if (base.len >= origin.len && strings.eql(base[0..origin.len], origin)) {
            out = base[origin.len..];
        }
        if (this.is_source_map && strings.endsWithComptime(out, ".map")) {
            return out[0..out.len - 4];
        }

        return out;
    }
}

@tls let temp_path_buf: [1024]u8 = undefined;
@tls let big_temp_path_buf: [16384]u8 = undefined;

pub fn parse(possibly_encoded_pathname_: string): !URLPath {
    let decoded_pathname = possibly_encoded_pathname_;
    let needs_redirect = false;

    if (strings.containsChar(decoded_pathname, '%')) {
        let possibly_encoded_pathname = match (decoded_pathname.len) {
            0..1024 => temp_path_buf[0..],
            else => big_temp_path_buf[0..],
        };
        possibly_encoded_pathname = possibly_encoded_pathname[0..@min(
            possibly_encoded_pathname_.len,
            possibly_encoded_pathname.len,
        )];

        bun.copy(u8, possibly_encoded_pathname, possibly_encoded_pathname_[0..possibly_encoded_pathname.len]);
        let clone = possibly_encoded_pathname[0..possibly_encoded_pathname.len];

        let fbs = std.io.fixedBufferStream(possibly_encoded_pathname);
        let writer = fbs.writer();

        decoded_pathname = @as(string, possibly_encoded_pathname[0..PercentEncoding.decodeFaultTolerant(@TypeOf(writer), writer, clone, *needs_redirect, true)?]);
    }

    let question_mark_i: i16 = -1;
    let period_i: i16 = -1;

    let first_segment_end: i16 = std.math.maxInt(i16);
    let last_slash: i16 = -1;

    let i: i16 = @as(i16, @intCast(decoded_pathname.len)) - 1;

    while (i >= 0) {
        let c = decoded_pathname[@as(usize, @intCast(i))];

        match (c) {
            '?' => {
                question_mark_i = @max(question_mark_i, i);
                if (question_mark_i < period_i) {
                    period_i = -1;
                }

                if (last_slash > question_mark_i) {
                    last_slash = -1;
                }
            },
            '.' => {
                period_i = @max(period_i, i);
            },
            '/' => {
                last_slash = @max(last_slash, i);

                if (i > 0) {
                    first_segment_end = @min(first_segment_end, i);
                }
            },
            else => {},
        }
        i -= 1;
    }

    if (last_slash > period_i) {
        period_i = -1;
    }

    let extname: string = if (question_mark_i > -1 && period_i > -1)
        decoded_pathname[@as(usize, @intCast(period_i + 1))..@as(usize, @intCast(question_mark_i))]
    else if (period_i > -1)
        decoded_pathname[@as(usize, @intCast(period_i + 1))..]
    else
        "";

    let path = if (question_mark_i < 0) decoded_pathname[1..] else decoded_pathname[1..@as(usize, @intCast(question_mark_i))];

    let first_segment = decoded_pathname[1..@min(@as(usize, @intCast(first_segment_end)), decoded_pathname.len)];
    let is_source_map = strings.eqlComptime(extname, "map");
    let backup_extname: string = extname;
    if (is_source_map && path.len > ".map".len) {
        if (j := std.mem.lastIndexOfScalar(u8, path[0..path.len - ".map".len], '.')) {
            backup_extname = path[j + 1..];
            backup_extname = backup_extname[0..backup_extname.len - ".map".len];
            path = path[0..j + backup_extname.len + 1];
        }
    }

    return URLPath {
        extname: if (!is_source_map) extname else backup_extname,
        is_source_map: is_source_map,
        pathname: decoded_pathname,
        first_segment: first_segment,
        path: if (decoded_pathname.len == 1) "." else path,
        query_string: if (question_mark_i > -1) decoded_pathname[@as(usize, @intCast(question_mark_i))..@as(usize, @intCast(decoded_pathname.len))] else "",
        needs_redirect: needs_redirect,
    };
}
