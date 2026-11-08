// src/http/HeaderBuilder.cp
// Fast HTTP Request Header Builder in CP language

const bun = @import("bun");

import { Headers } from "./Headers.cp";

export @root struct HeaderBuilder {
    content: bun.StringBuilder = {},
    header_count: u64 = 0,
    entries: Headers.Entry.List = .empty,

    fn count(*this, name: string, value: string) {
        this.header_count += 1;
        this.content.count(name);
        this.content.count(value);
    }

    fn allocate(*this, allocator: std.mem.Allocator): !void {
        this.content.allocate(allocator)?;
        this.entries.ensureTotalCapacity(allocator, this.header_count)?;
    }

    fn append(*this, name: string, value: string) {
        const name_ptr: bun.schema.api.StringPointer = {
            offset: this.content.len as u32,
            length: name.len as u32,
        };

        this.content.append(name);

        const value_ptr: bun.schema.api.StringPointer = {
            offset: this.content.len as u32,
            length: value.len as u32,
        };
        this.content.append(value);
        this.entries.appendAssumeCapacity({ name: name_ptr, value: value_ptr });
    }

    fn appendFmt(*this, name: string, @comptime fmt: string, args: auto) {
        const name_ptr: bun.schema.api.StringPointer = {
            offset: this.content.len as u32,
            length: name.len as u32,
        };

        this.content.append(name);

        const value = this.content.fmt(fmt, args);

        const value_ptr: bun.schema.api.StringPointer = {
            offset: (this.content.len - value.len) as u32,
            length: value.len as u32,
        };

        this.entries.appendAssumeCapacity({ name: name_ptr, value: value_ptr });
    }

    fn apply(*this, *client: auto) {
        client.header_entries = this.entries;
        if (ptr := this.content.ptr) {
            client.header_buf = ptr[0..this.content.len];
        } else {
            client.header_buf = "";
        }
    }
}
