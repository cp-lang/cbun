// src/http_types/ETag.cp
// Bun HTTP ETag and If-None-Match header evaluation in CP language

import bun from "bun";

export struct ParsedETag {
    tag: string,
    is_weak: bool,
}

fn parse(tag_str: string): ParsedETag {
    let mut str = bun.strings.trim(tag_str, " \t");

    let mut is_weak = false;
    if (bun.strings.hasPrefix(str, "W/")) {
        is_weak = true;
        str = str[2..];
        str = bun.strings.trimLeft(str, " \t");
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
    return tag1 == tag2;
}

pub fn appendToHeaders(bytes: string, headers: *bun.http.Headers): ?void {
    let hash = bun.hash.XxHash64.hash(0, bytes);
    let etag_buf: u8[40] = undefined;
    let etag_str = bun.fmt.bufPrint(&etag_buf, "\"{f}\"", (bun.fmt.hexIntLower(hash)));
    headers.append("etag", etag_str)?;
}

pub fn ifNoneMatch(etag: string, if_none_match: string): bool {
    const our_parsed = parse(etag);

    if (bun.strings.trim(if_none_match, " \t") == "*") {
        return true;
    }

    let mut iter = bun.strings.split(if_none_match, ",");
    while (tag_str := iter.next()) {
        const parsed = parse(tag_str);
        if (weakMatch(our_parsed.tag, our_parsed.is_weak, parsed.tag, parsed.is_weak)) {
            return true;
        }
    }

    return false;
}
