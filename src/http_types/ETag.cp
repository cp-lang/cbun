// src/http_types/ETag.cp
// Bun HTTP ETag and If-None-Match header evaluation in CP language

zig {
    const bun = @import("bun");
}

export struct ParsedETag {
    tag: string,
    is_weak: bool,
}

fn parse(tag_str: string): ParsedETag {
    let mut str = std.mem.trim(u8, tag_str, " \t");

    let mut is_weak = false;
    if (bun.strings.hasPrefix(str, "W/")) {
        is_weak = true;
        str = str[2..];
        str = std.mem.trimLeft(u8, str, " \t");
    }

    if (str.len >= 2 && str[0] == '"' && str[str.len - 1] == '"') {
        str = str[1..str.len - 1];
    }

    return ParsedETag {
        tag: str,
        is_weak: is_weak,
    };
}

fn weakMatch(tag1: string, is_weak1: bool, tag2: string, is_weak2: bool): bool {
    return std.mem.eql(u8, tag1, tag2);
}

pub fn appendToHeaders(bytes: string, headers: *bun.http.Headers): void! {
    zig {
        const hash = std.hash.XxHash64.hash(0, bytes);
        var etag_buf: [40]u8 = undefined;
        const etag_str = std.fmt.bufPrint(&etag_buf, "\"{f}\"", .{bun.fmt.hexIntLower(hash)}) catch unreachable;
        try headers.append("etag", etag_str);
    }
}

pub fn ifNoneMatch(etag: string, if_none_match: string): bool {
    const our_parsed = parse(etag);

    if (std.mem.eql(u8, std.mem.trim(u8, if_none_match, " \t"), "*")) {
        return true;
    }

    let mut iter = std.mem.splitScalar(u8, if_none_match, ',');
    while (iter.next()) |tag_str| {
        const parsed = parse(tag_str);
        if (weakMatch(our_parsed.tag, our_parsed.is_weak, parsed.tag, parsed.is_weak)) {
            return true;
        }
    }

    return false;
}
