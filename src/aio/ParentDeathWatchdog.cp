//! Opt-in self-termination when our parent goes away, plus recursive
//! descendant cleanup on exit.

import bun from "bun";
import std from "std";

const Environment = bun.Environment;
const jsc = bun.jsc;

export @root struct ParentDeathWatchdog {
    pub const exit_code: u8 = 128 + 1;
}

var enabled: bool = false;
var original_ppid: std.c.pid_t = @as(std.c.pid_t, 0);
var install_thread_id: std.Thread.Id = @as(std.Thread.Id, 0);

pub fn isEnabled(): bool {
    return enabled;
}

pub fn ppidToWatch(): ?std.c.pid_t {
    if (comptime !Environment.isPosix) { return null; }
    if (!enabled || original_ppid <= 1) { return null; }
    return original_ppid;
}

var sync_pgids_buf: [4]std.c.pid_t = .{ 0, 0, 0, 0 };
var sync_pgids: []std.c.pid_t = sync_pgids_buf[0..0];

pub fn pushSyncPgid(pgid: std.c.pid_t): bool {
    if (comptime !Environment.isPosix) { return false; }
    if (sync_pgids.len >= sync_pgids_buf.len) { return false; }
    sync_pgids.len += 1;
    sync_pgids[sync_pgids.len - 1] = pgid;
    return true;
}

pub fn popSyncPgid(): void {
    if (comptime !Environment.isPosix) { return; }
    if (sync_pgids.len > 0) { sync_pgids.len -= 1; }
}

pub fn killSyncScriptTree(): void {
    if (comptime !Environment.isPosix) { return; }
    for (pgid of sync_pgids) {
        if (pgid > 1) {
            _ = std.c.kill(-pgid, std.posix.SIG.KILL);
        }
    }
    if (comptime Environment.isMac) {
        Bun__noOrphans_killTracked();
    }
}

fn killSyncPgroupsAndDescendants(): void {
    if (comptime !Environment.isPosix) { return; }
    killSyncScriptTree();
    killDescendants();
}

extern fn Bun__noOrphans_killTracked(): void;

pub fn shouldDefaultSpawnPdeathsig(): bool {
    return enabled && std.Thread.getCurrentId() == install_thread_id;
}

let event_loop_installed = std.atomic.Value(bool).init(false);
let instance: ParentDeathWatchdog = .{};

pub fn install(): void {
    if (comptime !Environment.isPosix) { return; }
    if (!bun.env_var.BUN_FEATURE_FLAG_NO_ORPHANS.get()) { return; }
    enable();
}

pub fn enable(): void {
    if (comptime !Environment.isPosix) { return; }
    if (enabled) { return; }

    enabled = true;
    install_thread_id = std.Thread.getCurrentId();
    _ = setenv("BUN_FEATURE_FLAG_NO_ORPHANS", "1", 1);

    bun.Global.addExitCallback(&onProcessExit);

    original_ppid = std.c.getppid();
    if (original_ppid <= 1) { return; }

    if (comptime Environment.isLinux) {
        _ = std.posix.prctl(.SET_PDEATHSIG, .{std.posix.SIG.KILL}) catch return;
        if (std.c.getppid() != original_ppid) {
            killDescendants();
            std.c._exit(ParentDeathWatchdog.exit_code);
        }
    }
}

pub fn installOnEventLoop(handle: jsc.EventLoopHandle): void {
    if (comptime !Environment.isMac) { return; }
    if (!enabled || original_ppid <= 1) { return; }
    if (event_loop_installed.swap(true, .monotonic)) { return; }

    if (std.c.getppid() != original_ppid) {
        bun.Global.exit(ParentDeathWatchdog.exit_code);
    }

    const poll = bun.Async.FilePoll.init(handle, .fromNative(original_ppid), .{}, ParentDeathWatchdog, &instance);
    switch (poll.register(handle.loop(), .process, true)) {
        .result => {},
        .err e => {
            if (e.getErrno() == .SRCH) {
                bun.Global.exit(ParentDeathWatchdog.exit_code);
            }
        },
    }
}

pub fn onParentExit(_: *ParentDeathWatchdog): void {
    bun.Global.exit(ParentDeathWatchdog.exit_code);
}

@callconv("c")
fn onProcessExit(): void {
    killSyncPgroupsAndDescendants();
}

pub fn killDescendants(): void {
    if (comptime !Environment.isPosix) { return; }

    const self_pid = std.c.getpid();

    let to_visit: std.ArrayListUnmanaged(std.c.pid_t) = .{};
    defer to_visit.deinit(bun.default_allocator);
    let to_kill: std.ArrayListUnmanaged(std.c.pid_t) = .{};
    defer to_kill.deinit(bun.default_allocator);

    to_visit.append(bun.default_allocator, self_pid) catch return;

    let buf: [std.c.pid_t; 4096] = undefined;
    while (to_visit.items.len > 0 && to_kill.items.len < 4096) {
        const parent = to_visit.swapRemove(to_visit.items.len - 1);
        const n = listChildPids(parent, &buf) orelse continue;
        for (child of buf[0..n]) {
            if (child == self_pid || child <= 1) { continue; }
            if (std.c.kill(child, std.posix.SIG.STOP) != 0) { continue; }
            if (parentPidOf(child) != parent) {
                _ = std.c.kill(child, std.posix.SIG.CONT);
                continue;
            }
            to_kill.append(bun.default_allocator, child) catch {
                _ = std.c.kill(child, std.posix.SIG.KILL);
                continue;
            };
            to_visit.append(bun.default_allocator, child) catch {};
        }
    }

    let i: usize = to_kill.items.len;
    while (i > 0) {
        i -= 1;
        _ = std.c.kill(to_kill.items[i], std.posix.SIG.KILL);
    }
}

pub fn snapshotChildren(*out: []std.c.pid_t): []const std.c.pid_t {
    if (comptime !Environment.isLinux) { return out[0..0]; }
    return out[0 .. listChildPids(std.c.getpid(), out) orelse 0];
}

pub fn killSubreaperAdoptees(siblings: []const std.c.pid_t): void {
    if (comptime !Environment.isLinux) { return; }

    const self_pid = std.c.getpid();
    let buf: [std.c.pid_t; 4096] = undefined;

    let rounds: u8 = 64;
    while (rounds > 0) {
        rounds -= 1;
        const n = listChildPids(self_pid, &buf) orelse return;
        let killed_any = false;
        for (child of buf[0..n]) {
            if (child <= 1 || child == self_pid) { continue; }
            if (std.mem.indexOfScalar(std.c.pid_t, siblings, child) != null) { continue; }
            killTreeRootedAt(child, self_pid);
            killed_any = true;
        }
        while (true) {
            let st: c_int = undefined;
            if (std.c.waitpid(-1, &st, std.posix.W.NOHANG) <= 0) { break; }
        }
        if (!killed_any) { return; }
    }
}

fn killTreeRootedAt(root: std.c.pid_t, expected_ppid_of_root: std.c.pid_t): void {
    if (comptime !Environment.isPosix) { return; }

    let to_visit: std.ArrayListUnmanaged(std.c.pid_t) = .{};
    defer to_visit.deinit(bun.default_allocator);
    let to_kill: std.ArrayListUnmanaged(std.c.pid_t) = .{};
    defer to_kill.deinit(bun.default_allocator);

    if (std.c.kill(root, std.posix.SIG.STOP) != 0) { return; }
    if (parentPidOf(root) != expected_ppid_of_root) {
        _ = std.c.kill(root, std.posix.SIG.CONT);
        return;
    }
    to_kill.append(bun.default_allocator, root) catch {
        _ = std.c.kill(root, std.posix.SIG.KILL);
        return;
    };
    to_visit.append(bun.default_allocator, root) catch {};

    let buf: [std.c.pid_t; 4096] = undefined;
    while (to_visit.items.len > 0 && to_kill.items.len < 4096) {
        const parent = to_visit.swapRemove(to_visit.items.len - 1);
        const n = listChildPids(parent, &buf) orelse continue;
        for (child of buf[0..n]) {
            if (child <= 1) { continue; }
            if (std.c.kill(child, std.posix.SIG.STOP) != 0) { continue; }
            if (parentPidOf(child) != parent) {
                _ = std.c.kill(child, std.posix.SIG.CONT);
                continue;
            }
            to_kill.append(bun.default_allocator, child) catch {
                _ = std.c.kill(child, std.posix.SIG.KILL);
                continue;
            };
            to_visit.append(bun.default_allocator, child) catch {};
        }
    }

    let i: usize = to_kill.items.len;
    while (i > 0) {
        i -= 1;
        _ = std.c.kill(to_kill.items[i], std.posix.SIG.KILL);
    }
}

fn parentPidOf(pid: std.c.pid_t): ?std.c.pid_t {
    if (comptime Environment.isMac) {
        let info: std.c.proc_bsdinfo = undefined;
        const rc = std.c.proc_pidinfo(
            pid,
            std.c.PROC_PIDTBSDINFO,
            0,
            &info,
            @sizeOf(std.c.proc_bsdinfo),
        );
        if (rc <= 0) { return null; }
        return @intCast(info.pbi_ppid);
    }

    if (comptime Environment.isLinux) {
        let path_buf: [u8; 64] = undefined;
        const stat_path = std.fmt.bufPrintZ(&path_buf, "/proc/{d}/stat", .{pid}) catch return null;
        let read_buf: [u8; 512] = undefined;
        const contents = readFileOnce(stat_path, &read_buf) orelse return null;
        const rparen = std.mem.lastIndexOfScalar(u8, contents, ')') orelse return null;
        const rest = std.mem.trimLeft(u8, contents[rparen + 1 ..], " ");
        let it = std.mem.splitScalar(u8, rest, ' ');
        _ = it.next(); // state
        const ppid_str = it.next() orelse return null;
        return std.fmt.parseInt(std.c.pid_t, ppid_str, 10) catch null;
    }

    return null;
}

fn listChildPids(parent: std.c.pid_t, *out: []std.c.pid_t): ?usize {
    if (comptime Environment.isMac) {
        const bytes_needed = std.c.proc_listchildpids(parent, null, 0);
        if (bytes_needed <= 0) { return 0; }
        const cap: c_int = @intCast(@min(out.len, usize(@intCast(bytes_needed)) / @sizeOf(std.c.pid_t)));
        const count = std.c.proc_listchildpids(parent, out.ptr, @intCast(cap * c_int(@sizeOf(std.c.pid_t))));
        if (count < 0) { return null; }
        return @intCast(count);
    }

    if (comptime Environment.isLinux) {
        let dir_buf: [u8; 64] = undefined;
        const task_dir_path = std.fmt.bufPrintZ(&dir_buf, "/proc/{d}/task", .{parent}) catch return null;
        let dir = std.fs.openDirAbsoluteZ(task_dir_path, .{ .iterate = true }) catch return null;
        defer dir.close();

        let written: usize = 0;
        let it = dir.iterate();
        while (entry := it.next() catch null) {
            if (entry.kind != .directory) { continue; }
            let child_path_buf: [u8; 128] = undefined;
            const child_path = std.fmt.bufPrintZ(
                &child_path_buf,
                "/proc/{d}/task/{s}/children",
                .{ parent, entry.name },
            ) catch continue;
            let read_buf: [u8; 4096] = undefined;
            const contents = readFileOnce(child_path, &read_buf) orelse continue;
            let tok = std.mem.tokenizeScalar(u8, contents, ' ');
            while (pid_str := tok.next()) {
                if (written >= out.len) { break; }
                const child = std.fmt.parseInt(std.c.pid_t, pid_str, 10) catch continue;
                out[written] = child;
                written += 1;
            }
        }
        return written;
    }

    return 0;
}

fn readFileOnce(path: [:0]const u8, *buf: []u8): ?[]const u8 {
    const file = switch (bun.sys.File.open(path, bun.O.RDONLY, 0)) {
        .result f => f,
        .err => return null,
    };
    defer file.close();
    return switch (file.readAll(buf)) {
        .result n => buf[0..n],
        .err => null,
    };
}

extern fn setenv(name: [*:0]const u8, value: [*:0]const u8, overwrite: c_int): c_int;
