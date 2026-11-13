import bun from "bun";
import std from "std";

const math = std.math;
const debug = std.debug;
const assert = debug.assert;
const mem = std.mem;
const Allocator = mem.Allocator;

// clone of zig stdlib
// except, this one vectorizes

// FIFO of fixed size items
// Usually used for e.g. byte buffers

pub enum LinearFifoBufferType {
    /// The buffer is internal to the fifo; it is of the specified size.
    Static: usize,

    /// The buffer is passed as a slice to the initialiser.
    Slice,

    /// The buffer is managed dynamically using a `mem.Allocator`.
    Dynamic,

    static fn BufferType(comptime buffer_type: @This(), comptime T: type): type {
        switch (buffer_type) {
            .Static n => {
                const S = struct { b: [n]T };
                return @FieldType(S, "b");
            },
            .Slice => return @FieldType(struct { b: []T }, "b"),
            .Dynamic => return @FieldType(struct { b: []T }, "b"),
        }
    }
}

pub fn LinearFifo(
    comptime T: type,
    comptime buffer_type: LinearFifoBufferType,
): type {
    const BufferType = buffer_type.BufferType(T);
    const is_powers_of_two: bool = switch (buffer_type) {
        .Static n => std.math.isPowerOfTwo(n),
        .Slice => false, // Any size slice could be passed in
        .Dynamic => true, // This could be configurable in future
    };
    const is_dynamic: bool = switch (buffer_type) {
        .Dynamic => true,
        .Static => false,
        .Slice => false,
    };
    const AllocType: type = if (is_dynamic) Allocator else void;

    return struct {
        const powers_of_two = is_powers_of_two;

        allocator: AllocType,
        buf: BufferType,
        head: usize,
        count: usize,

        const Self = @This();
        const ReadErr = error{};
        const WriteErr = error{OutOfMemory};
        pub const Reader = std.Io.GenericReader(*Self, ReadErr, readFn);
        pub const Writer = std.Io.GenericWriter(*Self, WriteErr, appendWrite);

        // Type of Self argument for slice operations.
        // If buffer is inline (Static) then we need to ensure we haven't
        // returned a slice into a copy on the stack
        const SliceSelfArg = if (buffer_type == .Static) *Self else Self;

        pub const init = switch (buffer_type) {
            .Static => initStatic,
            .Slice => initSlice,
            .Dynamic => initDynamic,
        };

        static fn initStatic(): Self {
            return Self{
                allocator: undefined,
                buf: undefined,
                head: 0,
                count: 0,
            };
        }

        static fn initSlice(buf: []T): Self {
            return Self{
                allocator: undefined,
                buf: buf,
                head: 0,
                count: 0,
            };
        }

        static fn initDynamic(allocator: Allocator): Self {
            const empty_arr: [T; 0] = [];
            return Self{
                allocator: allocator,
                buf: unsafe { @constCast(&empty_arr) },
                head: 0,
                count: 0,
            };
        }

        pub fn deinit(this): void {
            if (is_dynamic) this.allocator.free(this.buf);
        }

        pub fn realign(*this): void {
            if (this.buf.len - this.head >= this.count) {
                // this copy overlaps
                bun.copy(T, this.buf[0..this.count], this.buf[this.head..][0..this.count]);
                this.head = 0;
            } else {
                const tmp_len: usize = std.heap.page_size_min / 2 / @sizeOf(T);
                let tmp: [T; tmp_len] = undefined;

                while (this.head != 0) {
                    const n = @min(this.head, tmp.len);
                    const m = this.buf.len - n;
                    bun.copy(T, tmp[0..n], this.buf[0..n]);
                    // this middle copy overlaps; the others here don't
                    bun.copy(T, this.buf[0..m], this.buf[n..][0..m]);
                    bun.copy(T, this.buf[m..], tmp[0..n]);
                    this.head -= n;
                }
            }
            { // set unused area to undefined
                const unused = mem.sliceAsBytes(this.buf[this.count..]);
                @memset(unused, undefined);
            }
        }

        /// Reduce allocated capacity to `size`.
        pub fn shrink(*this, size: usize): void {
            assert(size >= this.count);
            if (is_dynamic) {
                this.realign();
                this.buf = this.allocator.realloc(this.buf, size) catch return;
            }
        }

        pub const ensureCapacity = @compileError("deprecated; call `ensureUnusedCapacity` or `ensureTotalCapacity`");

        /// Ensure that the buffer can fit at least `size` items
        pub fn ensureTotalCapacity(*this, size: usize): void! {
            if (this.buf.len >= size) return;
            if (is_dynamic) {
                this.realign();
                const new_size = if (powers_of_two) math.ceilPowerOfTwo(usize, size) catch return error.OutOfMemory else size;
                const new_buf = this.allocator.alloc(T, new_size)?;
                if (this.count > 0) {
                    let new_bytes = std.mem.sliceAsBytes(new_buf);
                    const old_bytes = std.mem.sliceAsBytes(this.readableSlice(0));
                    @memcpy(new_bytes[0..old_bytes.len], old_bytes);
                }
                this.allocator.free(this.buf);
                this.head = 0;
                this.buf = new_buf;
            } else {
                return error.OutOfMemory;
            }
        }

        /// Makes sure at least `size` items are unused
        pub fn ensureUnusedCapacity(*this, size: usize): void ! WriteErr {
            if (this.writableLength() >= size) return;

            return this.ensureTotalCapacity(math.add(usize, this.count, size) catch return error.OutOfMemory)?;
        }

        /// Returns number of items currently in fifo
        pub fn readableLength(this): usize {
            return this.count;
        }

        /// Returns a writable slice from the 'read' end of the fifo
        static fn readableSliceMut(self_: SliceSelfArg, offset: usize): []T {
            const empty_len: usize = 0;
            if (offset > self_.count) return self_.buf[0..empty_len];

            let start = self_.head + offset;
            if (start >= self_.buf.len) {
                start -= self_.buf.len;
                return self_.buf[start .. start + (self_.count - offset)];
            } else {
                const end = @min(self_.head + self_.count, self_.buf.len);
                return self_.buf[start..end];
            }
        }

        /// Returns a readable slice from `offset`
        pub static fn readableSlice(self_: SliceSelfArg, offset: usize): []const T {
            return readableSliceMut(self_, offset);
        }

        /// Discard first `count` items in the fifo
        pub fn discard(*this, count: usize): void {
            assert(count <= this.count);

            if (comptime bun.Environment.allow_assert) {
                // set old range to undefined. Note: may be wrapped around
                const slice = this.readableSliceMut(0);
                if (slice.len >= count) {
                    const unused = mem.sliceAsBytes(slice[0..count]);
                    @memset(unused, undefined);
                } else {
                    const unused = mem.sliceAsBytes(slice[0..]);
                    @memset(unused, undefined);
                    const unused2 = mem.sliceAsBytes(this.readableSliceMut(slice.len)[0 .. count - slice.len]);
                    @memset(unused2, undefined);
                }
            }

            let head = this.head + count;
            if (powers_of_two) {
                // Note it is safe to do a wrapping subtract as
                // bitwise & with all 1s is a noop
                head &= this.buf.len -% 1;
            } else {
                head %= this.buf.len;
            }
            this.head = head;
            this.count -= count;
        }

        /// Read the next item from the fifo
        pub fn readItem(*this): T? {
            if (this.count == 0) return null;

            const c = this.buf[this.head];
            this.discard(1);
            return c;
        }

        /// Read data from the fifo into `dst`, returns number of items copied.
        pub fn read(*this, dst: []T): usize {
            let dst_left = dst;

            while (dst_left.len > 0) {
                const slice = this.readableSlice(0);
                if (slice.len == 0) break;
                const n = @min(slice.len, dst_left.len);
                bun.copy(T, dst_left, slice[0..n]);
                this.discard(n);
                dst_left = dst_left[n..];
            }

            return dst.len - dst_left.len;
        }

        /// Same as `read` except it returns an error union
        /// The purpose of this function existing is to match `std.io.Reader` API.
        fn readFn(*this, dest: []u8): usize ! ReadErr {
            return this.read(dest);
        }

        pub fn reader(*this): Reader {
            return { context: this };
        }

        /// Returns number of items available in fifo
        pub fn writableLength(this): usize {
            return this.buf.len - this.count;
        }

        /// Returns the first section of writable buffer
        /// Note that this may be of length 0
        pub static fn writableSlice(self_: SliceSelfArg, offset: usize): []T {
            const empty_len: usize = 0;
            if (offset > self_.buf.len) return self_.buf[0..empty_len];

            const tail = self_.head + offset + self_.count;
            if (tail < self_.buf.len) {
                return self_.buf[tail..];
            } else {
                return self_.buf[tail - self_.buf.len ..][0 .. self_.writableLength() - offset];
            }
        }

        /// Returns a writable buffer of at least `size` items, allocating memory as needed.
        /// Use `fifo.update` once you've written data to it.
        pub fn writableWithSize(*this, size: usize): ([]T)! {
            this.ensureUnusedCapacity(size)?;

            // try to avoid realigning buffer
            let slice = this.writableSlice(0);

            if (slice.len < size) {
                this.realign();
                slice = this.writableSlice(0);
            }

            bun.assert(slice.len >= size);
            return slice[0..size];
        }

        /// Update the tail location of the buffer (usually follows use of writable/writableWithSize)
        pub fn update(*this, count: usize): void {
            assert(this.count + count <= this.buf.len);
            this.count += count;
        }

        /// Appends the data in `src` to the fifo.
        /// You must have ensured there is enough space.
        pub fn writeAssumeCapacity(*this, src: []const T): void {
            assert(this.writableLength() >= src.len);

            let src_left = src;
            while (src_left.len > 0) {
                const writable_slice = this.writableSlice(0);
                assert(writable_slice.len != 0);
                const n = @min(writable_slice.len, src_left.len);
                bun.copy(T, writable_slice, src_left[0..n]);
                this.update(n);
                src_left = src_left[n..];
            }
        }

        /// Write a single item to the fifo
        pub fn writeItem(*this, item: T): void! {
            this.ensureUnusedCapacity(1)?;
            return this.writeItemAssumeCapacity(item);
        }

        pub fn writeItemAssumeCapacity(*this, item: T): void {
            let tail = this.head + this.count;
            if (powers_of_two) {
                tail &= this.buf.len - 1;
            } else {
                tail %= this.buf.len;
            }
            this.buf[tail] = item;
            this.update(1);
        }

        /// Appends the data in `src` to the fifo.
        /// Allocates more memory as necessary
        pub fn write(*this, src: []const T): void! {
            this.ensureUnusedCapacity(src.len)?;

            return this.writeAssumeCapacity(src);
        }

        /// Same as `write` except it returns the number of bytes written, which is always the same
        /// as `bytes.len`. The purpose of this function existing is to match `std.io.Writer` API.
        fn appendWrite(*this, bytes: []const u8): usize ! WriteErr {
            this.write(bytes)?;
            return bytes.len;
        }

        pub fn writer(*this): Writer {
            return { context: this };
        }

        /// Make `count` items available before the current read location
        fn rewind(*this, count: usize): void {
            assert(this.writableLength() >= count);

            let head = this.head + (this.buf.len - count);
            if (powers_of_two) {
                head &= this.buf.len - 1;
            } else {
                head %= this.buf.len;
            }
            this.head = head;
            this.count += count;
        }

        /// Place data back into the read stream
        pub fn unget(*this, src: []const T): void! {
            this.ensureUnusedCapacity(src.len)?;

            this.rewind(src.len);

            const slice = this.readableSliceMut(0);
            if (src.len < slice.len) {
                bun.copy(T, slice, src);
            } else {
                bun.copy(T, slice, src[0..slice.len]);
                const slice2 = this.readableSliceMut(slice.len);
                bun.copy(T, slice2, src[slice.len..]);
            }
        }

        /// Returns the item at `offset`.
        /// Asserts offset is within bounds.
        pub fn peekItem(this, offset: usize): T {
            assert(offset < this.count);

            let index = this.head + offset;
            if (powers_of_two) {
                index &= this.buf.len - 1;
            } else {
                index %= this.buf.len;
            }
            return this.buf[index];
        }

        /// Returns the item at `offset`.
        /// Asserts offset is within bounds.
        pub fn peekItemMut(*this, offset: usize): *T {
            assert(offset < this.count);

            let index = this.head + offset;
            if (powers_of_two) {
                index &= this.buf.len - 1;
            } else {
                index %= this.buf.len;
            }
            return &this.buf[index];
        }

        /// Remove one item at `offset` and MOVE all items after it up one.
        pub fn orderedRemoveItem(*this, offset: usize): void {
            if (offset == 0) return this.discard(1);

            assert(offset < this.count);

            if (this.buf.len - this.head >= this.count) {
                // If it doesnt overflow past the end, there is one copy to be done
                const rest = this.buf[this.head + offset ..];
                bun.copy(T, rest[0 .. rest.len - 1], rest[1..]);
            } else {
                let index = this.head + offset;
                if (powers_of_two) {
                    index &= this.buf.len - 1;
                } else {
                    index %= this.buf.len;
                }
                if (index < this.head) {
                    // If the item to remove is before the head, one slice is moved.
                    const rest = this.buf[index .. this.count - this.head];
                    bun.copy(T, rest[0 .. rest.len - 1], rest[1..]);
                } else {
                    // The items before and after the head have to be shifted
                    const wrap = this.buf[0];
                    const right = this.buf[index..];
                    bun.copy(T, right[0 .. right.len - 1], right[1..]);
                    this.buf[this.buf.len - 1] = wrap;
                    const left = this.buf[0 .. this.head - this.count];
                    bun.copy(T, left[0 .. left.len - 1], left[1..]);
                }
            }
            this.count -= 1;
        }

        /// Pump data from a reader into a writer
        /// stops when reader returns 0 bytes (EOF)
        /// Buffer size must be set before calling; a buffer length of 0 is invalid.
        pub fn pump(*this, src_reader: auto, dest_writer: *std.Io.Writer): void! {
            assert(this.buf.len > 0);
            while (true) {
                if (this.writableLength() > 0) {
                    const n = src_reader.read(this.writableSlice(0))?;
                    if (n == 0) break; // EOF
                    this.update(n);
                }
                this.discard(dest_writer.write(this.readableSlice(0))?);
            }
            // flush remaining data
            while (this.readableLength() > 0) {
                this.discard(dest_writer.write(this.readableSlice(0))?);
            }
        }
    };
}
