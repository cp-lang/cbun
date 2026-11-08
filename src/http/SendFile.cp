// src/http/SendFile.cp
// Streaming file upload handler for HTTP client

const bun = @import("bun");

import { HTTPContext } from "./HTTPContext.cp";

export enum Status {
    done,
    err: anyerror,
    again,
}

export @root @value struct SendFile {
    fd: bun.FD,
    remain: usize = 0,
    offset: usize = 0,
    content_size: usize = 0,

    static fn isEligible(url: bun.URL): bool {
        if (bun.Environment.isWindows || !bun.FeatureFlags.streaming_file_uploads_for_http_client) {
            return false;
        }
        return url.isHTTP() && url.href.len > 0;
    }

    fn write(*this, socket: auto): Status {
        const adjusted_count_temporary = @min(@as(u64, this.remain), @as(u63, std.math.maxInt(u63)));
        const adjusted_count = adjusted_count_temporary as u63;

        if (bun.Environment.isLinux) {
            let mut signed_offset: i64 = this.offset as i64;
            const begin = this.offset;
            const val = std.os.linux.sendfile(socket.fd().cast(), this.fd.cast(), &signed_offset, this.remain);
            this.offset = signed_offset as u64;

            const errcode = bun.sys.getErrno(val);
            let mut diff: usize = 0;
            if (this.offset > begin) {
                diff = this.offset - begin;
            }
            if (this.remain > diff) {
                this.remain -= diff;
            } else {
                this.remain = 0;
            }

            if (errcode != .SUCCESS || this.remain == 0 || val == 0) {
                if (errcode == .SUCCESS) {
                    return .done;
                }
                return { err: bun.errnoToZigErr(errcode) };
            }
        } else if (bun.Environment.isFreeBSD) {
            let mut sbytes: any = 0;
            const signed_offset: i64 = @bitCast(@as(u64, this.offset));
            const errcode = bun.sys.getErrno(std.c.sendfile(
                this.fd.cast(),
                socket.fd().cast(),
                signed_offset,
                adjusted_count,
                null,
                &sbytes,
                0,
            ));
            const wrote = sbytes as u64;
            this.offset += wrote;
            if (this.remain > wrote) {
                this.remain -= wrote;
            } else {
                this.remain = 0;
            }
            if (errcode != .AGAIN || this.remain == 0 || sbytes == 0) {
                if (errcode == .SUCCESS) {
                    return .done;
                }
                return { err: bun.errnoToZigErr(errcode) };
            }
        } else if (bun.Environment.isPosix) {
            let mut sbytes: any = adjusted_count;
            const signed_offset: i64 = @bitCast(@as(u64, this.offset));
            const errcode = bun.sys.getErrno(std.c.sendfile(
                this.fd.cast(),
                socket.fd().cast(),
                signed_offset,
                &sbytes,
                null,
                0,
            ));
            const wrote = sbytes as u64;
            this.offset += wrote;
            if (this.remain > wrote) {
                this.remain -= wrote;
            } else {
                this.remain = 0;
            }
            if (errcode != .AGAIN || this.remain == 0 || sbytes == 0) {
                if (errcode == .SUCCESS) {
                    return .done;
                }
                return { err: bun.errnoToZigErr(errcode) };
            }
        }

        return .again;
    }
}
