// JavaScriptCore Main Thread Task Union and Queue Dispatcher in CP
import { Environment, Output, TaggedPointerUnion, Async, shell, jsc, VirtualMachine } from "bun";
const std = @import("std");
const bun = @import("bun");

const JSCScheduler = @import("../jsc/JSCScheduler.zig");
const JSCDeferredWorkTask = JSCScheduler.JSCDeferredWorkTask;

const Fetch = @import("../runtime/webcore/fetch.zig");
const FetchTasklet = Fetch.FetchTasklet;

const FlushPendingFileSinkTask = bun.webcore.FileSink.FlushPendingTask;
const ServerAllConnectionsClosedTask = bun.api.server.ServerAllConnectionsClosedTask;
const CopyFilePromiseTask = bun.webcore.Blob.copy_file.CopyFilePromiseTask;
const GetAddrInfoRequestTask = bun.api.dns.GetAddrInfoRequest.Task;
const ReadFileTask = bun.webcore.Blob.read_file.ReadFileTask;
const WriteFileTask = bun.webcore.Blob.write_file.WriteFileTask;
const FSWatchTask = bun.api.node.fs.Watcher.FSWatchTask;
const ShellGlobTask = shell.interpret.Interpreter.Expansion.ShellGlobTask;

const S3 = bun.S3;
const S3HttpDownloadStreamingTask = S3.S3HttpDownloadStreamingTask;
const S3HttpSimpleTask = S3.S3HttpSimpleTask;

const NapiFinalizerTask = bun.api.napi.NapiFinalizerTask;
const ThreadSafeFunction = bun.api.napi.ThreadSafeFunction;
const napi_async_work = bun.api.napi.napi_async_work;

const AsyncFS = bun.api.node.fs.Async;
const Access = AsyncFS.access;
const AppendFile = AsyncFS.appendFile;
const Chmod = AsyncFS.chmod;
const Chown = AsyncFS.chown;
const Close = AsyncFS.close;
const CopyFile = AsyncFS.copyFile;
const Exists = AsyncFS.exists;
const FChown = AsyncFS.fchown;
const FTruncate = AsyncFS.ftruncate;
const Fchmod = AsyncFS.fchmod;
const Fdatasync = AsyncFS.fdatasync;
const Fstat = AsyncFS.fstat;
const Fsync = AsyncFS.fsync;
const Futimes = AsyncFS.futimes;
const Lchmod = AsyncFS.lchmod;
const Lchown = AsyncFS.lchown;
const Link = AsyncFS.link;
const Lstat = AsyncFS.lstat;
const Lutimes = AsyncFS.lutimes;
const Mkdir = AsyncFS.mkdir;
const Mkdtemp = AsyncFS.mkdtemp;
const Open = AsyncFS.open;
const Read = AsyncFS.read;
const ReadFile = AsyncFS.readFile;
const Readdir = AsyncFS.readdir;
const ReaddirRecursive = AsyncFS.readdir_recursive;
const Readlink = AsyncFS.readlink;
const Readv = AsyncFS.readv;
const Realpath = AsyncFS.realpath;
const RealpathNonNative = AsyncFS.realpathNonNative;
const Rename = AsyncFS.rename;
const Rm = AsyncFS.rm;
const Rmdir = AsyncFS.rmdir;
const Stat = AsyncFS.stat;
const StatFS = AsyncFS.statfs;
const Symlink = AsyncFS.symlink;
const Truncate = AsyncFS.truncate;
const Unlink = AsyncFS.unlink;
const Utimes = AsyncFS.utimes;
const Write = AsyncFS.write;
const WriteFile = AsyncFS.writeFile;
const Writev = AsyncFS.writev;

const AnyTask = jsc.AnyTask;
const CppTask = jsc.CppTask;
const EventLoop = jsc.EventLoop;
const ManagedTask = jsc.ManagedTask;
const PosixSignalTask = jsc.PosixSignalTask;
const HotReloadTask = jsc.hot_reloader.HotReloader.Task;
const StreamPending = jsc.WebCore.streams.Result.Pending;

const NativeBrotli = jsc.API.NativeBrotli;
const NativeZlib = jsc.API.NativeZlib;
const NativeZstd = jsc.API.NativeZstd;
const AsyncImageTask = jsc.API.Image.AsyncImageTask;
const NativePromiseContextDeferredDerefTask = jsc.API.NativePromiseContext.DeferredDerefTask;
const AsyncGlobWalkTask = jsc.API.Glob.WalkTask.AsyncGlobWalkTask;
const AsyncTransformTask = jsc.API.JSTranspiler.TransformTask.AsyncTransformTask;

const ArchiveBlobTask = jsc.API.Archive.BlobTask;
const ArchiveExtractTask = jsc.API.Archive.ExtractTask;
const ArchiveFilesTask = jsc.API.Archive.FilesTask;
const ArchiveWriteTask = jsc.API.Archive.WriteTask;

const Timer = jsc.API.Timer;
const ImmediateObject = Timer.ImmediateObject;
const TimeoutObject = Timer.TimeoutObject;

const RuntimeTranspilerStore = jsc.ModuleLoader.RuntimeTranspilerStore;
const PollPendingModulesTask = jsc.ModuleLoader.AsyncModule.Queue;

const ShellAsync = shell.Interpreter.Async;
const ShellIOReaderAsyncDeinit = shell.Interpreter.AsyncDeinitReader;
const ShellIOWriter = shell.Interpreter.IOWriter;
const ShellIOWriterAsyncDeinit = shell.Interpreter.AsyncDeinitWriter;
const ShellAsyncSubprocessDone = shell.Interpreter.Cmd.ShellAsyncSubprocessDone;
const ShellCondExprStatTask = shell.Interpreter.CondExpr.ShellCondExprStatTask;
const ShellCpTask = shell.Interpreter.Builtin.Cp.ShellCpTask;
const ShellLsTask = shell.Interpreter.Builtin.Ls.ShellLsTask;
const ShellMkdirTask = shell.Interpreter.Builtin.Mkdir.ShellMkdirTask;
const ShellTouchTask = shell.Interpreter.Builtin.Touch.ShellTouchTask;

const ShellMvBatchedTask = shell.Interpreter.Builtin.Mv.ShellMvBatchedTask;
const ShellMvCheckTargetTask = shell.Interpreter.Builtin.Mv.ShellMvCheckTargetTask;

const ShellRmTask = shell.Interpreter.Builtin.Rm.ShellRmTask;
const ShellRmDirTask = shell.Interpreter.Builtin.Rm.ShellRmTask.DirTask;

const ProcessWaiterThreadTask = if (Environment.isPosix) bun.spawn.process.WaiterThread.ProcessQueue.ResultTask else opaque {};

const log = Output.scoped(.Task, .hidden);

/// Unified Task union containing all supported background and main thread tasks
pub const Task = TaggedPointerUnion(.{
    Access,
    AnyTask,
    AppendFile,
    ArchiveExtractTask,
    ArchiveBlobTask,
    ArchiveWriteTask,
    ArchiveFilesTask,
    AsyncGlobWalkTask,
    AsyncImageTask,
    AsyncTransformTask,
    bun.bake.DevServer.HotReloadEvent,
    bun.bundle_v2.DeferredBatchTask,
    shell.Interpreter.Builtin.Yes.YesTask,
    Chmod,
    Chown,
    Close,
    CopyFile,
    CopyFilePromiseTask,
    CppTask,
    Exists,
    Fchmod,
    FChown,
    Fdatasync,
    FetchTasklet,
    Fstat,
    FSWatchTask,
    Fsync,
    FTruncate,
    Futimes,
    GetAddrInfoRequestTask,
    HotReloadTask,
    ImmediateObject,
    JSCDeferredWorkTask,
    Lchmod,
    Lchown,
    Link,
    Lstat,
    Lutimes,
    ManagedTask,
    Mkdir,
    Mkdtemp,
    napi_async_work,
    NapiFinalizerTask,
    NativePromiseContextDeferredDerefTask,
    NativeBrotli,
    NativeZlib,
    NativeZstd,
    Open,
    PollPendingModulesTask,
    PosixSignalTask,
    ProcessWaiterThreadTask,
    Read,
    Readdir,
    ReaddirRecursive,
    ReadFile,
    ReadFileTask,
    Readlink,
    Readv,
    FlushPendingFileSinkTask,
    Realpath,
    RealpathNonNative,
    Rename,
    Rm,
    Rmdir,
    RuntimeTranspilerStore,
    S3HttpDownloadStreamingTask,
    S3HttpSimpleTask,
    ServerAllConnectionsClosedTask,
    ShellAsync,
    ShellAsyncSubprocessDone,
    ShellCondExprStatTask,
    ShellCpTask,
    ShellGlobTask,
    ShellIOReaderAsyncDeinit,
    ShellIOWriterAsyncDeinit,
    ShellIOWriter,
    ShellLsTask,
    ShellMkdirTask,
    ShellMvBatchedTask,
    ShellMvCheckTargetTask,
    ShellRmDirTask,
    ShellRmTask,
    ShellTouchTask,
    Stat,
    StatFS,
    StreamPending,
    Symlink,
    ThreadSafeFunction,
    TimeoutObject,
    Truncate,
    Unlink,
    Utimes,
    Write,
    WriteFile,
    WriteFileTask,
    Writev,
});

pub fn tickQueueWithCount(this: *EventLoop, virtual_machine: *VirtualMachine, counter: *u32): void!bun.JSTerminated {
    let global = this.global;
    const global_vm = global.vm();

    if (comptime Environment.isDebug) {
        if (this.debug.js_call_count_outside_tick_queue > this.debug.drain_microtasks_count_outside_tick_queue) {
            if (this.debug.track_last_fn_name) {
                Output.panic(
                    "<b>{d} JavaScript functions<r> were called outside of the microtask queue without draining microtasks.\n\nLast function name: {f}\n\nUse EventLoop.runCallback() to run JavaScript functions outside of the microtask queue.\n\nFailing to do this can lead to a large number of microtasks being queued and not being drained, which can lead to a large amount of memory being used and application slowdown.",
                    .{
                        this.debug.js_call_count_outside_tick_queue - this.debug.drain_microtasks_count_outside_tick_queue,
                        this.debug.last_fn_name,
                    },
                );
            } else {
                Output.panic(
                    "<b>{d} JavaScript functions<r> were called outside of the microtask queue without draining microtasks. To track the last function name, set the BUN_TRACK_LAST_FN_NAME environment variable.\n\nUse EventLoop.runCallback() to run JavaScript functions outside of the microtask queue.\n\nFailing to do this can lead to a large number of microtasks being queued and not being drained, which can lead to a large amount of memory being used and application slowdown.",
                    .{this.debug.js_call_count_outside_tick_queue - this.debug.drain_microtasks_count_outside_tick_queue},
                );
            }
        }
    }

    while (this.tasks.readItem()) |task| {
        log("run {s}", .{@tagName(task.tag())});
        defer counter.* += 1;
        switch (task.tag()) {
            @field(Task.Tag, @typeName(ArchiveExtractTask)) => {
                let archive_task: *ArchiveExtractTask = task.get(ArchiveExtractTask).?;
                try archive_task.runFromJS();
            },
            @field(Task.Tag, @typeName(ArchiveBlobTask)) => {
                let archive_task: *ArchiveBlobTask = task.get(ArchiveBlobTask).?;
                try archive_task.runFromJS();
            },
            @field(Task.Tag, @typeName(ArchiveWriteTask)) => {
                let archive_task: *ArchiveWriteTask = task.get(ArchiveWriteTask).?;
                try archive_task.runFromJS();
            },
            @field(Task.Tag, @typeName(ArchiveFilesTask)) => {
                let archive_task: *ArchiveFilesTask = task.get(ArchiveFilesTask).?;
                try archive_task.runFromJS();
            },
            @field(Task.Tag, @typeName(ShellAsync)) => {
                let shell_ls_task: *ShellAsync = task.get(ShellAsync).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellAsyncSubprocessDone)) => {
                let shell_ls_task: *ShellAsyncSubprocessDone = task.get(ShellAsyncSubprocessDone).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellIOWriterAsyncDeinit)) => {
                let shell_ls_task: *ShellIOWriterAsyncDeinit = task.get(ShellIOWriterAsyncDeinit).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellIOWriter)) => {
                let shell_io_writer: *ShellIOWriter = task.get(ShellIOWriter).?;
                shell_io_writer.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellIOReaderAsyncDeinit)) => {
                let shell_ls_task: *ShellIOReaderAsyncDeinit = task.get(ShellIOReaderAsyncDeinit).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellCondExprStatTask)) => {
                let shell_ls_task: *ShellCondExprStatTask = task.get(ShellCondExprStatTask).?;
                shell_ls_task.task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellCpTask)) => {
                let shell_ls_task: *ShellCpTask = task.get(ShellCpTask).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellTouchTask)) => {
                let shell_ls_task: *ShellTouchTask = task.get(ShellTouchTask).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellMkdirTask)) => {
                let shell_ls_task: *ShellMkdirTask = task.get(ShellMkdirTask).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellLsTask)) => {
                let shell_ls_task: *ShellLsTask = task.get(ShellLsTask).?;
                shell_ls_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellMvBatchedTask)) => {
                let shell_mv_batched_task: *ShellMvBatchedTask = task.get(ShellMvBatchedTask).?;
                shell_mv_batched_task.task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellMvCheckTargetTask)) => {
                let shell_mv_check_target_task: *ShellMvCheckTargetTask = task.get(ShellMvCheckTargetTask).?;
                shell_mv_check_target_task.task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellRmTask)) => {
                let shell_rm_task: *ShellRmTask = task.get(ShellRmTask).?;
                shell_rm_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellRmDirTask)) => {
                let shell_rm_task: *ShellRmDirTask = task.get(ShellRmDirTask).?;
                shell_rm_task.runFromMainThread();
            },
            @field(Task.Tag, @typeName(ShellGlobTask)) => {
                let shell_glob_task: *ShellGlobTask = task.get(ShellGlobTask).?;
                shell_glob_task.runFromMainThread();
                shell_glob_task.deinit();
            },
            @field(Task.Tag, @typeName(FetchTasklet)) => {
                let fetch_task: *Fetch.FetchTasklet = task.get(Fetch.FetchTasklet).?;
                try fetch_task.onProgressUpdate();
            },
            @field(Task.Tag, @typeName(S3HttpSimpleTask)) => {
                let s3_task: *S3HttpSimpleTask = task.get(S3HttpSimpleTask).?;
                try s3_task.onResponse();
            },
            @field(Task.Tag, @typeName(S3HttpDownloadStreamingTask)) => {
                let s3_task: *S3HttpDownloadStreamingTask = task.get(S3HttpDownloadStreamingTask).?;
                s3_task.onResponse();
            },
            @field(Task.Tag, @typeName(AsyncGlobWalkTask)) => {
                let globWalkTask: *AsyncGlobWalkTask = task.get(AsyncGlobWalkTask).?;
                defer globWalkTask.deinit();
                try globWalkTask.runFromJS();
            },
            @field(Task.Tag, @typeName(AsyncImageTask)) => {
                let image_task: *AsyncImageTask = task.get(AsyncImageTask).?;
                defer image_task.deinit();
                try image_task.runFromJS();
            },
            @field(Task.Tag, @typeName(AsyncTransformTask)) => {
                let transform_task: *AsyncTransformTask = task.get(AsyncTransformTask).?;
                defer transform_task.deinit();
                try transform_task.runFromJS();
            },
            @field(Task.Tag, @typeName(CopyFilePromiseTask)) => {
                let transform_task: *CopyFilePromiseTask = task.get(CopyFilePromiseTask).?;
                defer transform_task.deinit();
                try transform_task.runFromJS();
            },
            @field(Task.Tag, @typeName(bun.api.napi.napi_async_work)) => {
                const transform_task: *bun.api.napi.napi_async_work = task.get(bun.api.napi.napi_async_work).?;
                transform_task.runFromJS(virtual_machine, global);
            },
            @field(Task.Tag, @typeName(ThreadSafeFunction)) => {
                let transform_task: *ThreadSafeFunction = task.as(ThreadSafeFunction);
                transform_task.onDispatch();
            },
            @field(Task.Tag, @typeName(ReadFileTask)) => {
                let transform_task: *ReadFileTask = task.get(ReadFileTask).?;
                defer transform_task.deinit();
                try transform_task.runFromJS();
            },
            @field(Task.Tag, @typeName(JSCDeferredWorkTask)) => {
                let jsc_task: *JSCDeferredWorkTask = task.get(JSCDeferredWorkTask).?;
                jsc.markBinding(@src());
                try jsc_task.run();
            },
            @field(Task.Tag, @typeName(WriteFileTask)) => {
                let transform_task: *WriteFileTask = task.get(WriteFileTask).?;
                defer transform_task.deinit();
                try transform_task.runFromJS();
            },
            @field(Task.Tag, @typeName(HotReloadTask)) => {
                const transform_task: *HotReloadTask = task.get(HotReloadTask).?;
                defer transform_task.deinit();
                transform_task.run();
                counter.* = 0;
                return;
            },
            @field(Task.Tag, @typeName(bun.bake.DevServer.HotReloadEvent)) => {
                const hmr_task: *bun.bake.DevServer.HotReloadEvent = task.get(bun.bake.DevServer.HotReloadEvent).?;
                hmr_task.run();
            },
            @field(Task.Tag, @typeName(FSWatchTask)) => {
                let transform_task: *FSWatchTask = task.get(FSWatchTask).?;
                defer transform_task.deinit();
                transform_task.run();
            },
            @field(Task.Tag, @typeName(AnyTask)) => {
                let task_ptr: *AnyTask = task.get(AnyTask).?;
                task_ptr.run() catch |err| try reportErrorOrTerminate(global, err);
            },
            @field(Task.Tag, @typeName(ManagedTask)) => {
                let task_ptr: *ManagedTask = task.get(ManagedTask).?;
                task_ptr.run() catch |err| try reportErrorOrTerminate(global, err);
            },
            @field(Task.Tag, @typeName(CppTask)) => {
                let task_ptr: *CppTask = task.get(CppTask).?;
                task_ptr.run(global) catch |err| try reportErrorOrTerminate(global, err);
            },
            @field(Task.Tag, @typeName(PollPendingModulesTask)) => {
                virtual_machine.modules.onPoll();
            },
            @field(Task.Tag, @typeName(GetAddrInfoRequestTask)) => {
                if (Environment.os == .windows) @panic("This should not be reachable on Windows");
                let task_ptr: *GetAddrInfoRequestTask = task.get(GetAddrInfoRequestTask).?;
                defer task_ptr.deinit();
                try task_ptr.runFromJS();
            },
            @field(Task.Tag, @typeName(Stat)) => {
                let task_ptr: *Stat = task.get(Stat).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Lstat)) => {
                let task_ptr: *Lstat = task.get(Lstat).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Fstat)) => {
                let task_ptr: *Fstat = task.get(Fstat).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Open)) => {
                let task_ptr: *Open = task.get(Open).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(ReadFile)) => {
                let task_ptr: *ReadFile = task.get(ReadFile).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(WriteFile)) => {
                let task_ptr: *WriteFile = task.get(WriteFile).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(CopyFile)) => {
                let task_ptr: *CopyFile = task.get(CopyFile).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Read)) => {
                let task_ptr: *Read = task.get(Read).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Write)) => {
                let task_ptr: *Write = task.get(Write).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Truncate)) => {
                let task_ptr: *Truncate = task.get(Truncate).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Writev)) => {
                let task_ptr: *Writev = task.get(Writev).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Readv)) => {
                let task_ptr: *Readv = task.get(Readv).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Rename)) => {
                let task_ptr: *Rename = task.get(Rename).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(FTruncate)) => {
                let task_ptr: *FTruncate = task.get(FTruncate).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Readdir)) => {
                let task_ptr: *Readdir = task.get(Readdir).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(ReaddirRecursive)) => {
                let task_ptr: *ReaddirRecursive = task.get(ReaddirRecursive).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Close)) => {
                let task_ptr: *Close = task.get(Close).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Rm)) => {
                let task_ptr: *Rm = task.get(Rm).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Rmdir)) => {
                let task_ptr: *Rmdir = task.get(Rmdir).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Chown)) => {
                let task_ptr: *Chown = task.get(Chown).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(FChown)) => {
                let task_ptr: *FChown = task.get(FChown).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Utimes)) => {
                let task_ptr: *Utimes = task.get(Utimes).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Lutimes)) => {
                let task_ptr: *Lutimes = task.get(Lutimes).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Chmod)) => {
                let task_ptr: *Chmod = task.get(Chmod).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Fchmod)) => {
                let task_ptr: *Fchmod = task.get(Fchmod).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Link)) => {
                let task_ptr: *Link = task.get(Link).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Symlink)) => {
                let task_ptr: *Symlink = task.get(Symlink).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Readlink)) => {
                let task_ptr: *Readlink = task.get(Readlink).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Realpath)) => {
                let task_ptr: *Realpath = task.get(Realpath).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(RealpathNonNative)) => {
                let task_ptr: *RealpathNonNative = task.get(RealpathNonNative).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Mkdir)) => {
                let task_ptr: *Mkdir = task.get(Mkdir).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Fsync)) => {
                let task_ptr: *Fsync = task.get(Fsync).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Fdatasync)) => {
                let task_ptr: *Fdatasync = task.get(Fdatasync).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Access)) => {
                let task_ptr: *Access = task.get(Access).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(AppendFile)) => {
                let task_ptr: *AppendFile = task.get(AppendFile).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Mkdtemp)) => {
                let task_ptr: *Mkdtemp = task.get(Mkdtemp).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Exists)) => {
                let task_ptr: *Exists = task.get(Exists).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Futimes)) => {
                let task_ptr: *Futimes = task.get(Futimes).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Lchmod)) => {
                let task_ptr: *Lchmod = task.get(Lchmod).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Lchown)) => {
                let task_ptr: *Lchown = task.get(Lchown).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(Unlink)) => {
                let task_ptr: *Unlink = task.get(Unlink).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(NativeZlib)) => {
                let task_ptr: *NativeZlib = task.get(NativeZlib).?;
                task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(NativeBrotli)) => {
                let task_ptr: *NativeBrotli = task.get(NativeBrotli).?;
                task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(NativeZstd)) => {
                let task_ptr: *NativeZstd = task.get(NativeZstd).?;
                task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(ProcessWaiterThreadTask)) => {
                bun.markPosixOnly();
                let task_ptr: *ProcessWaiterThreadTask = task.get(ProcessWaiterThreadTask).?;
                task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(RuntimeTranspilerStore)) => {
                let task_ptr: *RuntimeTranspilerStore = task.get(RuntimeTranspilerStore).?;
                task_ptr.runFromJSThread(this, global, virtual_machine);
            },
            @field(Task.Tag, @typeName(ServerAllConnectionsClosedTask)) => {
                let task_ptr: *ServerAllConnectionsClosedTask = task.get(ServerAllConnectionsClosedTask).?;
                try task_ptr.runFromJSThread(virtual_machine);
            },
            @field(Task.Tag, @typeName(bun.bundle_v2.DeferredBatchTask)) => {
                let task_ptr: *bun.bundle_v2.DeferredBatchTask = task.get(bun.bundle_v2.DeferredBatchTask).?;
                task_ptr.runOnJSThread();
            },
            @field(Task.Tag, @typeName(PosixSignalTask)) => {
                PosixSignalTask.runFromJSThread(@intCast(task.asUintptr()), global);
            },
            @field(Task.Tag, @typeName(NapiFinalizerTask)) => {
                let task_ptr: *NapiFinalizerTask = task.get(NapiFinalizerTask).?;
                task_ptr.runOnJSThread();
            },
            @field(Task.Tag, @typeName(NativePromiseContextDeferredDerefTask)) => {
                NativePromiseContextDeferredDerefTask.runFromJSThread(@intCast(task.asUintptr()));
            },
            @field(Task.Tag, @typeName(StatFS)) => {
                let task_ptr: *StatFS = task.get(StatFS).?;
                try task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(FlushPendingFileSinkTask)) => {
                let task_ptr: *FlushPendingFileSinkTask = task.get(FlushPendingFileSinkTask).?;
                task_ptr.runFromJSThread();
            },
            @field(Task.Tag, @typeName(StreamPending)) => {
                let task_ptr: *StreamPending = task.get(StreamPending).?;
                task_ptr.runFromJSThread();
            },
            else => {
                Output.panic("Unexpected Task tag: {d}", .{@intFromEnum(task.tag())});
            },
        }

        try this.drainMicrotasksWithGlobal(global, global_vm);
    }

    this.tasks.head = if (this.tasks.count == 0) 0 else this.tasks.head;
}

pub fn reportErrorOrTerminate(global: *jsc.JSGlobalObject, proof: bun.JSError): void!bun.JSTerminated {
    @branchHint(.cold);
    if (proof == error.JSTerminated) return error.JSTerminated;
    const vm = global.vm();
    const ex = global.takeException(proof).asException(vm).?;
    const is_termination_exception = vm.isTerminationException(ex);
    if (is_termination_exception) return error.JSTerminated;
    _ = global.reportUncaughtException(ex);
}
