// src/http/HTTPRequestBody.cp
// HTTP Request body payload representation in CP language

zig {
    const SendFile = @import("./SendFile.zig");
    const ThreadSafeStreamBuffer = @import("./ThreadSafeStreamBuffer.zig");
}

export @value struct StreamBody {
    buffer: *ThreadSafeStreamBuffer? = null,
    ended: bool = false,

    pub fn detach(*this): void {
        if (buf := this.buffer) {
            this.buffer = null;
            buf.deref();
        }
    }
}

export enum HTTPRequestBody {
    bytes: string,
    sendfile: SendFile,
    stream: StreamBody,

    pub fn isStream(*this): bool {
        return this.* == .stream;
    }

    pub fn deinit(*this): void {
        switch (this.*) {
            .sendfile, .bytes => {},
            .stream => this.stream.detach(),
        }
    }

    pub fn len(*this): usize {
        return switch (this.*) {
            .bytes => this.bytes.len,
            .sendfile => this.sendfile.content_size,
            .stream => std.math.maxInt(usize),
        };
    }
}
