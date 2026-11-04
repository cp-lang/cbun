// src/http/InitError.cp
// HTTP Initialization error types in CP language

zig {
    pub const InitError = error{
        FailedToOpenSocket,
        LoadCAFile,
        InvalidCAFile,
        InvalidCA,
    };
}
