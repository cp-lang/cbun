// src/http/H2Client.cp
// HTTP/2 path constants, atomic counters, and coordinator exports in CP

export const local_initial_window_size: u31 = 1 << 24;
export const local_max_header_list_size: u32 = 256 * 1024;
export const write_buffer_high_water: usize = 256 * 1024;
export const write_buffer_control_limit: usize = 1024 * 1024;

zig {
    pub var live_sessions = std.atomic.Value(i32).init(0);
    pub var live_streams = std.atomic.Value(i32).init(0);

    pub const Stream = @import("./h2_client/Stream.zig");
    pub const ClientSession = @import("./h2_client/ClientSession.zig");
    pub const PendingConnect = @import("./h2_client/PendingConnect.zig");

    pub const TestingAPIs = @import("../http_jsc/headers_jsc.zig").H2TestingAPIs;
}
