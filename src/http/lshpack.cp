// src/http/lshpack.cp
// HPACK encoder/decoder bindings over lshpack in CP language

zig {
    const bun = @import("bun");

    pub const LSHPACK_MAX_HEADER_SIZE: usize = 65536;

    pub const lshpack_header = extern struct {
        name: ?[*]u8 = null,
        name_len: usize = 0,
        value: ?[*]u8 = null,
        value_len: usize = 0,
        never_index: bool = false,
        hpack_index: u16 = 255,
    };

    pub extern "c" fn lshpack_wrapper_init(
        alloc: ?*const fn (usize) callconv(.c) ?*anyopaque,
        free: ?*const fn (?*anyopaque) callconv(.c) void,
        capacity: usize,
    ) ?*HPACK;
    pub extern "c" fn lshpack_wrapper_enc_set_max_capacity(self: *HPACK, max_capacity: u32) void;
    pub extern "c" fn lshpack_wrapper_deinit(self: *HPACK) void;
    pub extern "c" fn lshpack_wrapper_decode(self: *HPACK, src: [*]const u8, src_len: usize, output: *lshpack_header) usize;
    pub extern "c" fn lshpack_wrapper_encode(self: *HPACK, name: [*]const u8, name_len: usize, value: [*]const u8, value_len: usize, never_index: i32, buffer: [*]u8, buffer_len: usize, buffer_offset: usize) usize;

    pub const HPACK = struct {
        self: *anyopaque,

        pub const DecodeResult = struct {
            name: []const u8,
            value: []const u8,
            never_index: bool,
            well_know: u16,
            next: usize,
        };

        pub fn init(max_capacity: u32) *HPACK {
            return lshpack_wrapper_init(bun.mimalloc.mi_malloc, bun.mimalloc.mi_free, max_capacity) orelse bun.outOfMemory();
        }

        pub fn decode(self: *HPACK, src: []const u8) !DecodeResult {
            var header: lshpack_header = .{};
            const offset = lshpack_wrapper_decode(self, src.ptr, src.len, &header);
            if (offset == 0) return error.UnableToDecode;
            if (header.name_len == 0) return error.EmptyHeaderName;

            return DecodeResult{
                .name = header.name.?[0..header.name_len],
                .value = header.value.?[0..header.value_len],
                .next = offset,
                .never_index = header.never_index,
                .well_know = header.hpack_index,
            };
        }

        pub fn encode(self: *HPACK, name: []const u8, value: []const u8, never_index: bool, dst_buffer: []u8, dst_buffer_offset: usize) !usize {
            const never_flag: i32 = if (never_index) 1 else 0;
            const offset = lshpack_wrapper_encode(
                self,
                name.ptr,
                name.len,
                value.ptr,
                value.len,
                never_flag,
                dst_buffer.ptr,
                dst_buffer.len,
                dst_buffer_offset,
            );
            if (offset <= 0) return error.UnableToEncode;
            return offset;
        }

        pub fn setEncoderMaxCapacity(self: *HPACK, max_capacity: u32) void {
            lshpack_wrapper_enc_set_max_capacity(self, max_capacity);
        }

        pub fn release(self: *HPACK) void {
            lshpack_wrapper_deinit(self);
        }

        pub fn deinit(self: *HPACK) void {
            lshpack_wrapper_deinit(self);
        }
    };
}
