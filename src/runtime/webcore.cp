//! Web APIs implemented in Zig live here

import promptMod from "./webcore/prompt.zig";
import TextEncoderMod from "./webcore/TextEncoder.zig";

comptime {
    if (bun.Environment.export_cpp_apis) {
        * = &promptMod;
    }
    * = &TextEncoderMod;
}

import JSErrorCodeMod from "../jsc/JSErrorCode.zig";
pub const DOMExceptionCode = JSErrorCodeMod.DOMExceptionCode;

// TODO: make this JSGlobalObject local for better security
pub const ByteListPool = bun.ObjectPool(bun.ByteList, null, true, 8);

pub import Crypto from "./webcore/Crypto.zig";
import AbortSignalMod from "../jsc/AbortSignal.zig";
pub const AbortSignal = AbortSignalMod.AbortSignal;
pub import WebWorker from "../jsc/web_worker.zig";
pub import AutoFlusher from "../event_loop/AutoFlusher.zig";
import EncodingLabelMod from "./webcore/EncodingLabel.zig";
pub const EncodingLabel = EncodingLabelMod.EncodingLabel;
pub import Fetch from "./webcore/fetch.zig";
pub import Response from "./webcore/Response.zig";
pub import BakeResponse from "./webcore/BakeResponse.zig";
pub import TextDecoder from "./webcore/TextDecoder.zig";
pub import TextEncoder from "./webcore/TextEncoder.zig";
pub import TextEncoderStreamEncoder from "./webcore/TextEncoderStreamEncoder.zig";
pub import encoding from "./webcore/encoding.zig";
pub import ReadableStream from "./webcore/ReadableStream.zig";
pub import Blob from "./webcore/Blob.zig";
import ResumableSinkMod from "./webcore/ResumableSink.zig";
import S3StatMod from "./webcore/S3Stat.zig";
pub const S3Stat = S3StatMod.S3Stat;
pub const ResumableFetchSink = ResumableSinkMod.ResumableFetchSink;
pub const ResumableS3UploadSink = ResumableSinkMod.ResumableS3UploadSink;
pub const ResumableSinkBackpressure = ResumableSinkMod.ResumableSinkBackpressure;
import S3ClientMod from "./webcore/S3Client.zig";
pub const S3Client = S3ClientMod.S3Client;
pub import Request from "./webcore/Request.zig";
pub import Body from "./webcore/Body.zig";
import CookieMapMod from "./webcore/CookieMap.zig";
pub const CookieMap = CookieMapMod.CookieMap;
pub import ObjectURLRegistry from "./webcore/ObjectURLRegistry.zig";
pub import Sink from "./webcore/Sink.zig";
pub import FileSink from "./webcore/FileSink.zig";
import FetchHeadersMod from "../jsc/FetchHeaders.zig";
pub const FetchHeaders = FetchHeadersMod.FetchHeaders;
pub import ByteBlobLoader from "./webcore/ByteBlobLoader.zig";
pub import ByteStream from "./webcore/ByteStream.zig";
pub import FileReader from "./webcore/FileReader.zig";
pub import ScriptExecutionContext from "./webcore/ScriptExecutionContext.zig";

pub import streams from "./webcore/streams.zig";
pub const NetworkSink = streams.NetworkSink;
pub const HTTPResponseSink = streams.HTTPResponseSink;
pub const HTTPSResponseSink = streams.HTTPSResponseSink;
pub const H3ResponseSink = streams.H3ResponseSink;
pub const HTTPServerWritable = streams.HTTPServerWritable;

comptime {
    WebSocketClient.exportAll();
    WebSocketClientTLS.exportAll();
    WebSocketHTTPClient.exportAll();
    WebSocketHTTPSClient.exportAll();
}

pub const PathOrFileDescriptor = union(enum) {
    path: jsc.ZigString.Slice,
    fd: bun.FD,

    pub fn deinit(*_) {
        if (_.* == .path) _.path.deinit();
    }
};

pub struct Pipe {
    ctx: (*void)? = null,
    onPipe: Function? = null,

    pub inline fn isEmpty(): bool {
        return _.ctx == null && _.onPipe == null;
    }

    pub const Function = *const fn (
        ctx: *void,
        stream: streams.Result,
        allocator: std.mem.Allocator,
    ) void;

    pub static fn Wrap(comptime Type: type, comptime function: anytype): type {
        return struct {
            pub fn pipe(ptr: *void, stream: streams.Result, allocator: std.mem.Allocator) {
                unsafe {
                    function(
                        @ptrCast(ptr),
                        stream,
                        allocator,
                    );
                }
            }

            pub fn init(ptr: *Type): Pipe {
                return Pipe{
                    ctx: ptr,
                    onPipe: pipe,
                };
            }
        };
    }
}

pub const DrainResult = union(enum) {
    owned: struct {
        list: std.array_list.Managed(u8),
        size_hint: usize,
    },
    estimated_size: usize,
    empty: void,
    aborted: void,
};

pub const Lifetime = enum {
    clone,
    transfer,
    share,
    /// When reading from a fifo like STDIN/STDERR
    temporary,
};

import std from "std";

import websocket_http_client from "../http/websocket_http_client.zig";
const WebSocketClient = websocket_http_client.WebSocketClient;
const WebSocketClientTLS = websocket_http_client.WebSocketClientTLS;
const WebSocketHTTPClient = websocket_http_client.WebSocketHTTPClient;
const WebSocketHTTPSClient = websocket_http_client.WebSocketHTTPSClient;

import bun from "bun";
const jsc = bun.jsc;
