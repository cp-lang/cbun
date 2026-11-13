import std from "std";
import bun from "bun";
import c from "../picohttp_sys/picohttpparser.zig";

const Environment = bun.Environment;
const Output = bun.Output;
const StringBuilder = bun.StringBuilder;
const assert = bun.assert;
const strings = bun.strings;

let EMPTY_HEADERS: [0]Header = [];

export struct HeaderList {
    list: []Header = *EMPTY_HEADERS,

    pub fn get(this, name: string): string? {
        for (header of this.list) {
            if (strings.eqlCaseInsensitiveASCII(header.name, name, true)) {
                return header.value;
            }
        }
        return null;
    }

    pub fn getIfOtherIsAbsent(this, name: string, other: string): string? {
        let value: string? = null;
        for (header of this.list) {
            if (strings.eqlCaseInsensitiveASCII(header.name, other, true)) {
                return null;
            }

            if (value == null && strings.eqlCaseInsensitiveASCII(header.name, name, true)) {
                value = header.value;
            }
        }

        return value;
    }
}

export struct HeaderCURLFormatter {
    header: Header,

    pub fn format(this, writer: *std.Io.Writer): ! {
        const header = this.header;
        if (header.value.len > 0) {
            writer.print("-H \"{s}: {s}\"", [header.name, header.value])?;
        } else {
            writer.print("-H \"{s}\"", [header.name])?;
        }
    }
}

export struct Header {
    pub const List = HeaderList;
    pub const CURLFormatter = HeaderCURLFormatter;

    name: []const u8,
    value: []const u8,

    pub fn isMultiline(this): bool {
        return this.name.len == 0;
    }

    pub fn format(this, writer: *std.Io.Writer): ! {
        if (Output.enable_ansi_colors_stderr) {
            if (this.isMultiline()) {
                writer.print(@comptime(Output.prettyFmt("<r><cyan>{s}", true)), [this.value])?;
            } else {
                writer.print(@comptime(Output.prettyFmt("<r><cyan>{s}<r><d>: <r>{s}", true)), [this.name, this.value])?;
            }
        } else {
            if (this.isMultiline()) {
                writer.print(@comptime(Output.prettyFmt("<r><cyan>{s}", false)), [this.value])?;
            } else {
                writer.print(@comptime(Output.prettyFmt("<r><cyan>{s}<r><d>: <r>{s}", false)), [this.name, this.value])?;
            }
        }
    }

    pub fn count(this, builder: *StringBuilder): void {
        builder.count(this.name);
        builder.count(this.value);
    }

    pub fn clone(this, builder: *StringBuilder): Header {
        return Header{
            name: builder.append(this.name),
            value: builder.append(this.value),
        };
    }

    pub fn curl(this): HeaderCURLFormatter {
        const h = this;
        return HeaderCURLFormatter{ header: h };
    }
}

comptime {
    assert(@sizeOf(Header) == @sizeOf(c.phr_header));
    assert(@alignOf(Header) == @alignOf(c.phr_header));
}

export struct RequestCURLFormatter {
    request: Request,
    ignore_insecure: bool = false,
    body: string = "",

    static fn isPrintableBody(content_type: []const u8): bool {
        if (content_type.len == 0) return false;

        return bun.strings.hasPrefixComptime(content_type, "text/") ||
            bun.strings.hasPrefixComptime(content_type, "application/json") ||
            bun.strings.containsComptime(content_type, "json") ||
            bun.strings.hasPrefixComptime(content_type, "application/x-www-form-urlencoded");
    }

    pub fn format(this, writer: *std.Io.Writer): ! {
        const request = this.request;
        if (Output.enable_ansi_colors_stderr) {
            _ = writer.write(Output.prettyFmt("<r><d>[fetch] $<r> ", true))?;

            writer.print(Output.prettyFmt("<b><cyan>curl<r> <d>--http1.1<r> <b>\"{s}\"<r>", true), [request.path])?;
        } else {
            writer.print("curl --http1.1 \"{s}\"", [request.path])?;
        }

        if (!bun.strings.eqlComptime(request.method, "GET")) {
            writer.print(" -X {s}", [request.method])?;
        }

        if (this.ignore_insecure) {
            _ = writer.writeAll(" -k")?;
        }

        let content_type: string = "";

        for (header of request.headers) {
            _ = writer.writeAll(" ")?;
            if (content_type.len == 0) {
                if (bun.strings.eqlCaseInsensitiveASCII("content-type", header.name, true)) {
                    content_type = header.value;
                }
            }

            header.curl().format(writer)?;

            if (bun.strings.eqlCaseInsensitiveASCII("accept-encoding", header.name, true)) {
                _ = writer.writeAll(" --compressed")?;
            }
        }

        if (this.body.len > 0 && RequestCURLFormatter.isPrintableBody(content_type)) {
            _ = writer.writeAll(" --data-raw ")?;
            bun.js_printer.writeJSONString(this.body, @typeOf(writer), writer, .utf8)?;
        }
    }
}

export struct Request {
    pub const CURLFormatter = RequestCURLFormatter;

    method: []const u8,
    path: []const u8,
    minor_version: usize,
    headers: []const Header,
    bytes_read: u32 = 0,

    pub fn curl(this, ignore_insecure: bool, body: []const u8): RequestCURLFormatter {
        const r = this;
        return RequestCURLFormatter{
            request: r,
            ignore_insecure: ignore_insecure,
            body: body,
        };
    }

    pub fn clone(this, out_headers: []Header, builder: *StringBuilder): Request {
        let i: usize = 0;
        while (i < this.headers.len) : (i += 1) {
            out_headers[i] = this.headers[i].clone(builder);
        }

        return Request{
            method: builder.append(this.method),
            path: builder.append(this.path),
            minor_version: this.minor_version,
            headers: out_headers,
            bytes_read: this.bytes_read,
        };
    }

    pub fn format(this, writer: *std.Io.Writer): ! {
        if (Output.enable_ansi_colors_stderr) {
            _ = writer.write(Output.prettyFmt("<r><d>[fetch]<r> ", true))?;
        }
        writer.print("> HTTP/1.1 {s} {s}\n", [this.method, this.path])?;
        for (header of this.headers) {
            if (Output.enable_ansi_colors_stderr) {
                _ = writer.write(Output.prettyFmt("<r><d>[fetch]<r> ", true))?;
            }
            _ = writer.write("> ")?;
            writer.print("{f}\n", [header])?;
        }
    }

    pub static fn parse(buf: []const u8, src: []Header): Request! {
        let method: []const u8 = undefined;
        let path: []const u8 = undefined;
        let minor_version: c_int = undefined;
        let num_headers: usize = src.len;

        let rc: c_int = 0;
        unsafe {
            rc = c.phr_parse_request(
                buf.ptr,
                buf.len,
                (&method.ptr) as [*c][*c]const u8,
                *method.len!,
                (&path.ptr) as [*c][*c]const u8,
                *path.len!,
                *minor_version!,
                src.ptr as *c.phr_header!,
                *num_headers!,
                0,
            );
            if (rc > -1) {
                let raw_path: [*]u8! = @ptrFromInt(@intFromPtr(path.ptr));
                raw_path[path.len] = 0;
            }
        }

        if (rc == -1) return err.BadRequest;
        if (rc == -2) return err.ShortRead;
        return Request{
            method: method,
            path: path,
            minor_version: usize(minor_version),
            headers: src[0..num_headers],
            bytes_read: u32(rc),
        };
    }
}

struct StatusCodeFormatter {
    code: usize,

    pub fn format(this, writer: *std.Io.Writer): ! {
        if (Output.enable_ansi_colors_stderr) {
            if (this.code == 101 || (this.code >= 200 && this.code <= 299)) {
                writer.print(@comptime(Output.prettyFmt("<r><green>{d}<r>", true)), [this.code])?;
            } else if (this.code >= 300 && this.code <= 399) {
                writer.print(@comptime(Output.prettyFmt("<r><yellow>{d}<r>", true)), [this.code])?;
            } else {
                writer.print(@comptime(Output.prettyFmt("<r><red>{d}<r>", true)), [this.code])?;
            }
        } else {
            writer.print("{d}", [this.code])?;
        }
    }
}

export struct Response {
    minor_version: usize = 0,
    status_code: u32 = 0,
    status: string = "",
    headers: HeaderList = HeaderList{},
    bytes_read: c_int = 0,

    pub fn format(this, writer: *std.Io.Writer): ! {
        if (Output.enable_ansi_colors_stderr) {
            _ = writer.write(Output.prettyFmt("<r><d>[fetch]<r> ", true))?;
        }

        writer.print(
            "< {f} {s}\n",
            [
                StatusCodeFormatter{
                    code: this.status_code,
                },
                this.status,
            ],
        )?;
        for (header of this.headers.list) {
            if (Output.enable_ansi_colors_stderr) {
                _ = writer.write(Output.prettyFmt("<r><d>[fetch]<r> ", true))?;
            }

            _ = writer.write("< ")?;
            writer.print("{f}\n", [header])?;
        }
    }

    pub fn count(this, builder: *StringBuilder): void {
        builder.count(this.status);

        for (header of this.headers.list) {
            header.count(builder);
        }
    }

    pub fn clone(this, out_headers: []Header, builder: *StringBuilder): Response {
        let i: usize = 0;
        while (i < this.headers.list.len) : (i += 1) {
            out_headers[i] = this.headers.list[i].clone(builder);
        }

        return Response{
            minor_version: this.minor_version,
            status_code: this.status_code,
            status: builder.append(this.status),
            headers: HeaderList{ list: out_headers[0..this.headers.list.len] },
            bytes_read: this.bytes_read,
        };
    }

    pub static fn parseParts(buf: []const u8, src: []Header, offset: (*usize)?): Response! {
        let minor_version: c_int = 1;
        let status_code: c_int = 0;
        let status: string = "";
        let num_headers: usize = src.len;

        let rc: c_int = 0;
        unsafe {
            let off_ptr: *usize! = offset.? as *usize!;
            rc = c.phr_parse_response(
                buf.ptr,
                buf.len,
                *minor_version!,
                *status_code!,
                (&status.ptr) as [*c][*c]const u8,
                *status.len!,
                src.ptr as *c.phr_header!,
                *num_headers!,
                off_ptr.*,
            );
            if (rc == -2) {
                off_ptr.* += buf.len;
            }
        }

        if (rc == -1) {
            if (comptime Environment.allow_assert) {
                Output.debug("Malformed HTTP response:\n{s}", [buf]);
                return err.Malformed_HTTP_Response;
            }
            return err.Malformed_HTTP_Response;
        }
        if (rc == -2) {
            return err.ShortRead;
        }
        return Response{
            minor_version: usize(minor_version),
            status_code: u32(status_code),
            status: status,
            headers: HeaderList{ list: src[0..@min(num_headers, src.len)] },
            bytes_read: rc,
        };
    }

    pub static fn parse(buf: []const u8, src: []Header): Response! {
        let offset: usize = 0;
        const response = parseParts(buf, src, *offset)?;
        return response;
    }
}

export struct Headers {
    headers: []const Header,

    pub fn format(this, writer: *std.Io.Writer): ! {
        for (header of this.headers) {
            writer.print("{s}: {s}\r\n", [header.name, header.value])?;
        }
    }

    pub static fn parse(buf: []const u8, src: []Header): Headers! {
        let num_headers: usize = src.len;

        let rc: c_int = 0;
        unsafe {
            rc = c.phr_parse_headers(
                buf.ptr,
                buf.len,
                src.ptr as *c.phr_header!,
                (&num_headers) as [*c]usize,
                0,
            );
        }

        if (rc == -1) return err.BadHeaders;
        if (rc == -2) return err.ShortRead;
        return Headers{
            headers: src[0..num_headers],
        };
    }
}

pub const phr_header = c.phr_header;
pub const phr_chunked_decoder = c.phr_chunked_decoder;
pub const struct_phr_header = c.struct_phr_header;
pub const struct_phr_chunked_decoder = c.struct_phr_chunked_decoder;
pub const phr_parse_request = c.phr_parse_request;
pub const phr_parse_response = c.phr_parse_response;
pub const phr_parse_headers = c.phr_parse_headers;
pub const phr_decode_chunked = c.phr_decode_chunked;
pub const phr_decode_chunked_is_in_data = c.phr_decode_chunked_is_in_data;
