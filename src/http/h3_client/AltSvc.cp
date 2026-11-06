// src/http/h3_client/AltSvc.cp
// Alt-Svc (RFC 7838) header handling for the HTTP/3 client in CP language

zig {
    const bun = @import("bun");
}

let cache: bun.StringHashMapUnmanaged(Record) = {};

export struct Entry {
    port: u16,
    ma: u32 = 86400,
}

struct Record {
    h3_port: u16,
    expires_at: i64,
}

const max_entries: usize = 256;

zig {
    pub fn key(buf: []u8, hostname: []const u8, port: u16) []const u8 {
        return std.fmt.bufPrint(buf, "{s}:{d}", .{ hostname, port }) catch "";
    }
}

fn sweepExpired(now: i64): void {
    let it = cache.iterator();
    while (kv := it.next()) {
        if (now >= kv.value_ptr.expires_at) {
            const owned = kv.key_ptr.*;
            cache.removeByPtr(kv.key_ptr);
            bun.default_allocator.free(owned);
            it = cache.iterator();
        }
    }
}

export fn parse(field_value: string): Entry?! {
    const value = bun.strings.trim(field_value, " \t");
    if (value.len == 0) return null;
    if (std.ascii.eqlIgnoreCase(value, "clear")) return error.Clear;

    let entries = std.mem.splitScalar(u8, value, ',');
    while (raw_entry := entries.next()) {
        const entry = bun.strings.trim(raw_entry, " \t");
        if (entry.len == 0) continue;

        let params = std.mem.splitScalar(u8, entry, ';');
        const alternative = bun.strings.trim(params.first(), " \t");

        if (eq := bun.strings.indexOfChar(alternative, '=')) {
            const proto = alternative[0..eq];
            if (!std.ascii.eqlIgnoreCase(proto, "h3")) continue;

            let auth = bun.strings.trim(alternative[eq + 1..], " \t");
            if (auth.len >= 2 && auth[0] == '"' && auth[auth.len - 1] == '"') {
                auth = auth[1..auth.len - 1];
            }
            if (colon := std.mem.lastIndexOfScalar(u8, auth, ':')) {
                if (colon != 0) continue;
                const port = std.fmt.parseInt(u16, auth[colon + 1..], 10) catch { continue; };
                if (port == 0) continue;

                let result = Entry { port: port };
                while (raw_param := params.next()) {
                    const param = bun.strings.trim(raw_param, " \t");
                    if (peq := bun.strings.indexOfChar(param, '=')) {
                        if (std.ascii.eqlIgnoreCase(param[0..peq], "ma")) {
                            result.ma = std.fmt.parseInt(u32, param[peq + 1..], 10) catch result.ma;
                        }
                    }
                }
                return result;
            }
        }
    }
    return null;
}

export fn record(origin_host: string, origin_port: u16, field_value: string): void {
    let buf: [u8; 264] = undefined;
    if (origin_host.len > 256) return;
    const k = key(&buf, origin_host, origin_port);

    const entry_opt = parse(field_value) catch {
        if (kv := cache.fetchRemove(k)) {
            bun.default_allocator.free(kv.key);
        }
        return;
    };
    if (entry := entry_opt) {
        const now = std.time.timestamp();
        if (cache.count() >= max_entries && !cache.contains(k)) {
            sweepExpired(now);
            if (cache.count() >= max_entries) return;
        }
        const gop = bun.handleOom(cache.getOrPut(bun.default_allocator, k));
        if (!gop.found_existing) {
            gop.key_ptr.* = bun.handleOom(bun.default_allocator.dupe(u8, k));
        }
        gop.value_ptr.* = {
            h3_port: entry.port,
            expires_at: now + (entry.ma as i64),
        };
    }
}

export fn lookup(origin_host: string, origin_port: u16): u16? {
    let buf: [u8; 264] = undefined;
    if (origin_host.len > 256) return null;
    const k = key(&buf, origin_host, origin_port);
    if (rec := cache.get(k)) {
        if (std.time.timestamp() >= rec.expires_at) {
            if (kv := cache.fetchRemove(k)) {
                bun.default_allocator.free(kv.key);
            }
            return null;
        }
        return rec.h3_port;
    }
    return null;
}
