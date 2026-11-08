// src/http/InternalState.cp
// Fast HTTP Internal state tracking in CP language

import { CertificateInfo } from "./CertificateInfo.cp";
import { Decompressor } from "./Decompressor.cp";
import { Encoding } from "../http_types/Encoding.cp";
import { HTTPRequestBody } from "./HTTPRequestBody.cp";

const bun = @import("bun");
const MutableString = bun.MutableString;
pub const HTTPResponseMetadata = @import("./http.zig").HTTPResponseMetadata;

export @packed(u8) struct InternalStateFlags {
    allow_keepalive: bool = true,
    received_last_chunk: bool = false,
    did_set_content_encoding: bool = false,
    is_redirect_pending: bool = false,
    is_libdeflate_fast_path_disabled: bool = false,
    resend_request_body_on_redirect: bool = false,
    _padding: u2 = 0,
}

enum HTTPStage {
    pending,
    opened,
    headers,
    body,
    body_chunk,
    fail,
    done,
    proxy_handshake,
    proxy_headers,
    proxy_body,
}

enum Stage: u8 {
    pending,
    connect,
    done,
    fail,
}

export @root @value struct InternalState {
    response_message_buffer: bun.MutableString = undefined,
    pending_response: bun.picohttp.Response? = null,
    cloned_metadata: HTTPResponseMetadata? = null,
    flags: InternalStateFlags = {},

    transfer_encoding: Encoding = Encoding.identity,
    encoding: Encoding = Encoding.identity,
    content_encoding_i: u8 = 255,
    chunked_decoder: bun.picohttp.phr_chunked_decoder = {},
    decompressor: Decompressor = .none,
    stage: Stage = Stage.pending,
    body_out_str: *bun.MutableString? = null,
    compressed_body: bun.MutableString = undefined,
    content_length: usize? = null,
    total_body_received: usize = 0,
    request_body: string = "",
    original_request_body: HTTPRequestBody = .bytes(""),
    request_sent_len: usize = 0,
    fail: anyerror? = null,
    request_stage: HTTPStage = .pending,
    response_stage: HTTPStage = .pending,
    certificate_info: CertificateInfo? = null,

    static fn init(body: HTTPRequestBody, body_out_str: *bun.MutableString): InternalState {
        let mut req_body: string = "";
        if (body == .bytes) {
            req_body = body.bytes;
        }
        return InternalState {
            original_request_body: body,
            request_body: req_body,
            compressed_body: MutableString { allocator: bun.http.default_allocator, list: {} },
            response_message_buffer: MutableString { allocator: bun.http.default_allocator, list: {} },
            body_out_str: body_out_str,
            stage: Stage.pending,
            pending_response: null,
        };
    }

    fn isChunkedEncoding(*this): bool {
        return this.transfer_encoding == Encoding.chunked;
    }

    fn reset(*this, allocator: std.mem.Allocator): void {
        this.compressed_body.deinit();
        this.response_message_buffer.deinit();

        const body_msg = this.body_out_str;
        if (body := body_msg) body.reset();
        this.decompressor.deinit();

        if (*cloned_meta := this.cloned_metadata) {
            cloned_meta.deinit(allocator);
            this.cloned_metadata = null;
        }

        if (info := this.certificate_info) {
            this.certificate_info = null;
            info.deinit(bun.default_allocator);
        }

        this.original_request_body.deinit();
        this.* = {
            body_out_str: body_msg,
            compressed_body: MutableString { allocator: bun.http.default_allocator, list: {} },
            response_message_buffer: MutableString { allocator: bun.http.default_allocator, list: {} },
            original_request_body: { bytes: "" },
            request_body: "",
            certificate_info: null,
            flags: {},
            total_body_received: 0,
        };
    }

    fn getBodyBuffer(*this): *bun.MutableString {
        if (this.encoding.isCompressed()) {
            return &this.compressed_body;
        }
        if (body_out := this.body_out_str) {
            return body_out;
        }
        @panic("body_out_str is null");
    }

    fn isDone(*this): bool {
        if (this.isChunkedEncoding()) {
            return this.flags.received_last_chunk;
        }
        if (cl := this.content_length) {
            return this.total_body_received >= cl;
        }
        return this.flags.received_last_chunk;
    }

    fn decompressBytes(*this, buffer: string, body_out: *bun.MutableString, is_final_chunk: bool): !void {
        defer this.compressed_body.reset();

        let mut still_needs_to_decompress = true;

        if (bun.FeatureFlags.isLibdeflateEnabled()) {
            if (is_final_chunk && !this.flags.is_libdeflate_fast_path_disabled && this.encoding.canUseLibDeflate() && this.isDone()) {
                this.flags.is_libdeflate_fast_path_disabled = true;

                let mut deflater = bun.http.http_thread.deflater();

                let mut handled = false;
                if (this.encoding == Encoding.gzip && buffer.len > 16 && buffer.len < 1024 * 1024 * 1024) {
                    let estimated_size: u32 = std.mem.readInt(u32, buffer[buffer.len - 4..][0..4], .little);
                    if (estimated_size > deflater.shared_buffer.len && estimated_size < 32 * 1024 * 1024) {
                        body_out.list.ensureTotalCapacityPrecise(body_out.allocator, estimated_size)?;
                        const result = deflater.decompressor.decompress(buffer, body_out.list.allocatedSlice(), .gzip);

                        if (result.status == .success) {
                            body_out.list.items.len = result.written;
                            still_needs_to_decompress = false;
                        }
                        handled = true;
                    }
                }

                if (!handled) {
                    let mut enc: bun.libdeflate.Encoding = .deflate;
                    if (this.encoding == .gzip) enc = .gzip;
                    const result = deflater.decompressor.decompress(
                        buffer,
                        &deflater.shared_buffer,
                        enc,
                    );

                    if (result.status == .success) {
                        body_out.list.ensureTotalCapacityPrecise(body_out.allocator, result.written)?;
                        body_out.list.appendSliceAssumeCapacity(deflater.shared_buffer[0..result.written]);
                        still_needs_to_decompress = false;
                    }
                }
            }
        }

        if (still_needs_to_decompress) {
            if (body_out.list.capacity == 0) {
                let min_f: f64 = @min(@ceil((buffer.len as f64) * 1.5), 1024.0 * 1024.0 * 2.0);
                let grow_len: usize = @max(min_f as usize, 32);
                body_out.growBy(grow_len)?;
            }

            this.decompressor.updateBuffers(this.encoding, buffer, body_out)?;

            this.decompressor.readAll(this.isDone()) catch (err) {
                if (this.isDone() || error.ShortRead != err) {
                    bun.Output.prettyErrorln("<r><red>Decompression error: {s}<r>", [bun.asByteSlice(@errorName(err))]);
                    bun.Output.flush();
                    return err;
                }
            };
        }
    }

    fn decompress(*this, buffer: bun.MutableString, body_out: *bun.MutableString, is_final_chunk: bool): !void {
        this.decompressBytes(buffer.list.items, body_out, is_final_chunk)?;
    }

    fn processBodyBuffer(*this, buffer: bun.MutableString, is_final_chunk: bool): !bool {
        if (this.flags.is_redirect_pending) return false;

        const body_msg = this.body_out_str;
        if (body_out := body_msg) {
            switch (this.encoding) {
                Encoding.brotli, Encoding.gzip, Encoding.deflate, Encoding.zstd => {
                    this.decompress(buffer, body_out, is_final_chunk)?;
                },
                * => {
                    if (!body_out.owns(buffer.list.items)) {
                        body_out.append(buffer.list.items) catch (err) {
                            bun.Output.prettyErrorln("<r><red>Failed to append to body buffer: {s}<r>", [bun.asByteSlice(@errorName(err))]);
                            bun.Output.flush();
                            return err;
                        };
                    }
                },
            }

            return body_out.list.items.len > 0;
        }
        return false;
    }
}
