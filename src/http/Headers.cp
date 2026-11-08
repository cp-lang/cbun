// src/http/Headers.cp
// Fast HTTP Headers collection and parser in CP language

const bun = @import("bun");

import { toFetchHeaders } from "../http_jsc/headers_jsc.zig";

export struct Entry {
    name: bun.schema.api.StringPointer,
    value: bun.schema.api.StringPointer,

    pub const List = bun.MultiArrayList(Entry);
}

export struct Options {
    body: *const bun.webcore.Blob.Any? = null,
}

export @root @value struct Headers {
    entries: Entry.List = {},
    buf: std.ArrayListUnmanaged(u8) = {},
    allocator: std.mem.Allocator,

    fn memoryCost(this): usize {
        return this.buf.items.len + this.entries.memoryCost();
    }

    fn clone(*this): Headers! {
        return {
            entries: this.entries.clone(this.allocator)?,
            buf: this.buf.clone(this.allocator)?,
            allocator: this.allocator,
        };
    }

    fn get(this, name: string): string? {
        const entries = this.entries.slice();
        const names = entries.items(.name);
        const values = entries.items(.value);
        let i: usize = 0;
        while (i < names.len) {
            if (bun.strings.eqlCaseInsensitiveASCII(this.asStr(names[i]), name, true)) {
                return this.asStr(values[i]);
            }
            i += 1;
        }
        return null;
    }

    fn append(*this, name: string, value: string): !void {
        let offset: u32 = @truncate(this.buf.items.len);
        this.buf.ensureUnusedCapacity(this.allocator, name.len + value.len)?;
        const name_ptr: bun.schema.api.StringPointer = {
            offset: offset,
            length: @truncate(name.len),
        };
        this.buf.appendSliceAssumeCapacity(name);
        offset = @truncate(this.buf.items.len);
        this.buf.appendSliceAssumeCapacity(value);

        const value_ptr: bun.schema.api.StringPointer = {
            offset: offset,
            length: @truncate(value.len),
        };
        this.entries.append(this.allocator, {
            name: name_ptr,
            value: value_ptr,
        })?;
    }

    fn release(*this) {
        this.entries.deinit(this.allocator);
        this.buf.clearAndFree(this.allocator);
    }

    fn deinit(*this) {
        this.release();
    }

    fn getContentDisposition(this): string? {
        return this.get("content-disposition");
    }

    fn getContentEncoding(this): string? {
        return this.get("content-encoding");
    }

    fn getContentType(this): string? {
        return this.get("content-type");
    }

    fn asStr(this, ptr: bun.schema.api.StringPointer): string {
        if (ptr.offset + ptr.length <= this.buf.items.len) {
            return this.buf.items[ptr.offset..][0..ptr.length];
        } else {
            return "";
        }
    }

    static fn fromPicoHttpHeaders(headers: []bun.picohttp.Header, allocator: std.mem.Allocator): Headers! {
        const header_count = headers.len;
        let result: Headers = {
            entries: {},
            buf: {},
            allocator: allocator,
        };

        let buf_len: usize = 0;
        let j: usize = 0;
        while (j < headers.len) {
            buf_len += headers[j].name.len + headers[j].value.len;
            j += 1;
        }
        bun.handleOom(result.entries.ensureTotalCapacity(allocator, header_count));
        result.entries.len = headers.len;
        bun.handleOom(result.buf.ensureTotalCapacityPrecise(allocator, buf_len));
        result.buf.items.len = buf_len;
        let offset: u32 = 0;
        let i: usize = 0;
        while (i < headers.len) {
            const header = headers[i];
            const name_offset = offset;
            bun.copy(u8, result.buf.items[offset..][0..header.name.len], header.name);
            offset += header.name.len as u32;
            const value_offset = offset;
            bun.copy(u8, result.buf.items[offset..][0..header.value.len], header.value);
            offset += header.value.len as u32;

            result.entries.set(i, {
                name: {
                    offset: name_offset,
                    length: header.name.len as u32,
                },
                value: {
                    offset: value_offset,
                    length: header.value.len as u32,
                },
            });
            i += 1;
        }
        return result;
    }

    static fn from(fetch_headers_ref: *bun.webcore.FetchHeaders?, allocator: std.mem.Allocator, options: Options): Headers! {
        let header_count: u32 = 0;
        let buf_len: u32 = 0;
        if (headers_ref := fetch_headers_ref) {
            headers_ref.count(*header_count, *buf_len);
        }
        let headers: Headers = {
            entries: {},
            buf: {},
            allocator: allocator,
        };
        const buf_len_before_content_type = buf_len;
        let needs_content_type = false;
        if (body := options.body) {
            let has_ct = false;
            if (headers_ref2 := fetch_headers_ref) {
                has_ct = headers_ref2.fastHas(.ContentType);
            }
            if (body.hasContentTypeFromUser() && !has_ct) {
                header_count += 1;
                buf_len += (body.contentType().len + "Content-Type".len) as u32;
                needs_content_type = true;
            }
        }
        bun.handleOom(headers.entries.ensureTotalCapacity(allocator, header_count));
        headers.entries.len = header_count;
        bun.handleOom(headers.buf.ensureTotalCapacityPrecise(allocator, buf_len));
        headers.buf.items.len = buf_len;
        let sliced = headers.entries.slice();
        let names = sliced.items(.name);
        let values = sliced.items(.value);
        if (headers_ref := fetch_headers_ref) {
            headers_ref.copyTo(names.ptr, values.ptr, headers.buf.items.ptr);
        }

        if (needs_content_type) {
            bun.copy(u8, headers.buf.items[buf_len_before_content_type..], "Content-Type");
            names[header_count - 1] = {
                offset: buf_len_before_content_type,
                length: "Content-Type".len as u32,
            };

            if (b := options.body) {
                bun.copy(u8, headers.buf.items[buf_len_before_content_type + "Content-Type".len..], b.contentType());
                values[header_count - 1] = {
                    offset: buf_len_before_content_type + ("Content-Type".len as u32),
                    length: b.contentType().len as u32,
                };
            }
        }

        return headers;
    }
}
