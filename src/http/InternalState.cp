// src/http/InternalState.cp
// Fast HTTP Internal state tracking in CP language

zig {
    const std = @import("std");

    const bun = @import("bun");
    const FeatureFlags = bun.FeatureFlags;
    const MutableString = bun.MutableString;
    const Output = bun.Output;
    const picohttp = bun.picohttp;

    const HTTPClient = bun.http;
    const CertificateInfo = HTTPClient.CertificateInfo;
    const Decompressor = HTTPClient.Decompressor;
    const Encoding = HTTPClient.Encoding;
    const HTTPRequestBody = HTTPClient.HTTPRequestBody;
    const HTTPResponseMetadata = HTTPClient.HTTPResponseMetadata;

    const log = Output.scoped(.HTTPInternalState, .hidden);
}

export @packed(u8) struct InternalStateFlags {
    pub allow_keepalive: bool = true,
    pub received_last_chunk: bool = false,
    pub did_set_content_encoding: bool = false,
    pub is_redirect_pending: bool = false,
    pub is_libdeflate_fast_path_disabled: bool = false,
    pub resend_request_body_on_redirect: bool = false,
    pub _padding: u2 = 0,
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

enum Stage(u8) {
    pending,
    connect,
    done,
    fail,
}

export @root @value struct InternalState {
    response_message_buffer: MutableString = undefined,
    pending_response: picohttp.Response? = null,
    cloned_metadata: HTTPResponseMetadata? = null,
    flags: InternalStateFlags = {},

    transfer_encoding: Encoding = Encoding.identity,
    encoding: Encoding = Encoding.identity,
    content_encoding_i: u8 = std.math.maxInt(u8),
    chunked_decoder: picohttp.phr_chunked_decoder = {},
    decompressor: Decompressor = { none: {} },
    stage: Stage = Stage.pending,
    body_out_str: *MutableString? = null,
    compressed_body: MutableString = undefined,
    content_length: usize? = null,
    total_body_received: usize = 0,
    request_body: string = "",
    original_request_body: HTTPRequestBody = { bytes: "" },
    request_sent_len: usize = 0,
    fail: anyerror? = null,
    request_stage: HTTPStage = .pending,
    response_stage: HTTPStage = .pending,
    certificate_info: CertificateInfo? = null,

    pub static fn init(body: HTTPRequestBody, body_out_str: *MutableString): InternalState {
        let mut req_body: string = "";
        if (body == .bytes) {
            req_body = body.bytes;
        }
        return {
            original_request_body: body,
            request_body: req_body,
            compressed_body: MutableString { allocator: bun.http.default_allocator, list: {} },
            response_message_buffer: MutableString { allocator: bun.http.default_allocator, list: {} },
            body_out_str: body_out_str,
            stage: Stage.pending,
            pending_response: null,
        };
    }

    pub fn isChunkedEncoding(*this): bool {
        return this.transfer_encoding == Encoding.chunked;
    }

    pub fn reset(*this, allocator: std.mem.Allocator): void {
        this.compressed_body.deinit();
        this.response_message_buffer.deinit();

        const body_msg = this.body_out_str;
        if (body := body_msg) body.reset();
        this.decompressor.deinit();

        zig {
            if (self.cloned_metadata != null) {
                self.cloned_metadata.?.deinit(allocator);
                self.cloned_metadata = null;
            }
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

    pub fn getBodyBuffer(*this): *MutableString {
        if (this.encoding.isCompressed()) {
            return &this.compressed_body;
        }
        if (body_out := this.body_out_str) {
            return body_out;
        }
        @panic("body_out_str is null");
    }

    pub fn isDone(*this): bool {
        if (this.isChunkedEncoding()) {
            return this.flags.received_last_chunk;
        }
        if (cl := this.content_length) {
            return this.total_body_received >= cl;
        }
        return this.flags.received_last_chunk;
    }

    pub fn decompressBytes(*this, buffer: string, body_out: *MutableString, is_final_chunk: bool): void! {
        defer this.compressed_body.reset();
        let mut gzip_timer: std.time.Timer = undefined;

        zig {
            if (bun.http.extremely_verbose) {
                gzip_timer = std.time.Timer.start() catch @panic("Timer failure");
            }
        }

        let mut still_needs_to_decompress = true;

        if (FeatureFlags.isLibdeflateEnabled()) {
            if (is_final_chunk && !this.flags.is_libdeflate_fast_path_disabled && this.encoding.canUseLibDeflate() && this.isDone()) {
                this.flags.is_libdeflate_fast_path_disabled = true;

                log("Decompressing {d} bytes with libdeflate\n", [buffer.len]);
                let mut deflater = bun.http.http_thread.deflater();

                let mut handled = false;
                if (this.encoding == Encoding.gzip && buffer.len > 16 && buffer.len < 1024 * 1024 * 1024) {
                    let mut estimated_size: u32 = 0;
                    zig {
                        estimated_size = @bitCast(buffer[buffer.len - 4..][0..4].*);
                    }
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
                    zig {
                        const result = deflater.decompressor.decompress(buffer, &deflater.shared_buffer, switch (self.encoding) {
                            .gzip => .gzip,
                            .deflate => .deflate,
                            else => unreachable,
                        });

                        if (result.status == .success) {
                            try body_out.list.ensureTotalCapacityPrecise(body_out.allocator, result.written);
                            body_out.list.appendSliceAssumeCapacity(deflater.shared_buffer[0..result.written]);
                            still_needs_to_decompress = false;
                        }
                    }
                }
            }
        }

        if (still_needs_to_decompress) {
            log("Decompressing {d} bytes\n", [buffer.len]);
            if (body_out.list.capacity == 0) {
                const min = @min(@ceil(@as(f64, @floatFromInt(buffer.len)) * 1.5), @as(f64, 1024 * 1024 * 2));
                body_out.growBy(@max(@as(usize, @intFromFloat(min)), 32))?;
            }

            this.decompressor.updateBuffers(this.encoding, buffer, body_out)?;

            this.decompressor.readAll(this.isDone()) catch (err) {
                if (this.isDone() || error.ShortRead != err) {
                    Output.prettyErrorln("<r><red>Decompression error: {s}<r>", [bun.asByteSlice(@errorName(err))]);
                    Output.flush();
                    return err;
                }
            };
        }

        zig {
            if (bun.http.extremely_verbose) {
                self.gzip_elapsed = gzip_timer.read();
            }
        }
    }

    pub fn decompress(*this, buffer: MutableString, body_out: *MutableString, is_final_chunk: bool): void! {
        this.decompressBytes(buffer.list.items, body_out, is_final_chunk)?;
    }

    pub fn processBodyBuffer(*this, buffer: MutableString, is_final_chunk: bool): bool! {
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
                            Output.prettyErrorln("<r><red>Failed to append to body buffer: {s}<r>", [bun.asByteSlice(@errorName(err))]);
                            Output.flush();
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
