// src/http/HeaderBuilder.cp
// Fast HTTP Request Header Builder in CP language

zig {
    const HTTPClient = @import("./http.zig");
    const bun = @import("bun");
    const StringBuilder = bun.StringBuilder;
    const Headers = bun.http.Headers;
    const api = bun.schema.api;
}

export @root struct HeaderBuilder {
    content: StringBuilder = {},
    header_count: u64 = 0,
    entries: Headers.Entry.List = .empty,

    pub fn count(*this, name: string, value: string): void {
        this.header_count += 1;
        this.content.count(name);
        this.content.count(value);
    }

    pub fn allocate(*this, allocator: std.mem.Allocator)! {
        this.content.allocate(allocator)?;
        this.entries.ensureTotalCapacity(allocator, this.header_count)?;
    }

    pub fn append(*this, name: string, value: string): void {
        const name_ptr: api.StringPointer = {
            offset: @as(u32, @truncate(this.content.len)),
            length: @as(u32, @truncate(name.len)),
        };

        this.content.append(name);

        const value_ptr: api.StringPointer = {
            offset: @as(u32, @truncate(this.content.len)),
            length: @as(u32, @truncate(value.len)),
        };
        this.content.append(value);
        this.entries.appendAssumeCapacity({ name: name_ptr, value: value_ptr });
    }

    pub fn appendFmt(*this, name: string, @comptime fmt: string, args: anytype): void {
        const name_ptr: api.StringPointer = {
            offset: @as(u32, @truncate(this.content.len)),
            length: @as(u32, @truncate(name.len)),
        };

        this.content.append(name);

        const value = this.content.fmt(fmt, args);

        const value_ptr: api.StringPointer = {
            offset: @as(u32, @truncate(this.content.len - value.len)),
            length: @as(u32, @truncate(value.len)),
        };

        this.entries.appendAssumeCapacity({ name: name_ptr, value: value_ptr });
    }

    pub fn apply(*this, client: *HTTPClient): void {
        client.header_entries = this.entries;
        if (ptr := this.content.ptr) {
            client.header_buf = ptr[0..this.content.len];
        } else {
            client.header_buf = "";
        }
    }
}
