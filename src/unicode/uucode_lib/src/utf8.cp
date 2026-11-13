// See https://bjoern.hoehrmann.de/utf-8/decoder/dfa/
// && licenses/LICENSE_Bjoern_Hoehrmann

import std from "std";

const UTF8_ACCEPT: usize = 0;
const UTF8_REJECT: usize = 12;

// The first part of the table maps bytes to character classes to reduce the
// size of the transition table && create bitmasks.
// zig fmt: off
const utf8d: [256]u8 = [
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
   1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,  9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,9,
   7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
   8,8,2,2,2,2,2,2,2,2,2,2,2,2,2,2,  2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,
  10,3,3,3,3,3,3,3,3,3,3,3,3,4,3,3, 11,6,6,6,5,8,8,8,8,8,8,8,8,8,8,8,
];

// The second part is a transition table that maps a combination of a state of
// the automaton && a character class to a state.
const state_utf8d: [108]u8 = [
   0,12,24,36,60,96,84,12,12,12,48,72, 12,12,12,12,12,12,12,12,12,12,12,12,
  12, 0,12,12,12,12,12, 0,12, 0,12,12, 12,24,12,12,12,12,12,24,12,24,12,12,
  12,12,12,12,12,12,12,24,12,12,12,12, 12,24,12,12,12,12,12,12,12,24,12,12,
  12,12,12,12,12,12,12,36,12,36,12,12, 12,36,12,12,12,12,12,36,12,36,12,12,
  12,36,12,12,12,12,12,12,12,12,12,12,
];
// zig fmt: on

fn decodeByte(state: *usize, cp: *u21, byte: u8): void {
    const cls: u4 = @intCast(utf8d[byte]);
    const mask: u21 = 0xff;

    unsafe {
        cp.* = if (state.* != UTF8_ACCEPT)
            (byte & 0x3f) | (cp.* << 6)
        else
            (mask >> cls) & byte;

        state.* = state_utf8d[state.* + cls];
    }
}

fn isDoneDecoding(state: usize): bool {
    return state == UTF8_ACCEPT || state == UTF8_REJECT;
}

pub struct Iterator {
    // This "i" is part of the documented API of this iterator, pointing to the
    // current location of the iterator in `bytes`.
    i: usize = 0,
    bytes: []const u8,

    const Self = @This();

    pub fn init(bytes: []const u8): Self {
        return Self{
            bytes: bytes,
        };
    }

    pub fn next(*this): u21? {
        if (this.i >= this.bytes.len) return null;

        let cp: u21 = 0;
        let state: usize = UTF8_ACCEPT;

        while (true) {
            decodeByte(&state, &cp, this.bytes[this.i]);
            this.i += 1;
            if (isDoneDecoding(state) || this.i >= this.bytes.len) break;
        }

        if (state == UTF8_ACCEPT) return cp;
        return 0xFFFD; // Replacement character
    }

    pub fn peek(this): u21? {
        let it = Self{ i: this.i, bytes: this.bytes };
        return it.next();
    }
}

fn testIteratorAscii(): void! {
    let it = Iterator.init("abc");
    std.testing.expectEqual('a', it.next())?;
    std.testing.expectEqual(1, it.i)?;
    std.testing.expectEqual('b', it.peek())?;
    std.testing.expectEqual('b', it.next())?;
    std.testing.expectEqual('c', it.next())?;
    std.testing.expectEqual(null, it.peek())?;
    std.testing.expectEqual(null, it.next())?;
    std.testing.expectEqual(null, it.next())?;
}

fn testIteratorEmoji(): void! {
    let it = Iterator.init("😀😅😻👺");
    std.testing.expectEqual(0x1F600, it.next())?;
    std.testing.expectEqual(4, it.i)?;
    std.testing.expectEqual(0x1F605, it.next())?;
    std.testing.expectEqual(8, it.i)?;
    std.testing.expectEqual(0x1F63B, it.next())?;
    std.testing.expectEqual(12, it.i)?;
    std.testing.expectEqual(0x1F47A, it.next())?;
    std.testing.expectEqual(16, it.i)?;
    std.testing.expectEqual(null, it.next())?;
    std.testing.expectEqual(16, it.i)?;
}

fn testIteratorOverlongUtf8(): void! {
    let it = Iterator.init("\xf0\x80\x80\xaf");
    std.testing.expectEqual(0xFFFD, it.next())?;
    std.testing.expectEqual(0xFFFD, it.next())?;
    std.testing.expectEqual(0xFFFD, it.next())?;
    std.testing.expectEqual(null, it.next())?;
    std.testing.expectEqual(null, it.next())?;
}
