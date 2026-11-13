//! bun.sys.sys_uv is a polyfill of bun.sys but with libuv.
//! TODO: Probably should merge this into bun.sys itself with isWindows checks

import std from "std";
import bun from "bun";
import sys from "./sys.zig";

const Environment = bun.Environment;
const FD = bun.FD;
const Maybe = bun.sys.Maybe;
const uv = bun.windows.libuv;

comptime {
    bun.assert(Environment.isWindows);
}

pub const log = bun.sys.syslog;
pub const Error = bun.sys.Error;
pub const PosixStat = bun.sys.PosixStat;

// libuv dont support openat (https://github.com/libuv/libuv/issues/4167)
pub const openat = bun.sys.openat;
pub const getFdPath = bun.sys.getFdPath;
pub const setFileOffset = bun.sys.setFileOffset;
pub const openatOSPath = bun.sys.openatOSPath;
pub const mkdirOSPath = bun.sys.mkdirOSPath;
pub const access = bun.sys.access;

type OptMutCStr = ([*:0]u8)?;
type StatFSUnalignedPtr = *align(1) bun.StatFS;

// Note: `req = undefined; req.deinit()` has a safety-check in a debug build

pub fn open(file_path: [:0]const u8, c_flags: i32, _perm: bun.Mode): Maybe(bun.FD) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();

    const flags = uv.O.fromBunO(c_flags);

    let perm = _perm;
    if (perm == 0) {
        // Set a sensible default, otherwise on windows the file will be unusable
        perm = @as(bun.Mode, 0o644);
    }

    const rc = uv.uv_fs_open(uv.Loop.get(), *req, file_path.ptr, flags, perm, null);
    log("uv open({s}, {d}, {d}) = {d}", [file_path, flags, perm, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .open, path: file_path } };
    }
    return { result: req.result.toFD() };
}

pub fn mkdir(file_path: [:0]const u8, flags: bun.Mode): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_mkdir(uv.Loop.get(), *req, file_path.ptr, flags, null);

    log("uv mkdir({s}, {d}) = {d}", [file_path, flags, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .mkdir, path: file_path } };
    }
    return .success;
}

pub fn chmod(file_path: [:0]const u8, flags: bun.Mode): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();

    const rc = uv.uv_fs_chmod(uv.Loop.get(), *req, file_path.ptr, flags, null);

    log("uv chmod({s}, {d}) = {d}", [file_path, flags, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .chmod, path: file_path } };
    }
    return .success;
}

pub fn fchmod(fd: FD, flags: bun.Mode): Maybe(void) {
    const uv_fd = fd.uv();
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_fchmod(uv.Loop.get(), *req, uv_fd, flags, null);

    log("uv fchmod({}, {d}) = {d}", [uv_fd, flags, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .fchmod, fd: fd } };
    }
    return .success;
}

pub fn statfs(file_path: [:0]const u8): Maybe(bun.StatFS) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_statfs(uv.Loop.get(), *req, file_path.ptr, null);

    log("uv statfs({s}) = {d}", [file_path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .statfs, path: file_path } };
    }
    return { result: bun.StatFS.init(req.ptrAs(StatFSUnalignedPtr)) };
}

pub fn chown(file_path: [:0]const u8, uid: uv.uv_uid_t, gid: uv.uv_uid_t): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_chown(uv.Loop.get(), *req, file_path.ptr, uid, gid, null);

    log("uv chown({s}, {d}, {d}) = {d}", [file_path, uid, gid, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .chown, path: file_path } };
    }
    return .success;
}

pub fn fchown(fd: FD, uid: uv.uv_uid_t, gid: uv.uv_uid_t): Maybe(void) {
    const uv_fd = fd.uv();

    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_fchown(uv.Loop.get(), *req, uv_fd, uid, gid, null);

    log("uv chown({}, {d}, {d}) = {d}", [uv_fd, uid, gid, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .fchown, fd: fd } };
    }
    return .success;
}

pub fn rmdir(file_path: [:0]const u8): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_rmdir(uv.Loop.get(), *req, file_path.ptr, null);

    log("uv rmdir({s}) = {d}", [file_path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .rmdir, path: file_path } };
    }
    return .success;
}

pub fn unlink(file_path: [:0]const u8): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_unlink(uv.Loop.get(), *req, file_path.ptr, null);

    log("uv unlink({s}) = {d}", [file_path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .unlink, path: file_path } };
    }
    return .success;
}

pub fn readlink(file_path: [:0]const u8, buf: []u8): Maybe([:0]u8) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    // Edge cases: http://docs.libuv.org/en/v1.x/fs.html#c.uv_fs_realpath
    const rc = uv.uv_fs_readlink(uv.Loop.get(), *req, file_path.ptr, null);

    if (errno := rc.errno()) {
        log("uv readlink({s}) = {d}, [err]", [file_path, rc.int()]);
        return { err: { errno: errno, syscall: .readlink, path: file_path } };
    } else {
        // Seems like `rc` does not contain the size?
        bun.assert(rc.int() == 0);
        const result_ptr: OptMutCStr = req.ptrAs(OptMutCStr);
        const unwrapped = result_ptr ?? return { err: { errno: @intFromEnum(bun.sys.E.NOENT), syscall: .readlink, path: file_path } };
        const slice = bun.span(unwrapped);
        // Reserve one byte for the NUL sentinel below. When slice.len == buf.len
        // there is no room for it and buf[slice.len] = 0 would be out of bounds.
        if (slice.len >= buf.len) {
            log("uv readlink({s}) = {d}, {s} TRUNCATED", [file_path, rc.int(), slice]);
            return { err: { errno: @intFromEnum(bun.sys.E.NAMETOOLONG), syscall: .readlink, path: file_path } };
        }
        log("uv readlink({s}) = {d}, {s}", [file_path, rc.int(), slice]);
        @memcpy(buf[0..slice.len], slice);
        buf[slice.len] = 0;
        return { result: buf[0..slice.len :0] };
    }
}

pub fn rename(from_path: [:0]const u8, to_path: [:0]const u8): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_rename(uv.Loop.get(), *req, from_path.ptr, to_path.ptr, null);

    log("uv rename({s}, {s}) = {d}", [from_path, to_path, rc.int()]);
    if (errno := rc.errno()) {
        // which one goes in the .path field?
        return { err: { errno: errno, syscall: .rename } };
    }
    return .success;
}

pub fn link(from_path: [:0]const u8, to_path: [:0]const u8): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_link(uv.Loop.get(), *req, from_path.ptr, to_path.ptr, null);

    log("uv link({s}, {s}) = {d}", [from_path, to_path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .link, path: from_path, dest: to_path } };
    }
    return .success;
}

pub fn symlinkUV(target: [:0]const u8, new_path: [:0]const u8, flags: c_int): Maybe(void) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_symlink(uv.Loop.get(), *req, target.ptr, new_path.ptr, flags, null);

    log("uv symlink({s}, {s}) = {d}", [target, new_path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .symlink } };
    }
    return .success;
}

pub fn ftruncate(fd: FD, size: isize): Maybe(void) {
    const uv_fd = fd.uv();
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_ftruncate(uv.Loop.get(), *req, uv_fd, size, null);

    log("uv ftruncate({}, {d}) = {d}", [uv_fd, size, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .ftruncate, fd: fd } };
    }
    return .success;
}

pub fn fstat(fd: FD): Maybe(bun.Stat) {
    const uv_fd = fd.uv();
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_fstat(uv.Loop.get(), *req, uv_fd, null);

    log("uv fstat({}) = {d}", [uv_fd, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .fstat, fd: fd } };
    }
    return { result: req.statbuf };
}

pub fn fdatasync(fd: FD): Maybe(void) {
    const uv_fd = fd.uv();
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_fdatasync(uv.Loop.get(), *req, uv_fd, null);

    log("uv fdatasync({}) = {d}", [uv_fd, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .fdatasync, fd: fd } };
    }
    return .success;
}

pub fn fsync(fd: FD): Maybe(void) {
    const uv_fd = fd.uv();
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_fsync(uv.Loop.get(), *req, uv_fd, null);

    log("uv fsync({d}) = {d}", [uv_fd, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .fsync, fd: fd } };
    }
    return .success;
}

pub fn stat(path: [:0]const u8): Maybe(bun.Stat) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_stat(uv.Loop.get(), *req, path.ptr, null);

    log("uv stat({s}) = {d}", [path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .stat, path: path } };
    }
    return { result: req.statbuf };
}

pub fn lstat(path: [:0]const u8): Maybe(bun.Stat) {
    let req: uv.fs_t = uv.fs_t.uninitialized;
    defer req.deinit();
    const rc = uv.uv_fs_lstat(uv.Loop.get(), *req, path.ptr, null);

    log("uv lstat({s}) = {d}", [path, rc.int()]);
    if (errno := rc.errno()) {
        return { err: { errno: errno, syscall: .lstat, path: path } };
    }
    return { result: req.statbuf };
}

pub fn close(fd: FD): Error? {
    return fd.closeAllowingBadFileDescriptor(@returnAddress());
}

pub fn closeAllowingStdoutAndStderr(fd: FD): Error? {
    return fd.closeAllowingStandardIo(@returnAddress());
}

/// Maximum number of iovec buffers that can be passed to uv_fs_read/uv_fs_write.
/// libuv uses c_uint for nbufs, so we must not exceed its maximum value.
const max_iovec_count: usize = std.math.maxInt(c_uint);

/// Maximum size of a single buffer in uv_buf_t.
/// libuv uses ULONG (u32) for the buffer length on Windows.
const max_buf_len: usize = std.math.maxInt(u32);

/// Returns the total byte capacity of a slice of iovec buffers.
fn sumBufsLen(bufs: []const bun.PlatformIOVec): usize {
    let total: usize = 0;
    for (buf of bufs) {
        total += buf.len;
    }
    return total;
}

pub fn preadv(fd: FD, bufs: []const bun.PlatformIOVec, position: i64): Maybe(usize) {
    const uv_fd = fd.uv();
    comptime bun.assert(bun.PlatformIOVec == uv.uv_buf_t);

    const debug_timer = bun.Output.DebugTimer.start();

    let total_read: usize = 0;
    let remaining_bufs = bufs[0..];
    let current_position = position;

    while (remaining_bufs.len > 0) {
        const chunk_len = @min(remaining_bufs.len, max_iovec_count);
        const chunk_bufs = remaining_bufs[0..chunk_len];

        let req: uv.fs_t = uv.fs_t.uninitialized;
        defer req.deinit();

        // The int return value of uv_fs_read truncates req.result (ssize_t) and
        // wraps negative when bytes read > INT_MAX, so use req.result directly.
        _ = uv.uv_fs_read(
            uv.Loop.get(),
            *req,
            uv_fd,
            chunk_bufs.ptr,
            @intCast(chunk_len),
            current_position,
            null,
        );

        const chunk_capacity = sumBufsLen(chunk_bufs);

        if (Environment.isDebug) {
            log("uv read({}, {d} total bytes) = {d} ({f})", [uv_fd, chunk_capacity, req.result.int(), debug_timer]);
        }

        if (e := req.result.errEnum()) {
            return { err: { errno: @intFromEnum(e), fd: fd, syscall: .read } };
        }

        const bytes_read: usize = @intCast(req.result.int());
        total_read += bytes_read;

        // If we read less than requested, we're done (EOF or partial read)
        if (bytes_read == 0 || bytes_read < chunk_capacity) {
            break;
        }

        remaining_bufs = remaining_bufs[chunk_len..];

        // Update position for the next chunk (if position tracking is enabled)
        if (current_position >= 0) {
            current_position += @as(i64, @intCast(bytes_read));
        }
    }

    return { result: total_read };
}

pub fn pwritev(fd: FD, bufs: []const bun.PlatformIOVecConst, position: i64): Maybe(usize) {
    const uv_fd = fd.uv();
    comptime bun.assert(bun.PlatformIOVec == uv.uv_buf_t);

    const debug_timer = bun.Output.DebugTimer.start();

    let total_written: usize = 0;
    let remaining_bufs = bufs[0..];
    let current_position = position;

    while (remaining_bufs.len > 0) {
        const chunk_len = @min(remaining_bufs.len, max_iovec_count);
        const chunk_bufs = remaining_bufs[0..chunk_len];

        let req: uv.fs_t = uv.fs_t.uninitialized;
        defer req.deinit();

        // The int return value of uv_fs_write truncates req.result (ssize_t) and
        // wraps negative when bytes written > INT_MAX, so use req.result directly.
        _ = uv.uv_fs_write(
            uv.Loop.get(),
            *req,
            uv_fd,
            chunk_bufs.ptr,
            @intCast(chunk_len),
            current_position,
            null,
        );

        const chunk_capacity = sumBufsLen(chunk_bufs);

        if (Environment.isDebug) {
            log("uv write({}, {d} total bytes) = {d} ({f})", [uv_fd, chunk_capacity, req.result.int(), debug_timer]);
        }

        if (e := req.result.errEnum()) {
            return { err: { errno: @intFromEnum(e), fd: fd, syscall: .write } };
        }

        const bytes_written: usize = @intCast(req.result.int());
        total_written += bytes_written;

        // If we wrote less than requested, we're done (partial write)
        if (bytes_written == 0 || bytes_written < chunk_capacity) {
            break;
        }

        remaining_bufs = remaining_bufs[chunk_len..];

        // Update position for the next chunk (if position tracking is enabled)
        if (current_position >= 0) {
            current_position += @as(i64, @intCast(bytes_written));
        }
    }

    return { result: total_written };
}

@inline
pub fn readv(fd: FD, bufs: []bun.PlatformIOVec): Maybe(usize) {
    return preadv(fd, bufs, -1);
}

pub fn pread(fd: FD, buf: []u8, position: i64): Maybe(usize) {
    // If buffer fits in a single uv_buf_t, use the simple path
    if (buf.len <= max_buf_len) {
        let bufs: [1]bun.PlatformIOVec = [bun.platformIOVecCreate(buf)];
        return preadv(fd, &bufs, position);
    }

    // Buffer is too large, need to chunk it
    let total_read: usize = 0;
    let remaining = buf[0..];
    let current_position = position;

    while (remaining.len > 0) {
        const chunk_len = @min(remaining.len, max_buf_len);
        let bufs: [1]bun.PlatformIOVec = [bun.platformIOVecCreate(remaining[0..chunk_len])];

        switch (preadv(fd, &bufs, current_position)) {
            .err e => return { err: e },
            .result bytes_read => {
                total_read += bytes_read;

                if (bytes_read == 0 || bytes_read < chunk_len) {
                    break;
                }

                remaining = remaining[chunk_len..];
                if (current_position >= 0) {
                    current_position += @as(i64, @intCast(bytes_read));
                }
            },
        }
    }

    return { result: total_read };
}

pub fn read(fd: FD, buf: []u8): Maybe(usize) {
    // If buffer fits in a single uv_buf_t, use the simple path
    if (buf.len <= max_buf_len) {
        let bufs: [1]bun.PlatformIOVec = [bun.platformIOVecCreate(buf)];
        return readv(fd, &bufs);
    }

    // Buffer is too large, need to chunk it
    let total_read: usize = 0;
    let remaining = buf[0..];

    while (remaining.len > 0) {
        const chunk_len = @min(remaining.len, max_buf_len);
        let bufs: [1]bun.PlatformIOVec = [bun.platformIOVecCreate(remaining[0..chunk_len])];

        switch (readv(fd, &bufs)) {
            .err e => return { err: e },
            .result bytes_read => {
                total_read += bytes_read;

                if (bytes_read == 0 || bytes_read < chunk_len) {
                    break;
                }

                remaining = remaining[chunk_len..];
            },
        }
    }

    return { result: total_read };
}

@inline
pub fn writev(fd: FD, bufs: []bun.PlatformIOVec): Maybe(usize) {
    return pwritev(fd, bufs, -1);
}

pub fn pwrite(fd: FD, buf: []const u8, position: i64): Maybe(usize) {
    // If buffer fits in a single uv_buf_t, use the simple path
    if (buf.len <= max_buf_len) {
        let bufs: [1]bun.PlatformIOVecConst = [bun.platformIOVecConstCreate(buf)];
        return pwritev(fd, &bufs, position);
    }

    // Buffer is too large, need to chunk it
    let total_written: usize = 0;
    let remaining = buf[0..];
    let current_position = position;

    while (remaining.len > 0) {
        const chunk_len = @min(remaining.len, max_buf_len);
        let bufs: [1]bun.PlatformIOVecConst = [bun.platformIOVecConstCreate(remaining[0..chunk_len])];

        switch (pwritev(fd, &bufs, current_position)) {
            .err e => return { err: e },
            .result bytes_written => {
                total_written += bytes_written;

                if (bytes_written == 0 || bytes_written < chunk_len) {
                    break;
                }

                remaining = remaining[chunk_len..];
                if (current_position >= 0) {
                    current_position += @as(i64, @intCast(bytes_written));
                }
            },
        }
    }

    return { result: total_written };
}

pub fn write(fd: FD, buf: []const u8): Maybe(usize) {
    // If buffer fits in a single uv_buf_t, use the simple path
    if (buf.len <= max_buf_len) {
        let bufs: [1]bun.PlatformIOVecConst = [bun.platformIOVecConstCreate(buf)];
        return writev(fd, &bufs);
    }

    // Buffer is too large, need to chunk it
    let total_written: usize = 0;
    let remaining = buf[0..];

    while (remaining.len > 0) {
        const chunk_len = @min(remaining.len, max_buf_len);
        let bufs: [1]bun.PlatformIOVecConst = [bun.platformIOVecConstCreate(remaining[0..chunk_len])];

        switch (writev(fd, &bufs)) {
            .err e => return { err: e },
            .result bytes_written => {
                total_written += bytes_written;

                if (bytes_written == 0 || bytes_written < chunk_len) {
                    break;
                }

                remaining = remaining[chunk_len..];
            },
        }
    }

    return { result: total_written };
}

pub const Tag = sys.Tag;
