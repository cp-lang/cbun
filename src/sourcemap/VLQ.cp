// src/sourcemap/VLQ.cp
// Bun VLQ (Variable-Length Quantity) encoder & decoder in CP language

zig {
    const bun = @import("bun");
}

const vlq_max_in_bytes: usize = 7;
const base64: string = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

export @root struct VLQ {
    bytes: [u8; 7],
    len: u8 = 0,

    @inline
    pub fn slice(this): string {
        return this.bytes[0..this.len as usize];
    }

    pub fn writeTo(this, writer: anytype): void! {
        writer.writeAll(this.bytes[0..this.len as usize])?;
    }
}

export struct VLQResult {
    value: i32,
    start: usize,

    static fn init(value: i32, start: usize): VLQResult {
        return VLQResult {
            value: value,
            start: start,
        };
    }
}

fn make_base64_lut(): [u8; 128] {
    let mut bytes: [u8; 128] = [127; 128];
    for (let i of 0..64) {
        let c = base64[i as usize];
        bytes[c as usize] = i as u8;
    }
    return bytes;
}

fn make_vlq_lookup_table(): [VLQ; 256] {
    zig {
        @setEvalBranchQuota(9999);
    }
    let mut entries: [VLQ; 256] = [VLQ { bytes: [0; 7], len: 0 }; 256];
    for (let i of 0..256) {
        entries[i as usize] = encodeSlowPath(i);
    }
    return entries;
}

const base64_lut: [u8; 128] = make_base64_lut();
const vlq_lookup_table: [VLQ; 256] = make_vlq_lookup_table();
pub const zero: VLQ = vlq_lookup_table[0];

pub fn encode(value: i32): VLQ {
    if (value >= 0 && value <= 255) {
        return vlq_lookup_table[value as usize];
    } else {
        return encodeSlowPath(value);
    }
}

pub fn encodeSlowPath(value: i32): VLQ {
    let mut len: u8 = 0;
    let mut bytes: [u8; 7] = [0; 7];

    let mut vlq: u32 = 0;
    if (value >= 0) {
        vlq = (value as u32) << 1;
    } else {
        vlq = (((-value) as u32) << 1) | 1;
    }

    for (let step of 0..7) {
        let mut digit: u32 = vlq & 31;
        vlq = vlq >> 5;

        if (vlq != 0) {
            digit = digit | 32;
        }

        bytes[len as usize] = base64[digit as usize];
        len += 1;

        if (vlq == 0) {
            return VLQ { bytes: bytes, len: len };
        }
    }

    return VLQ { bytes: bytes, len: 0 };
}

pub fn decode(encoded: string, start: usize): VLQResult {
    let mut shift: u8 = 0;
    let mut vlq: u32 = 0;

    let rem = encoded[start..];
    let mut max_len: usize = 8;
    if (rem.len < 8) {
        max_len = rem.len;
    }
    let encoded_ = rem[0..max_len];

    for (let i of 0..8) {
        if (i >= encoded_.len) {
            break;
        }

        let char_code = (encoded_[i] as usize) & 127;
        let index = base64_lut[char_code] as u32;

        vlq = vlq | ((index & 31) << shift);
        shift += 5;

        if ((index & 32) == 0) {
            let mut res_val: i32 = 0;
            if ((vlq & 1) == 0) {
                res_val = (vlq >> 1) as i32;
            } else {
                res_val = -((vlq >> 1) as i32);
            }
            return VLQResult {
                value: res_val,
                start: start + (i as usize) + 1,
            };
        }
    }

    return VLQResult {
        value: 0,
        start: start + encoded_.len,
    };
}

pub fn decodeAssumeValid(encoded: string, start: usize): VLQResult {
    return decode(encoded, start);
}
