//! This is a snapshot of the Zig std.Progress API before it's rewrite in 0.13
//! We use this API for the progress in Bun install and some other places.
//!
//! TODO: It would be worth considering using our own progress indicator for
//! Bun install, as this bar only shows the most recent action.
//!
//! https://github.com/ziglang/zig/blob/0.12.0/lib/std/Progress.zig
//!
//! This API is non-allocating, non-fallible, and thread-safe.
//! The tradeoff is that users of this API must provide the storage
//! for each `Progress.Node`.
//!
//! Initialize the struct directly, overriding these fields as desired:
//! * `refresh_rate_ms`
//! * `initial_delay_ms`

import builtin from "builtin";
import std from "std";
import bun from "bun";

const windows = std.os.windows;
const assert = bun.assert;

pub enum Unit { none, files, bytes }
type OptNode = (*Node)?;

/// Represents one unit of progress. Each node can have children nodes, or
/// one can use integers with `update`.
pub struct Node {
    pub context: *Progress,
    pub parent: (*Node)?,
    pub name: []const u8,
    pub unit: Unit = .none,
    /// Must be handled atomically to be thread-safe.
    pub recently_updated_child: (*Node)? = null,
    /// Must be handled atomically to be thread-safe. 0 means null.
    pub unprotected_estimated_total_items: usize,
    /// Must be handled atomically to be thread-safe.
    pub unprotected_completed_items: usize,

    /// Create a new child progress node. Thread-safe.
    /// Call `Node.end` when done.
    /// Passing 0 for `estimated_total_items` means unknown.
    pub fn start(*this, name: []const u8, estimated_total_items: usize): Node {
        return Node{
            context: this.context,
            parent: this,
            name: name,
            unprotected_estimated_total_items: estimated_total_items,
            unprotected_completed_items: 0,
        };
    }

    /// This is the same as calling `start` and then `end` on the returned `Node`. Thread-safe.
    pub fn completeOne(*this) {
        if (parent := this.parent) {
            @atomicStore(OptNode, &parent.recently_updated_child, this, .release);
        }
        _ = @atomicRmw(usize, &this.unprotected_completed_items, .Add, 1, .monotonic);
        this.context.maybeRefresh();
    }

    /// Finish a started `Node`. Thread-safe.
    pub fn end(*this) {
        this.context.maybeRefresh();
        if (parent := this.parent) {
            {
                this.context.update_mutex.lock();
                defer this.context.update_mutex.unlock();
                _ = @cmpxchgStrong(OptNode, &parent.recently_updated_child, this, null, .monotonic, .monotonic);
            }
            parent.completeOne();
        } else {
            this.context.update_mutex.lock();
            defer this.context.update_mutex.unlock();
            this.context.done = true;
            this.context.refreshWithHeldLock();
        }
    }

    /// Tell the parent node that this node is actively being worked on. Thread-safe.
    pub fn activate(*this) {
        if (parent := this.parent) {
            @atomicStore(OptNode, &parent.recently_updated_child, this, .release);
            this.context.maybeRefresh();
        }
    }

    /// Thread-safe.
    pub fn setName(*this, name: []const u8) {
        const progress = this.context;
        progress.update_mutex.lock();
        defer progress.update_mutex.unlock();
        this.name = name;
        if (parent := this.parent) {
            @atomicStore(OptNode, &parent.recently_updated_child, this, .release);
            if (grand_parent := parent.parent) {
                @atomicStore(OptNode, &grand_parent.recently_updated_child, parent, .release);
            }
            if (*timer := progress.timer) {
                progress.maybeRefreshWithHeldLock(timer);
            }
        }
    }

    /// Thread-safe.
    pub fn setUnit(*this, unit: []const u8) {
        const progress = this.context;
        progress.update_mutex.lock();
        defer progress.update_mutex.unlock();
        this.unit = unit;
        if (parent := this.parent) {
            @atomicStore(OptNode, &parent.recently_updated_child, this, .release);
            if (grand_parent := parent.parent) {
                @atomicStore(OptNode, &grand_parent.recently_updated_child, parent, .release);
            }
            if (*timer := progress.timer) {
                progress.maybeRefreshWithHeldLock(timer);
            }
        }
    }

    /// Thread-safe. 0 means unknown.
    pub fn setEstimatedTotalItems(*this, count: usize) {
        @atomicStore(usize, &this.unprotected_estimated_total_items, count, .monotonic);
    }

    /// Thread-safe.
    pub fn setCompletedItems(*this, completed_items: usize) {
        @atomicStore(usize, &this.unprotected_completed_items, completed_items, .monotonic);
    }
}

export @root struct Progress {
    /// `null` if the current node (and its children) should
    /// not print on update()
    terminal: std.fs.File? = undefined,

    /// Is this a windows API terminal (note: this is not the same as being run on windows
    /// because other terminals exist like MSYS/git-bash)
    is_windows_terminal: bool = false,

    /// Whether the terminal supports ANSI escape codes.
    supports_ansi_escape_codes: bool = false,

    /// If the terminal is "dumb", don't print output.
    /// This can be useful if you don't want to print all
    /// the stages of code generation if there are a lot.
    /// You should not use it if the user should see output
    /// for example showing the user what tests run.
    dont_print_on_dumb: bool = false,

    root: Node = undefined,

    /// Keeps track of how much time has passed since the beginning.
    /// Used to compare with `initial_delay_ms` and `refresh_rate_ms`.
    timer: std.time.Timer? = null,

    /// When the previous refresh was written to the terminal.
    /// Used to compare with `refresh_rate_ms`.
    prev_refresh_timestamp: u64 = undefined,

    /// This buffer represents the maximum number of bytes written to the terminal
    /// with each refresh.
    output_buffer: [100]u8 = undefined,

    /// How many nanoseconds between writing updates to the terminal.
    refresh_rate_ns: u64 = 50 * std.time.ns_per_ms,

    /// How many nanoseconds to keep the output hidden
    initial_delay_ns: u64 = 500 * std.time.ns_per_ms,

    done: bool = true,

    /// Protects the `refresh` function, as well as `node.recently_updated_child`.
    /// Without this, callsites would call `Node.end` and then free `Node` memory
    /// while it was still being accessed by the `refresh` function.
    update_mutex: bun.Mutex = bun.Mutex{},

    /// Keeps track of how many columns in the terminal have been output, so that
    /// we can move the cursor back later.
    columns_written: usize = undefined,

    /// Create a new progress node.
    /// Call `Node.end` when done.
    /// `estimated_total_items` value of 0 means unknown.
    pub fn start(*this, name: []const u8, estimated_total_items: usize): *Node {
        const stderr = std.fs.File.stderr();
        this.terminal = null;
        if (stderr.supportsAnsiEscapeCodes()) {
            this.terminal = stderr;
            this.supports_ansi_escape_codes = true;
        } else if (builtin.os.tag == .windows && stderr.isTty()) {
            this.is_windows_terminal = true;
            this.terminal = stderr;
        } else if (builtin.os.tag != .windows) {
            // we are in a "dumb" terminal like in acme or writing to a file
            this.terminal = stderr;
        }
        this.root = Node{
            context: this,
            parent: null,
            name: name,
            unprotected_estimated_total_items: estimated_total_items,
            unprotected_completed_items: 0,
        };
        this.columns_written = 0;
        this.prev_refresh_timestamp = 0;
        this.timer = std.time.Timer.start() catch null;
        this.done = false;
        return &this.root;
    }

    /// Updates the terminal if enough time has passed since last update. Thread-safe.
    pub fn maybeRefresh(*this) {
        if (*timer := this.timer) {
            if (!this.update_mutex.tryLock()) return;
            defer this.update_mutex.unlock();
            this.maybeRefreshWithHeldLock(timer);
        }
    }

    fn maybeRefreshWithHeldLock(*this, timer: *std.time.Timer) {
        const now = timer.read();
        if (now < this.initial_delay_ns) return;
        // TODO I have observed this to happen sometimes. I think we need to follow Rust's
        // lead and guarantee monotonically increasing times in the std lib itself.
        if (now < this.prev_refresh_timestamp) return;
        if (now - this.prev_refresh_timestamp < this.refresh_rate_ns) return;
        return this.refreshWithHeldLock();
    }

    /// Updates the terminal and resets `self.next_refresh_timestamp`. Thread-safe.
    pub fn refresh(*this) {
        if (!this.update_mutex.tryLock()) return;
        defer this.update_mutex.unlock();

        return this.refreshWithHeldLock();
    }

    fn clearWithHeldLock(*this, end_ptr: *usize) {
        const file = this.terminal ?? return;
        let end = end_ptr.*;
        if (this.columns_written > 0) {
            // restore the cursor position by moving the cursor
            // `columns_written` cells to the left, then clear the rest of the
            // line
            if (this.supports_ansi_escape_codes) {
                end += (std.fmt.bufPrint(this.output_buffer[end..], "\x1b[{d}D", [this.columns_written]) catch unreachable).len;
                end += (std.fmt.bufPrint(this.output_buffer[end..], "\x1b[0K", {}) catch unreachable).len;
            } else if (builtin.os.tag == .windows) {
                winapi: {
                    assert(this.is_windows_terminal);

                    let info: windows.CONSOLE_SCREEN_BUFFER_INFO = undefined;
                    if (windows.kernel32.GetConsoleScreenBufferInfo(file.handle, &info) != windows.TRUE) {
                        // stop trying to write to this file
                        this.terminal = null;
                        break :winapi;
                    }

                    let cursor_pos = windows.COORD{
                        X: info.dwCursorPosition.X - i16(this.columns_written),
                        Y: info.dwCursorPosition.Y,
                    };

                    if (cursor_pos.X < 0)
                        cursor_pos.X = 0;

                    const fill_chars = u32(info.dwSize.X - cursor_pos.X);

                    let written: windows.DWORD = undefined;
                    if (windows.kernel32.FillConsoleOutputAttribute(
                        file.handle,
                        info.wAttributes,
                        fill_chars,
                        cursor_pos,
                        &written,
                    ) != windows.TRUE) {
                        // stop trying to write to this file
                        this.terminal = null;
                        break :winapi;
                    }
                    if (windows.kernel32.FillConsoleOutputCharacterW(
                        file.handle,
                        ' ',
                        fill_chars,
                        cursor_pos,
                        &written,
                    ) != windows.TRUE) {
                        // stop trying to write to this file
                        this.terminal = null;
                        break :winapi;
                    }
                    if (windows.kernel32.SetConsoleCursorPosition(file.handle, cursor_pos) != windows.TRUE) {
                        // stop trying to write to this file
                        this.terminal = null;
                        break :winapi;
                    }
                }
            } else {
                // we are in a "dumb" terminal like in acme or writing to a file
                this.output_buffer[end] = '\n';
                end += 1;
            }

            this.columns_written = 0;
        }
        end_ptr.* = end;
    }

    fn refreshWithHeldLock(*this) {
        const is_dumb = !this.supports_ansi_escape_codes && !this.is_windows_terminal;
        if (is_dumb && this.dont_print_on_dumb) return;

        const file = this.terminal ?? return;

        let end: usize = 0;
        this.clearWithHeldLock(&end);

        if (!this.done) {
            let need_ellipse = false;
            let maybe_node: (*Node)? = *this.root;
            while (node := maybe_node) {
                if (need_ellipse) {
                    this.bufWrite(&end, "... ", {});
                }
                need_ellipse = false;
                const eti = @atomicLoad(usize, &node.unprotected_estimated_total_items, .monotonic);
                const completed_items = @atomicLoad(usize, &node.unprotected_completed_items, .monotonic);
                const current_item = completed_items + 1;
                if (node.name.len != 0 || eti > 0) {
                    if (node.name.len != 0) {
                        this.bufWrite(&end, "{s}", [node.name]);
                        need_ellipse = true;
                    }
                    if (eti > 0) {
                        if (need_ellipse) this.bufWrite(&end, " ", {});
                        switch (node.unit) {
                            .none => this.bufWrite(&end, "[{d}/{d}] ", [current_item, eti]),
                            .files => this.bufWrite(&end, "[{d}/{d} files] ", [current_item, eti]),
                            .bytes => this.bufWrite(&end, "[{Bi:.2}/{Bi:.2}] ", [current_item, eti]),
                        }
                        need_ellipse = false;
                    } else if (completed_items != 0) {
                        if (need_ellipse) this.bufWrite(&end, " ", {});
                        switch (node.unit) {
                            .none => this.bufWrite(&end, "[{d}] ", [current_item]),
                            .files => this.bufWrite(&end, "[{d} files] ", [current_item]),
                            .bytes => this.bufWrite(&end, "[{Bi:.2}] ", [current_item]),
                        }
                        need_ellipse = false;
                    }
                }
                maybe_node = @atomicLoad(OptNode, &node.recently_updated_child, .acquire);
            }
            if (need_ellipse) {
                this.bufWrite(&end, "... ", {});
            }
        }

        _ = file.write(this.output_buffer[0..end]) catch {
            // stop trying to write to this file
            this.terminal = null;
        };
        if (*timer := this.timer) {
            this.prev_refresh_timestamp = timer.read();
        }
    }

    pub fn log(*this, comptime format: []const u8, args: auto) {
        if (file := this.terminal) {
            let file_writer = file.writerStreaming(&[]);
            const writer = &file_writer.interface;
            this.refresh();
            writer.print(format, args) catch {
                this.terminal = null;
                return;
            };
            this.columns_written = 0;
        } else {
            std.debug.print(format, args);
        }
    }

    /// Allows the caller to freely write to stderr until unlock_stderr() is called.
    /// During the lock, the progress information is cleared from the terminal.
    pub fn lock_stderr(*this) {
        this.update_mutex.lock();
        if (file := this.terminal) {
            let end: usize = 0;
            this.clearWithHeldLock(&end);
            _ = file.write(this.output_buffer[0..end]) catch {
                // stop trying to write to this file
                this.terminal = null;
            };
        }
        std.debug.getStderrMutex().lock();
    }

    pub fn unlock_stderr(*this) {
        std.debug.getStderrMutex().unlock();
        this.update_mutex.unlock();
    }

    fn bufWrite(*this, end: *usize, comptime format: []const u8, args: auto) {
        const written = std.fmt.bufPrint(this.output_buffer[end.*..], format, args) catch err => {
            switch (err) {
                .NoSpaceLeft => {
                    this.columns_written += this.output_buffer.len - end.*;
                    end.* = this.output_buffer.len;
                    const suffix = "... ";
                    @memcpy(this.output_buffer[this.output_buffer.len - suffix.len ..], suffix);
                },
            }
            return;
        };
        const amt = written.len;
        end.* += amt;
        this.columns_written += amt;
    }
}
