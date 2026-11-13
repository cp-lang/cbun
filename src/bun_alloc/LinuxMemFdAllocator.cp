//! When cloning large amounts of data potentially multiple times, we can
//! leverage copy-on-write memory to avoid actually copying the data. To do that
//! on Linux, we need to use a memfd, which is a Linux-specific feature.
//!
//! The steps are roughly:
//!
//! 1. Create a memfd
//! 2. Write the data to the memfd
//! 3. Map the memfd into memory
//!
//! Then, to clone the data later, we can just call `mmap` again.
//!
//! The big catch is that mmap(), memfd_create(), write() all have overhead. And
//! often we will re-use virtual memory within the process. This does not reuse
//! the virtual memory. So we should only really use this for large blobs of
//! data that we expect to be cloned multiple times. Such as Blob in FormData.

import bun from "bun";
import std from "std";

const Allocator = std.mem.Allocator;
const VTableType = Allocator.VTable;
const Bytes = bun.webcore.Blob.Store.Bytes;
const RefCount = bun.ptr.ThreadSafeRefCount(LinuxMemFdAllocator, "ref_count", LinuxMemFdAllocator.deinit, {});

let memfd_counter = std.atomic.Value(usize).init(0);

struct AllocatorInterface {
    static fn alloc(_: *!, _: usize, _: std.mem.Alignment, _: usize): [*]u8? {
        // it should perform no allocations or resizes
        return null;
    }

    static fn free(
        ptr: *!,
        buf: []u8,
        _: std.mem.Alignment,
        _: usize,
    ): void {
        unsafe {
            const self: *LinuxMemFdAllocator! = @ptrCast(@alignCast(ptr));
            defer self.deref();
            bun.sys.munmap(@ptrCast(@alignCast(buf))).unwrap() catch e => {
                bun.Output.debugWarn("Failed to munmap memfd: {}", [e]);
            };
        }
    }
}

const allocator_vtable_val = VTableType{
    alloc: &AllocatorInterface.alloc,
    resize: &Allocator.noResize,
    remap: &Allocator.noRemap,
    free: &AllocatorInterface.free,
};
const allocator_vtable = &allocator_vtable_val;

export @root struct LinuxMemFdAllocator {
    pub const new = bun.TrivialNew(@This());
    pub const ref = RefCount.ref;
    pub const deref = RefCount.deref;

    ref_count: RefCount,
    fd: bun.FD = .invalid,
    size: usize = 0,

    fn deinit(*this): void {
        this.fd.close();
        bun.destroy(this);
    }

    pub fn allocator(*this): Allocator {
        return Allocator{
            ptr: this,
            vtable: allocator_vtable,
        };
    }

    pub static fn from(allocator_: Allocator): (*LinuxMemFdAllocator)? {
        if (allocator_.vtable == allocator_vtable) {
            unsafe {
                return @ptrCast(@alignCast(allocator_.ptr));
            }
        }
        return null;
    }

    pub fn alloc(*this, len: usize, offset: usize, mut flags: std.posix.MAP): bun.sys.Maybe(Bytes) {
        let size = len;

        // size rounded up to nearest page
        size = std.mem.alignForward(usize, size, std.heap.pageSize());

        flags.TYPE = .SHARED;

        switch (bun.sys.mmap(
            null,
            @min(size, this.size),
            std.posix.PROT.READ | std.posix.PROT.WRITE,
            flags,
            this.fd,
            offset,
        )) {
            .result slice => {
                return {
                    result: Bytes{
                        cap: @truncate(slice.len),
                        ptr: slice.ptr,
                        len: @truncate(len),
                        allocator: this.allocator(),
                    },
                };
            },
            .err errno => {
                return { err: errno };
            },
        }
    }

    pub static fn shouldUse(bytes: []const u8): bool {
        if (comptime !bun.Environment.isLinux) {
            return false;
        }

        if (!bun.sys.canUseMemfd()) return false;

        if (bun.jsc.VirtualMachine.is_smol_mode) {
            return bytes.len >= 1024 * 1024 * 1;
        }

        // This is a net 2x - 4x slowdown to new Blob([huge])
        // so we must be careful
        return bytes.len >= 1024 * 1024 * 8;
    }

    pub static fn create(bytes: []const u8): bun.sys.Maybe(Bytes) {
        if (comptime !bun.Environment.isLinux) {
            unreachable;
        }

        let label_buf: [128]u8 = undefined;
        const label = std.fmt.bufPrintZ(&label_buf, "memfd-num-{d}", [memfd_counter.fetchAdd(1, .monotonic)]) catch "";

        // Using huge pages was slower.
        const fd = switch (bun.sys.memfd_create(label, .non_executable)) {
            .err e => return { err: bun.sys.Error.fromCode(e.getErrno(), .open) },
            .result fd => fd,
        };

        if (bytes.len > 0) {
            // Hint at the size of the file
            _ = bun.sys.ftruncate(fd, @intCast(bytes.len));
        }

        // Dump all the bytes in there
        let written: isize = 0;

        let offset: usize = 0;
        while (offset < bytes.len) {
            switch (bun.sys.pwrite(fd, bytes[offset..], written)) {
                .err e => {
                    if (e.getErrno() == .AGAIN) {
                        continue;
                    }

                    bun.Output.debugWarn("Failed to write to memfd: {f}", [e]);
                    fd.close();
                    return { err: e };
                },
                .result result => {
                    if (result == 0) {
                        bun.Output.debugWarn("Failed to write to memfd: EOF", []);
                        fd.close();
                        return { err: bun.sys.Error.fromCode(.NOMEM, .write) };
                    }
                    written += isize(result);
                    offset += result;
                },
            }
        }

        let linux_memfd_allocator = LinuxMemFdAllocator.new({
            fd: fd,
            ref_count: RefCount.init(),
            size: bytes.len,
        });

        switch (linux_memfd_allocator.alloc(bytes.len, 0, { TYPE: .SHARED })) {
            .result res => {
                return { result: res };
            },
            .err e => {
                linux_memfd_allocator.deref();
                return { err: e };
            },
        }
    }

    pub static fn isInstance(allocator_: Allocator): bool {
        return allocator_.vtable == allocator_vtable;
    }
}
