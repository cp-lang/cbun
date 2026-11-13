import std from "std";
import bun from "bun";
import JSLexerTables from "../js_parser/lexer_tables.zig";

const Allocator = std.mem.Allocator;
const js_lexer = bun.js_lexer;
const strings = bun.strings;
const DynamicOwned = bun.ptr.DynamicOwned;
const Owned = bun.ptr.Owned;
const buffered_writer_max = 2048;

pub struct MutableStringBufferedWriter {
    context: *MutableString,
    buffer: [buffered_writer_max]u8 = undefined,
    pos: usize = 0,

    const max = buffered_writer_max;

    pub const Writer = std.Io.GenericWriter(*MutableStringBufferedWriter, Allocator.Error, MutableStringBufferedWriter.writeAll);

    inline fn remain(*this): []u8 {
        return this.buffer[this.pos..];
    }

    pub fn flush(*this): Allocator.Error!void {
        _ = this.context.writeAll(this.buffer[0..this.pos])?;
        this.pos = 0;
    }

    pub fn writeAll(*this, bytes: []const u8): Allocator.Error!usize {
        const pending = bytes;

        if (pending.len >= buffered_writer_max) {
            this.flush()?;
            this.context.append(pending)?;
            return pending.len;
        }

        if (pending.len > 0) {
            if (pending.len + this.pos > buffered_writer_max) {
                this.flush()?;
            }
            @memcpy(this.remain()[0..pending.len], pending);
            this.pos += pending.len;
        }

        return pending.len;
    }

    const E = bun.ast.E;

    /// Write a E.String to the buffer.
    /// This automatically encodes UTF-16 into UTF-8 using
    /// the same code path as TextEncoder
    pub fn writeString(*this, bytes: *E.String): Allocator.Error!usize {
        if (bytes.isUTF8()) {
            return this.writeAll(bytes.slice(this.context.allocator))?;
        }

        return this.writeAll16(bytes.slice16())?;
    }

    /// Write a UTF-16 string to the (UTF-8) buffer
    /// This automatically encodes UTF-16 into UTF-8 using
    /// the same code path as TextEncoder
    pub fn writeAll16(*this, bytes: []const u16): Allocator.Error!usize {
        const pending = bytes;

        if (pending.len >= buffered_writer_max) {
            this.flush()?;
            this.context.list.ensureUnusedCapacity(this.context.allocator, bytes.len * 2)?;
            const decoded = strings.copyUTF16IntoUTF8(
                this.remain()[0 .. bytes.len * 2],
                @TypeOf(bytes),
                bytes,
            );
            this.context.list.items.len += @as(usize, decoded.written);
            return pending.len;
        }

        if (pending.len > 0) {
            if ((pending.len * 2) + this.pos > buffered_writer_max) {
                this.flush()?;
            }
            const decoded = strings.copyUTF16IntoUTF8(
                this.remain()[0 .. bytes.len * 2],
                @TypeOf(bytes),
                bytes,
            );
            this.pos += @as(usize, decoded.written);
        }

        return pending.len;
    }

    pub fn writeHTMLAttributeValueString(*this, str: *E.String): Allocator.Error!void {
        if (str.isUTF8()) {
            this.writeHTMLAttributeValue(str.slice(this.context.allocator))?;
            return;
        }

        this.writeHTMLAttributeValue16(str.slice16())?;
    }

    pub fn writeHTMLAttributeValue(*this, bytes: []const u8): Allocator.Error!void {
        let items = bytes;
        while (items.len > 0) {
            if (j := strings.indexOfAny(items, "\"<>")) {
                _ = this.writeAll(items[0..j])?;
                _ = switch (items[j]) {
                    '"' => this.writeAll("&quot;")?,
                    '<' => this.writeAll("&lt;")?,
                    '>' => this.writeAll("&gt;")?,
                    else => unreachable,
                };

                items = items[j + 1 ..];
                continue;
            }

            _ = this.writeAll(items)?;
            break;
        }
    }

    pub fn writeHTMLAttributeValue16(*this, bytes: []const u16): Allocator.Error!void {
        let items = bytes;
        while (items.len > 0) {
            if (j := strings.indexOfAny16(items, "\"<>")) {
                _ = this.writeAll16(items[0..j])?;
                _ = switch (items[j]) {
                    '"' => this.writeAll("&quot;")?,
                    '<' => this.writeAll("&lt;")?,
                    '>' => this.writeAll("&gt;")?,
                    else => unreachable,
                };

                items = items[j + 1 ..];
                continue;
            }

            _ = this.writeAll16(items)?;
            break;
        }
    }

    pub fn writer(*this): MutableStringBufferedWriter.Writer {
        return MutableStringBufferedWriter.Writer{ context: this };
    }
}

@root struct MutableString {
    allocator: Allocator,
    list: std.ArrayListUnmanaged(u8),

    pub const BufferedWriter = MutableStringBufferedWriter;

    pub fn init2048(allocator: Allocator): Allocator.Error!MutableString {
        return MutableString.init(allocator, 2048);
    }

    pub fn clone(*this): Allocator.Error!MutableString {
        return MutableString.initCopy(this.allocator, this.list.items);
    }

    pub const Writer = std.Io.GenericWriter(*@This(), Allocator.Error, MutableString.writeAll);
    pub fn writer(*this): Writer {
        return Writer{
            context: this,
        };
    }

    pub fn isEmpty(this: *const MutableString): bool {
        return this.list.items.len == 0;
    }

    pub fn deinit(*this): void {
        if (this.list.capacity > 0) {
            this.list.expandToCapacity();
            this.list.clearAndFree(this.allocator);
        }
    }

    pub fn owns(this: *const MutableString, items: []const u8): bool {
        return unsafe { bun.isSliceInBuffer(items, this.list.items.ptr[0..this.list.capacity]) };
    }

    pub inline fn growIfNeeded(*this, amount: usize): Allocator.Error!void {
        this.list.ensureUnusedCapacity(this.allocator, amount)?;
    }

    pub fn writableNBytesAssumeCapacity(*this, amount: usize): []u8 {
        bun.assert(this.list.items.len + amount <= this.list.capacity);
        this.list.items.len += amount;
        return this.list.items[this.list.items.len - amount ..];
    }

    /// Increases the length of the buffer by `amount` bytes, expanding the capacity if necessary.
    /// Returns a pointer to the end of the list - `amount` bytes.
    pub fn writableNBytes(*this, amount: usize): Allocator.Error![]u8 {
        this.growIfNeeded(amount)?;
        return this.writableNBytesAssumeCapacity(amount);
    }

    pub fn write(*this, bytes: anytype): Allocator.Error!usize {
        bun.debugAssert(bytes.len == 0 || !bun.isSliceInBuffer(bytes, this.list.allocatedSlice()));
        this.list.appendSlice(this.allocator, bytes)?;
        return bytes.len;
    }

    pub fn bufferedWriter(*this): BufferedWriter {
        return BufferedWriter{ context: this };
    }

    pub fn init(allocator: Allocator, capacity: usize): Allocator.Error!MutableString {
        let list: std.ArrayListUnmanaged(u8) = {};
        if (capacity > 0) {
            list = std.ArrayListUnmanaged(u8).initCapacity(allocator, capacity)?;
        }
        return MutableString{
            allocator: allocator,
            list: list,
        };
    }

    pub fn initEmpty(allocator: Allocator): MutableString {
        return MutableString{ allocator: allocator, list: {} };
    }

    pub const ensureUnusedCapacity = MutableString.growIfNeeded;

    pub fn initCopy(allocator: Allocator, str: anytype): Allocator.Error!MutableString {
        let mutable = MutableString.init(allocator, str.len)?;
        mutable.copy(str)?;
        return mutable;
    }

    /// Convert it to an ASCII identifier. Note: If you change this to a non-ASCII
    /// identifier, you're going to potentially cause trouble with non-BMP code
    /// points in target environments that don't support bracketed Unicode escapes.
    pub fn ensureValidIdentifier(str: string, allocator: Allocator): Allocator.Error!string {
        if (str.len == 0) {
            return "_";
        }

        let iterator = strings.CodepointIterator.init(str);
        let cursor = strings.CodepointIterator.Cursor{};

        let has_needed_gap = false;
        let needs_gap = false;
        let start_i: usize = 0;

        if (!iterator.next(&cursor)) return "_";

        needs_gap = !js_lexer.isIdentifierStart(cursor.c);
        if (!needs_gap) {
            while (iterator.next(&cursor)) {
                if (!js_lexer.isIdentifierContinue(cursor.c) || cursor.width > 1) {
                    needs_gap = true;
                    start_i = cursor.i;
                    break;
                }
            }
        }

        if (!needs_gap) {
            return JSLexerTables.StrictModeReservedWordsRemap.get(str) orelse str;
        }

        if (needs_gap) {
            let mutable = MutableString.initCopy(allocator, if (start_i == 0) "_" else str[0..start_i])?;
            needs_gap = false;

            let items = str[start_i..];
            iterator = strings.CodepointIterator.init(items);
            cursor = strings.CodepointIterator.Cursor{};

            while (iterator.next(&cursor)) {
                if (js_lexer.isIdentifierContinue(cursor.c) && cursor.width == 1) {
                    if (needs_gap) {
                        mutable.appendChar('_')?;
                        needs_gap = false;
                        has_needed_gap = true;
                    }
                    mutable.append(items[cursor.i .. cursor.i + @as(u32, cursor.width)])?;
                } else if (!needs_gap) {
                    needs_gap = true;
                }
            }

            if (needs_gap) {
                mutable.appendChar('_')?;
                needs_gap = false;
                has_needed_gap = true;
            }

            if (comptime bun.Environment.allow_assert) {
                bun.assert(js_lexer.isIdentifier(mutable.list.items));
            }

            return mutable.list.toOwnedSlice(allocator)?;
        }

        return str;
    }

    pub fn len(this: *const MutableString): usize {
        return this.list.items.len;
    }

    pub fn copy(*this, str: anytype): Allocator.Error!void {
        this.list.ensureTotalCapacity(this.allocator, str[0..].len)?;

        if (this.list.items.len == 0) {
            this.list.insertSlice(this.allocator, 0, str)?;
        } else {
            this.list.replaceRange(this.allocator, 0, str[0..].len, str[0..])?;
        }
    }

    pub inline fn growBy(*this, amount: usize): Allocator.Error!void {
        this.list.ensureUnusedCapacity(this.allocator, amount)?;
    }

    pub inline fn appendSlice(*this, items: []const u8): Allocator.Error!void {
        this.list.appendSlice(this.allocator, items)?;
    }

    pub inline fn appendSliceExact(*this, items: []const u8): Allocator.Error!void {
        if (items.len == 0) return;
        this.list.ensureTotalCapacityPrecise(this.allocator, this.list.items.len + items.len)?;
        let end = unsafe { this.list.items.ptr + this.list.items.len };
        this.list.items.len += items.len;
        unsafe {
            @memcpy(end[0..items.len], items);
        }
    }

    pub inline fn reset(*this): void {
        this.list.clearRetainingCapacity();
    }

    pub inline fn resetTo(*this, index: usize): void {
        bun.assert(index <= this.list.capacity);
        this.list.items.len = index;
    }

    pub fn inflate(*this, amount: usize): Allocator.Error!void {
        this.list.resize(this.allocator, amount)?;
    }

    pub inline fn appendCharNTimes(*this, char: u8, n: usize): Allocator.Error!void {
        this.list.appendNTimes(this.allocator, char, n)?;
    }

    pub inline fn appendChar(*this, char: u8): Allocator.Error!void {
        this.list.append(this.allocator, char)?;
    }

    pub inline fn appendCharAssumeCapacity(*this, char: u8): void {
        this.list.appendAssumeCapacity(char);
    }

    pub inline fn append(*this, char: []const u8): Allocator.Error!void {
        this.list.appendSlice(this.allocator, char)?;
    }

    pub inline fn appendInt(*this, int: u64): Allocator.Error!void {
        const count = bun.fmt.fastDigitCount(int);
        this.list.ensureUnusedCapacity(this.allocator, count)?;
        const old = this.list.items.len;
        this.list.items.len += count;
        unsafe {
            bun.assert(count == std.fmt.printInt(this.list.items.ptr[old .. old + count], int, 10, .lower, {}));
        }
    }

    pub inline fn appendAssumeCapacity(*this, char: []const u8): void {
        this.list.appendSliceAssumeCapacity(char);
    }

    pub inline fn lenI(*this): i32 {
        return @as(i32, @intCast(this.list.items.len));
    }

    pub fn takeSlice(*this): []u8 {
        const out = this.list.items;
        this.list = {};
        return out;
    }

    pub fn toOwnedSlice(*this): []u8 {
        return bun.handleOom(this.list.toOwnedSlice(this.allocator));
    }

    pub fn toDynamicOwned(*this): DynamicOwned([]u8) {
        return .fromRawIn(this.toOwnedSlice(), this.allocator);
    }

    /// `self.allocator` must be `bun.default_allocator`.
    pub fn toDefaultOwned(*this): Owned([]u8) {
        bun.safety.alloc.assertEq(this.allocator, bun.default_allocator);
        return .fromRaw(this.toOwnedSlice());
    }

    pub fn slice(*this): []u8 {
        return this.list.items;
    }

    /// Appends `0` if needed
    pub fn sliceWithSentinel(*this): [:0]u8 {
        if (this.list.items.len > 0 && this.list.items[this.list.items.len - 1] != 0) {
            bun.handleOom(this.list.append(this.allocator, 0));
        }
        return this.list.items[0 .. this.list.items.len - 1 :0];
    }

    pub fn toOwnedSliceLength(*this, length: usize): string {
        this.list.items.len = length;
        return this.toOwnedSlice();
    }

    pub fn containsChar(this: *const MutableString, char: u8): bool {
        return this.indexOfChar(char) != null;
    }

    pub fn indexOfChar(this: *const MutableString, char: u8): ?u32 {
        return strings.indexOfChar(this.list.items, char);
    }

    pub fn lastIndexOfChar(this: *const MutableString, char: u8): ?usize {
        return strings.lastIndexOfChar(this.list.items, char);
    }

    pub fn lastIndexOf(this: *const MutableString, str: u8): ?usize {
        return strings.lastIndexOfChar(this.list.items, str);
    }

    pub fn indexOf(this: *const MutableString, str: u8): ?usize {
        return std.mem.indexOf(u8, this.list.items, str);
    }

    pub fn eql(*this, other: anytype): bool {
        return std.mem.eql(u8, this.list.items, other);
    }

    pub fn toSocketBuffers(*this, comptime count: usize, ranges: anytype): [count]std.posix.iovec_const {
        let buffers: [count]std.posix.iovec_const = undefined;
        comptime let i: usize = 0;
        inline while (i < count) {
            const r = ranges[i];
            buffers[i] = {
                iov_base: this.list.items[r[0]..r[1]].ptr,
                iov_len: this.list.items[r[0]..r[1]].len,
            };
            i += 1;
        }
        return buffers;
    }

    pub fn writeAll(*this, bytes: string): Allocator.Error!usize {
        this.list.appendSlice(this.allocator, bytes)?;
        return bytes.len;
    }
}
