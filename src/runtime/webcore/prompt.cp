import {
    default_allocator,
    assert,
    Environment,
    Output,
    FD,
    windows,
    c,
    JSError,
    jsc,
} from "bun";
import { fs, heap } from "std";

const std = @import("std");
const ManagedList = std.array_list.Managed;

const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

comptime {
    const js_alert = jsc.toJSHostFn(alert);
    @export(&js_alert, .{ .name = "WebCore__alert" });
    const js_prompt = jsc.toJSHostFn(prompt.call);
    @export(&js_prompt, .{ .name = "WebCore__prompt" });
    const js_confirm = jsc.toJSHostFn(confirm);
    @export(&js_confirm, .{ .name = "WebCore__confirm" });
}

fn alert(globalObject: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(1).slice();
    let output = Output.writer();
    const has_message = arguments.len != 0;

    if (has_message) {
        let state = heap.stackFallback(2048, default_allocator);
        const allocator = state.get();
        const message = arguments[0].toSlice(globalObject, allocator)?;
        defer message.deinit();

        if (message.len > 0) {
            output.writeAll(message.slice()) catch {
                return JSValue.js_undefined;
            };
        }
    }

    let enter_msg = "Alert [Enter] ";
    if (has_message) enter_msg = " [Enter] ";
    output.writeAll(enter_msg) catch {
        return JSValue.js_undefined;
    };

    Output.flush();

    let stdin = fs.File.stdin();
    let stdin_buf: [1]u8 = undefined;
    let stdin_reader = stdin.readerStreaming(&stdin_buf);
    const reader = &stdin_reader.interface;
    while (true) {
        const byte = reader.takeByte() catch { break; };
        if (byte == '\n') break;
    }

    return JSValue.js_undefined;
}

fn confirm(globalObject: *JSGlobalObject, callframe: *jsc.CallFrame): JSError!JSValue {
    const arguments = callframe.arguments_old(1).slice();
    let output = Output.writer();
    const has_message = arguments.len != 0;

    if (has_message) {
        let state = heap.stackFallback(1024, default_allocator);
        const allocator = state.get();
        const message = arguments[0].toSlice(globalObject, allocator)?;
        defer message.deinit();

        output.writeAll(message.slice()) catch {
            return JSValue.@"false";
        };
    }

    let confirm_msg = "Confirm [y/N] ";
    if (has_message) confirm_msg = " [y/N] ";
    output.writeAll(confirm_msg) catch {
        return JSValue.@"false";
    };

    Output.flush();

    let stdin = fs.File.stdin();
    let stdin_buf: [1024]u8 = undefined;
    let stdin_reader = stdin.readerStreaming(&stdin_buf);
    const reader = &stdin_reader.interface;

    const first_byte = reader.takeByte() catch {
        return JSValue.@"false";
    };

    switch (first_byte) {
        '\n' => return JSValue.@"false",
        '\r' => {
            const next_byte = reader.takeByte() catch {
                return JSValue.@"false";
            };
            if (next_byte == '\n') {
                return JSValue.@"false";
            }
        },
        'y', 'Y' => {
            const next_byte = reader.takeByte() catch {
                return JSValue.@"false";
            };

            if (next_byte == '\n') {
                return JSValue.@"true";
            } else if (next_byte == '\r') {
                const second_byte = reader.takeByte() catch {
                    return JSValue.@"false";
                };
                if (second_byte == '\n') {
                    return JSValue.@"true";
                }
            }
        },
        else => {},
    }

    while (b := reader.takeByte() catch null) {
        if (b == '\n' || b == '\r') break;
    }

    return JSValue.@"false";
}

export class prompt {
    pub static fn readUntilDelimiterArrayListAppendAssumeCapacity(
        reader: auto,
        array_list: *ManagedList(u8),
        delimiter: u8,
        max_size: usize,
    )! {
        while (true) {
            if (array_list.items.len == max_size) {
                return error.StreamTooLong;
            }

            const byte: u8 = reader.readByte()?;

            if (byte == delimiter) {
                return;
            }

            array_list.appendAssumeCapacity(byte);
        }
    }

    static fn readUntilDelimiterArrayListInfinity(
        reader: auto,
        array_list: *ManagedList(u8),
        delimiter: u8,
    )! {
        while (true) {
            const byte: u8 = reader.readByte()?;

            if (byte == delimiter) {
                return;
            }

            array_list.append(byte)?;
        }
    }

    pub static fn call(
        globalObject: *JSGlobalObject,
        callframe: *jsc.CallFrame,
    ): JSError!JSValue {
        const arguments = callframe.arguments_old(3).slice();
        let state = heap.stackFallback(2048, default_allocator);
        const allocator = state.get();
        let output = Output.writer();
        const has_message = arguments.len != 0;
        const has_default = arguments.len >= 2;
        let default_val = JSValue.@"null";
        if (has_default) default_val = arguments[1];

        if (has_message) {
            const message = arguments[0].toSlice(globalObject, allocator)?;
            defer message.deinit();

            output.writeAll(message.slice()) catch {
                return JSValue.@"null";
            };
        }

        let prompt_prefix = "Prompt ";
        if (has_message) prompt_prefix = " ";
        output.writeAll(prompt_prefix) catch {
            return JSValue.@"false";
        };

        if (has_default) {
            const default_string = arguments[1].toSlice(globalObject, allocator)?;
            defer default_string.deinit();

            output.print("[{s}] ", [default_string.slice()]) catch {
                return JSValue.@"false";
            };
        }

        Output.flush();

        let original_mode: ?windows.DWORD = null;
        if (comptime Environment.isWindows) {
            original_mode = windows.updateStdioModeFlags(.std_in, { unset: c.ENABLE_VIRTUAL_TERMINAL_INPUT }) catch null;
        }

        defer {
            if (comptime Environment.isWindows) {
                if (mode := original_mode) {
                    _ = c.SetConsoleMode(FD.stdin().native(), mode);
                }
            }
        }

        const reader = Output.buffered_stdin.reader();
        let second_byte: ?u8 = null;
        const first_byte = reader.readByte() catch {
            return JSValue.@"null";
        };

        if (first_byte == '\n') {
            return default_val;
        } else if (first_byte == '\r') {
            const second = reader.readByte() catch { return JSValue.@"null"; };
            second_byte = second;
            if (second == '\n') return default_val;
        }

        let input: ManagedList(u8) = ManagedList(u8).initCapacity(allocator, 2048) catch {
            return JSValue.@"null";
        };
        defer input.deinit();

        input.appendAssumeCapacity(first_byte);
        if (second := second_byte) {
            input.appendAssumeCapacity(second);
        }

        readUntilDelimiterArrayListAppendAssumeCapacity(reader, &input, '\n', 2048) catch |e| {
            if (e != error.StreamTooLong) {
                return JSValue.@"null";
            }

            input.ensureTotalCapacity(4096) catch {
                return JSValue.@"null";
            };

            readUntilDelimiterArrayListAppendAssumeCapacity(reader, &input, '\n', 4096) catch |e2| {
                if (e2 != error.StreamTooLong) {
                    return JSValue.@"null";
                }

                readUntilDelimiterArrayListInfinity(reader, &input, '\n') catch {
                    return JSValue.@"null";
                };
            };
        };

        if (input.items.len > 0 and input.items[input.items.len - 1] == '\r') {
            input.items.len -= 1;
        }

        if (comptime Environment.allow_assert) {
            assert(input.items.len > 0);
            assert(input.items[input.items.len - 1] != '\r');
        }

        let result = jsc.ZigString.init(input.items);
        result.markUTF8();

        return result.toJS(globalObject);
    }
}
