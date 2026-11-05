// src/http/Signals.cp
// HTTP atomic signaling and cancellation flags in CP language

export @value struct Store {
    header_progress: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    response_body_streaming: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    aborted: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    cert_errors: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),
    upgraded: std.atomic.Value(bool) = std.atomic.Value(bool).init(false),

    pub fn to(*this): Signals {
        return {
            header_progress: &this.header_progress,
            response_body_streaming: &this.response_body_streaming,
            aborted: &this.aborted,
            cert_errors: &this.cert_errors,
            upgraded: &this.upgraded,
        };
    }
}

export @root struct Signals {
    header_progress: *std.atomic.Value(bool)? = null,
    response_body_streaming: *std.atomic.Value(bool)? = null,
    aborted: *std.atomic.Value(bool)? = null,
    cert_errors: *std.atomic.Value(bool)? = null,
    upgraded: *std.atomic.Value(bool)? = null,

    pub fn isEmpty(this): bool {
        return this.aborted == null && this.response_body_streaming == null && this.header_progress == null && this.cert_errors == null && this.upgraded == null;
    }

    pub fn get(this, @comptime field: std.meta.FieldEnum(Signals)): bool {
        let ptr: *std.atomic.Value(bool) = @field(this, @tagName(field)) ? return false;
        return ptr.load(.monotonic);
    }
}
