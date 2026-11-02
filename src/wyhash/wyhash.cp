//
// Wyhash port to CP language for Bun.js
// Based on Wyhash from the zig standard library, version v0.11.0-dev.2609+5e19250a1
//

const primes: [u64; 5] = [
    0xa0761d6478bd642f,
    0xe7037ed1a0b428db,
    0x8ebc6af09c88c6e3,
    0x589965cc75374cc3,
    0x1d8e4e27c47d124f,
];

fn read_bytes1(data: string): u64 {
    return data[0] as u64;
}

fn read_bytes2(data: string): u64 {
    return (data[0] as u64) | ((data[1] as u64) << 8);
}

fn read_bytes4(data: string): u64 {
    return (data[0] as u64)
        | ((data[1] as u64) << 8)
        | ((data[2] as u64) << 16)
        | ((data[3] as u64) << 24);
}

fn read_bytes8(data: string): u64 {
    return (data[0] as u64)
        | ((data[1] as u64) << 8)
        | ((data[2] as u64) << 16)
        | ((data[3] as u64) << 24)
        | ((data[4] as u64) << 32)
        | ((data[5] as u64) << 40)
        | ((data[6] as u64) << 48)
        | ((data[7] as u64) << 56);
}

fn read_8bytes_swapped(data: string): u64 {
    return (read_bytes4(data) << 32) | read_bytes4(data[4..]);
}

fn mum(a: u64, b: u64): u64 {
    let r: u128 = (a as u128) * (b as u128);
    r = (r >> 64) ^ r;
    return r as u64;
}

fn mix0(a: u64, b: u64, seed: u64): u64 {
    return mum(a ^ seed ^ primes[0], b ^ seed ^ primes[1]);
}

fn mix1(a: u64, b: u64, seed: u64): u64 {
    return mum(a ^ seed ^ primes[2], b ^ seed ^ primes[3]);
}

// Wyhash version which does not store internal state for handling partial buffers.
// This is needed so that we can maximize the speed for the short key case, which will
// use the non-iterative api which the public Wyhash exposes.
export struct WyhashStateless {
    seed: u64,
    msg_len: usize,

    static fn init(seed: u64): WyhashStateless {
        return WyhashStateless {
            seed: seed,
            msg_len: 0,
        };
    }

    fn round(*this, b: string) {
        seed = mix0(
            read_bytes8(b[0..]),
            read_bytes8(b[8..]),
            seed,
        ) ^ mix1(
            read_bytes8(b[16..]),
            read_bytes8(b[24..]),
            seed,
        );
    }

    fn update(*this, b: string) {
        let off: usize = 0;
        while (off < b.len) {
            round(b[off..off + 32]);
            off += 32;
        }
        msg_len += b.len;
    }

    fn final(*this, b: string): u64 {
        let s = seed;
        let rem_len = b.len;
        let rem_key = b[0..rem_len];

        seed = switch (rem_len) {
            0 => s,
            1 => mix0(read_bytes1(rem_key), primes[4], s),
            2 => mix0(read_bytes2(rem_key), primes[4], s),
            3 => mix0((read_bytes2(rem_key) << 8) | read_bytes1(rem_key[2..]), primes[4], s),
            4 => mix0(read_bytes4(rem_key), primes[4], s),
            5 => mix0((read_bytes4(rem_key) << 8) | read_bytes1(rem_key[4..]), primes[4], s),
            6 => mix0((read_bytes4(rem_key) << 16) | read_bytes2(rem_key[4..]), primes[4], s),
            7 => mix0((read_bytes4(rem_key) << 24) | (read_bytes2(rem_key[4..]) << 8) | read_bytes1(rem_key[6..]), primes[4], s),
            8 => mix0(read_8bytes_swapped(rem_key), primes[4], s),
            9 => mix0(read_8bytes_swapped(rem_key), read_bytes1(rem_key[8..]), s),
            10 => mix0(read_8bytes_swapped(rem_key), read_bytes2(rem_key[8..]), s),
            11 => mix0(read_8bytes_swapped(rem_key), (read_bytes2(rem_key[8..]) << 8) | read_bytes1(rem_key[10..]), s),
            12 => mix0(read_8bytes_swapped(rem_key), read_bytes4(rem_key[8..]), s),
            13 => mix0(read_8bytes_swapped(rem_key), (read_bytes4(rem_key[8..]) << 8) | read_bytes1(rem_key[12..]), s),
            14 => mix0(read_8bytes_swapped(rem_key), (read_bytes4(rem_key[8..]) << 16) | read_bytes2(rem_key[12..]), s),
            15 => mix0(read_8bytes_swapped(rem_key), (read_bytes4(rem_key[8..]) << 24) | (read_bytes2(rem_key[12..]) << 8) | read_bytes1(rem_key[14..]), s),
            16 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s),
            17 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_bytes1(rem_key[16..]), primes[4], s),
            18 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_bytes2(rem_key[16..]), primes[4], s),
            19 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1((read_bytes2(rem_key[16..]) << 8) | read_bytes1(rem_key[18..]), primes[4], s),
            20 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_bytes4(rem_key[16..]), primes[4], s),
            21 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1((read_bytes4(rem_key[16..]) << 8) | read_bytes1(rem_key[20..]), primes[4], s),
            22 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1((read_bytes4(rem_key[16..]) << 16) | read_bytes2(rem_key[20..]), primes[4], s),
            23 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1((read_bytes4(rem_key[16..]) << 24) | (read_bytes2(rem_key[20..]) << 8) | read_bytes1(rem_key[22..]), primes[4], s),
            24 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), primes[4], s),
            25 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), read_bytes1(rem_key[24..]), s),
            26 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), read_bytes2(rem_key[24..]), s),
            27 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), (read_bytes2(rem_key[24..]) << 8) | read_bytes1(rem_key[26..]), s),
            28 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), read_bytes4(rem_key[24..]), s),
            29 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), (read_bytes4(rem_key[24..]) << 8) | read_bytes1(rem_key[28..]), s),
            30 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), (read_bytes4(rem_key[24..]) << 16) | read_bytes2(rem_key[28..]), s),
            31 => mix0(read_8bytes_swapped(rem_key), read_8bytes_swapped(rem_key[8..]), s) ^ mix1(read_8bytes_swapped(rem_key[16..]), (read_bytes4(rem_key[24..]) << 24) | (read_bytes2(rem_key[28..]) << 8) | read_bytes1(rem_key[30..]), s),
            else => s,
        };

        msg_len += b.len;
        return mum(seed ^ (msg_len as u64), primes[4]);
    }

    static fn hash(seed: u64, input: string): u64 {
        let aligned_len = input.len - (input.len % 32);
        let c = WyhashStateless.init(seed);
        c.update(input[0..aligned_len]);
        return c.final(input[aligned_len..]);
    }
}

/// Fast non-cryptographic 64bit hash function.
/// See https://github.com/wangyi-fudan/wyhash
export struct Wyhash11 {
    state: WyhashStateless,
    buf: [u8; 32],
    buf_len: usize,

    static fn init(seed: u64): Wyhash11 {
        return Wyhash11 {
            state: WyhashStateless.init(seed),
            buf: [0; 32],
            buf_len: 0,
        };
    }

    fn update(*this, b: string) {
        let off: usize = 0;

        if (buf_len != 0 && buf_len + b.len >= 32) {
            off += 32 - buf_len;
            let i: usize = 0;
            while (i < off) {
                buf[buf_len + i] = b[i];
                i += 1;
            }
            state.update(buf[0..32]);
            buf_len = 0;
        }

        let remain_len = b.len - off;
        let aligned_len = remain_len - (remain_len % 32);
        state.update(b[off..off + aligned_len]);

        let rem_slice = b[off + aligned_len..b.len];
        let j: usize = 0;
        while (j < rem_slice.len) {
            buf[buf_len + j] = rem_slice[j];
            j += 1;
        }
        buf_len += rem_slice.len;
    }

    fn final(*this): u64 {
        let rem_key = buf[0..buf_len];
        return state.final(rem_key);
    }

    static fn hash(seed: u64, input: string): u64 {
        return WyhashStateless.hash(seed, input);
    }
}
