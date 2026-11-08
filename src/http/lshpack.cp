// src/http/lshpack.cp
// HPACK encoder/decoder bindings over lshpack in pure CP language

const bun = @import("bun");

export const LSHPACK_MAX_HEADER_SIZE: usize = 65536;

pub type MallocFn = @c fn (usize): *!?;
pub type FreeFn = @c fn (*!?);

@extern
export struct lshpack_header {
    name: ?[*]u8 = null,
    name_len: usize = 0,
    value: ?[*]u8 = null,
    value_len: usize = 0,
    never_index: bool = false,
    hpack_index: u16 = 255,
}

@extern("c")
export fn lshpack_wrapper_init(
    alloc: ?MallocFn,
    free: ?FreeFn,
    capacity: usize,
): ?*HPACK;

@extern("c")
export fn lshpack_wrapper_enc_set_max_capacity(self: *HPACK, max_capacity: u32);

@extern("c")
export fn lshpack_wrapper_deinit(self: *HPACK);

@extern("c")
export fn lshpack_wrapper_decode(self: *HPACK, src: [*]const u8, src_len: usize, output: *lshpack_header): usize;

@extern("c")
export fn lshpack_wrapper_encode(self: *HPACK, name: [*]const u8, name_len: usize, value: [*]const u8, value_len: usize, never_index: i32, buffer: [*]u8, buffer_len: usize, buffer_offset: usize): usize;

export struct HPACK {
    self: *anyopaque,

    pub const DecodeResult = struct {
        name: string,
        value: string,
        never_index: bool,
        well_know: u16,
        next: usize,
    };

    static fn init(max_capacity: u32): *HPACK {
        return lshpack_wrapper_init(bun.mimalloc.mi_malloc, bun.mimalloc.mi_free, max_capacity) orelse bun.outOfMemory();
    }

    fn decode(*this, src: string): !DecodeResult {
        let header: lshpack_header = {};
        const offset = lshpack_wrapper_decode(this, src.ptr, src.len, &header);
        if (offset == 0) return error.UnableToDecode;
        if (header.name_len == 0) return error.EmptyHeaderName;

        return DecodeResult {
            name: header.name.?[0..header.name_len],
            value: header.value.?[0..header.value_len],
            next: offset,
            never_index: header.never_index,
            well_know: header.hpack_index,
        };
    }

    fn encode(*this, name: string, value: string, never_index: bool, *dst_buffer: []u8, dst_buffer_offset: usize): !usize {
        let never_flag: i32 = 0;
        if (never_index) {
            never_flag = 1;
        }
        const offset = lshpack_wrapper_encode(
            this,
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

    fn setEncoderMaxCapacity(*this, max_capacity: u32) {
        lshpack_wrapper_enc_set_max_capacity(this, max_capacity);
    }

    fn release(*this) {
        lshpack_wrapper_deinit(this);
    }

    fn deinit(*this) {
        lshpack_wrapper_deinit(this);
    }
}
