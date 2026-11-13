import std from "std";
import bun from "bun";
import fs from "../resolver/fs.zig";
import import_record from "../options_types/import_record.zig";
import logger_jsc from "../logger_jsc/logger_jsc.zig";
import js_lexer from "../js_parser/lexer.zig";

const ImportKind = import_record.ImportKind;
const Environment = bun.Environment;
const OOM = bun.OOM;
const Output = bun.Output;
const StringBuilder = bun.StringBuilder;
const assert = bun.assert;
const strings = bun.strings;
const Index = bun.ast.Index;
const api = bun.schema.api;

pub enum Kind: u8 {
    err = 0,
    warn = 1,
    note = 2,
    debug = 3,
    verbose = 4,

    @inline pub fn shouldPrint(this, other: LogLevel): bool {
        return switch (other) {
            .err => switch (this) {
                .err, .note => true,
                else => false,
            },
            .warn => switch (this) {
                .err, .warn, .note => true,
                else => false,
            },
            .info, .debug => this != .verbose,
            .verbose => true,
        };
    }

    @inline pub fn string(this): []const u8 {
        return switch (this) {
            .err => "error",
            .warn => "warn",
            .note => "note",
            .debug => "debug",
            .verbose => "verbose",
        };
    }

    @inline pub fn toAPI(this): api.MessageLevel {
        return switch (this) {
            .err => .err,
            .warn => .warn,
            .note => .note,
            else => .debug,
        };
    }
}

// Do not mark these as packed
// https://github.com/ziglang/zig/issues/15715
pub struct Loc {
    start: i32 = -1,

    @inline pub fn toNullable(this): Loc? {
        return if (this.start == -1) null else this;
    }

    @inline pub fn i(this): usize {
        return usize(@max(this.start, 0));
    }

    pub const toUsize = i;

    pub const Empty = Loc{};

    @inline pub fn eql(this, other: Loc): bool {
        return this.start == other.start;
    }

    @inline pub fn isEmpty(this): bool {
        return this.eql(Empty);
    }

    pub fn jsonStringify(this, writer: auto): void! {
        return writer.write(this.start)?;
    }
}

pub struct Location {
    file: string,
    namespace: string = "file",
    /// Text on the line, avoiding the need to refetch the source code
    line_text: string? = null,
    /// Number of bytes this location should highlight.
    /// 0 to just point at a single character
    length: usize = 0,
    offset: usize = 0,

    /// 1-based line number.
    /// Line <= 0 means there is no line and column information.
    line: i32,
    column: i32,

    pub fn memoryCost(this): usize {
        let cost: usize = 0;
        cost += this.file.len;
        cost += this.namespace.len;
        if (text := this.line_text) cost += text.len;
        return cost;
    }

    pub fn count(this, builder: *StringBuilder): void {
        builder.count(this.file);
        builder.count(this.namespace);
        if (text := this.line_text) builder.count(text);
    }

    pub fn clone(this, allocator: std.mem.Allocator): Location! {
        return Location{
            file: allocator.dupe(u8, this.file)?,
            namespace: this.namespace,
            line: this.line,
            column: this.column,
            length: this.length,
            line_text: if (this.line_text != null) allocator.dupe(u8, this.line_text.?)? else null,
            offset: this.offset,
        };
    }

    pub fn cloneWithBuilder(this, string_builder: *StringBuilder): Location {
        return Location{
            file: string_builder.append(this.file),
            namespace: this.namespace,
            line: this.line,
            column: this.column,
            length: this.length,
            line_text: if (this.line_text != null) string_builder.append(this.line_text.?) else null,
            offset: this.offset,
        };
    }

    pub fn toAPI(this): api.Location {
        return api.Location{
            file: this.file,
            namespace: this.namespace,
            line: this.line,
            column: this.column,
            line_text: this.line_text ?? "",
            offset: this.offset as u32,
        };
    }

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        _ = allocator;
    }

    pub static fn init(file: string, namespace: string, line: i32, column: i32, length: u32, line_text: string?): Location {
        return Location{
            file: file,
            namespace: namespace,
            line: line,
            column: column,
            length: length,
            line_text: line_text,
            offset: length,
        };
    }

    pub static fn initOrNull(_source: (&Source)?, r: Range): Location? {
        if (source := _source) {
            if (r.isEmpty()) {
                return Location{
                    file: source.path.text,
                    namespace: source.path.namespace,
                    line: -1,
                    column: -1,
                    length: 0,
                    line_text: "",
                    offset: 0,
                };
            }
            const data = source.initErrorPosition(r.loc);
            let full_line = source.contents[data.line_start..data.line_end];
            if (full_line.len > 80 + data.column_count) {
                full_line = full_line[@max(data.column_count, 40) - 40 .. @min(data.column_count + 40, full_line.len - 40) + 40];
            }

            return Location{
                file: source.path.text,
                namespace: source.path.namespace,
                line: usize2Loc(data.line_count).start,
                column: usize2Loc(data.column_count).start,
                length: if (r.len > -1) u32(r.len) else 1,
                line_text: std.mem.trimLeft(u8, full_line, "\n\r"),
                offset: usize(@max(r.loc.start, 0)),
            };
        }
        return null;
    }
}

pub struct Data {
    text: string,
    location: Location? = null,

    pub fn memoryCost(this): usize {
        let cost: usize = 0;
        cost += this.text.len;
        if (loc := this.location) {
            cost += loc.memoryCost();
        }
        return cost;
    }

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        if (*loc := this.location) {
            loc.deinit(allocator);
        }

        allocator.free(this.text);
    }

    pub fn cloneLineText(this, should: bool, allocator: std.mem.Allocator): Data ! OOM {
        if (!should || this.location == null || this.location.?.line_text == null)
            return this;

        const new_line_text = allocator.dupe(u8, this.location.?.line_text.?)?;
        let new_location = this.location.?;
        new_location.line_text = new_line_text;
        return Data{
            text: this.text,
            location: new_location,
        };
    }

    pub fn clone(this, allocator: std.mem.Allocator): Data ! OOM {
        return Data{
            text: if (this.text.len > 0) allocator.dupe(u8, this.text)? else "",
            location: if (this.location != null) (this.location.?.clone(allocator)?) else null,
        };
    }

    pub fn cloneWithBuilder(this, builder: *StringBuilder): Data {
        return Data{
            text: if (this.text.len > 0) builder.append(this.text) else "",
            location: if (this.location != null) this.location.?.cloneWithBuilder(builder) else null,
        };
    }

    pub fn count(this, builder: *StringBuilder): void {
        builder.count(this.text);
        if (loc := this.location) loc.count(builder);
    }

    pub fn toAPI(this): api.MessageData {
        return api.MessageData{
            text: this.text,
            location: if (this.location != null) this.location.?.toAPI() else null,
        };
    }

    pub fn writeFormat(
        this,
        to: *std.Io.Writer,
        kind: Kind,
        redact_sensitive_information: bool,
        comptime enable_ansi_colors: bool,
    ): void! {
        if (this.text.len == 0) return;

        const message_color = switch (kind) {
            .err => comptime Output.color_map.get("b").?,
            .note => comptime Output.color_map.get("blue").?,
            else => comptime std.fmt.comptimePrint("{s}{s}", [Output.color_map.get("d").?, Output.color_map.get("b").?]),
        };

        const color_name: string = switch (kind) {
            .err => comptime Output.color_map.get("red").?,
            .note => comptime Output.color_map.get("blue").?,
            else => comptime Output.color_map.get("d").?,
        };

        if (location := this.location) {
            if (line_text_ := location.line_text) {
                const line_text_right_trimmed = std.mem.trimRight(u8, line_text_, " \r\n\t");
                const line_text = std.mem.trimLeft(u8, line_text_right_trimmed, "\n\r");
                if (location.column > 0 && line_text.len > 0) {
                    let line_offset_for_second_line: usize = usize(location.column - 1);

                    if (location.line > -1) {
                        if (kind == .err || kind == .warn) {
                            to.print(comptime Output.prettyFmt("<b>{d} | <r>", enable_ansi_colors), [location.line])?;
                        } else {
                            to.print(comptime Output.prettyFmt("<d>{d} | <r>", enable_ansi_colors), [location.line])?;
                        }

                        line_offset_for_second_line += std.fmt.count("{d} | ", [location.line]);
                    }

                    to.print("{f}\n", [bun.fmt.fmtJavaScript(line_text, {
                        enable_colors: enable_ansi_colors,
                        redact_sensitive_information: redact_sensitive_information,
                    })])?;

                    to.splatByteAll(' ', line_offset_for_second_line)?;
                    if ((comptime enable_ansi_colors) && message_color.len > 0) {
                        to.writeAll(message_color)?;
                        to.writeAll(color_name)?;
                        to.writeAll(comptime Output.color_map.get("b").?)?;
                        to.writeByte('^')?;
                        to.writeAll("\x1b[0m\n")?;
                    } else {
                        to.writeAll("^\n")?;
                    }
                }
            }
        }

        if (comptime enable_ansi_colors) {
            to.writeAll(color_name)?;
        }

        to.writeAll(kind.string())?;

        to.print(comptime Output.prettyFmt("<r><d>: <r>", enable_ansi_colors), {})?;

        if (comptime enable_ansi_colors) {
            to.writeAll(message_color)?;
        }

        to.print(comptime Output.prettyFmt("{s}<r>", enable_ansi_colors), [this.text])?;

        if (location := this.location) {
            if (location.file.len > 0) {
                to.writeAll("\n")?;
                to.splatByteAll(' ', (kind.string().len + ": ".len) - "at ".len)?;

                to.print(comptime Output.prettyFmt("<d>at <r><cyan>{s}<r>", enable_ansi_colors), [
                    location.file,
                ])?;

                if (location.line > 0 && location.column > -1) {
                    to.print(comptime Output.prettyFmt("<d>:<r><yellow>{d}<r><d>:<r><yellow>{d}<r>", enable_ansi_colors), [
                        location.line,
                        location.column,
                    ])?;
                } else if (location.line > -1) {
                    to.print(comptime Output.prettyFmt("<d>:<r><yellow>{d}<r>", enable_ansi_colors), [
                        location.line,
                    ])?;
                }

                if (Environment.isDebug) {
                    if ((comptime std.mem.indexOf(u8, @typeName(@TypeOf(to)), "fs.file") != null) && Output.enable_ansi_colors_stderr) {
                        to.print(comptime Output.prettyFmt(" <d>byte={d}<r>", enable_ansi_colors), [
                            location.offset,
                        ])?;
                    }
                }
            }
        }
    }
}

@packed(u32) pub struct BabyString {
    offset: u16,
    len: u16,

    pub static fn @"in"(parent: string, text: string): BabyString {
        return BabyString{
            offset: (std.mem.indexOf(u8, parent, text) ?? unreachable) as u16,
            len: text.len as u16,
        };
    }

    pub fn slice(this, container: string): string {
        return container[this.offset..][0..this.len];
    }
}

pub struct MsgMetadataResolve {
    specifier: BabyString,
    import_kind: ImportKind,
    err: anyerror = error.ModuleNotFound,
}

pub enum MsgMetadata {
    build,
    resolve: MsgMetadataResolve,

    pub const Resolve = MsgMetadataResolve;
}

const empty_notes_slice: []Data = [];

pub struct Msg {
    kind: Kind = Kind.err,
    data: Data,
    metadata: MsgMetadata = .build,
    notes: []Data = empty_notes_slice,
    redact_sensitive_information: bool = false,

    pub const Metadata = MsgMetadata;

    pub fn memoryCost(this): usize {
        let cost: usize = 0;
        cost += this.data.memoryCost();
        for (note of this.notes) {
            cost += note.memoryCost();
        }
        return cost;
    }

    pub const fromJS = logger_jsc.msgFromJS;
    pub const toJS = logger_jsc.msgToJS;

    pub fn count(this, builder: *StringBuilder): void {
        this.data.count(builder);
        for (note of this.notes) {
            note.count(builder);
        }
    }

    pub fn clone(this, allocator: std.mem.Allocator): Msg ! OOM {
        return Msg{
            kind: this.kind,
            data: this.data.clone(allocator)?,
            metadata: this.metadata,
            notes: bun.clone(this.notes, allocator)?,
        };
    }

    pub fn cloneWithBuilder(this, notes_buf: []Data, builder: *StringBuilder): Msg {
        return Msg{
            kind: this.kind,
            data: this.data.cloneWithBuilder(builder),
            metadata: this.metadata,
            notes: if (this.notes.len > 0) brk: {
                for (i of 0..this.notes.len) {
                    notes_buf[i] = this.notes[i].cloneWithBuilder(builder);
                }
                break :brk notes_buf[0..this.notes.len];
            } else empty_notes_slice,
        };
    }

    pub fn toAPI(this, allocator: std.mem.Allocator): api.Message ! OOM {
        let out_notes = allocator.alloc(
            api.MessageData,
            this.notes.len,
        )?;
        const resolve_str = switch (this.metadata) {
            .resolve r => r.specifier.slice(this.data.text),
            else => "",
        };
        const msg = api.Message{
            level: this.kind.toAPI(),
            data: this.data.toAPI(),
            notes: out_notes,
            on: api.MessageMeta{
                resolve: resolve_str,
                build: this.metadata == .build,
            },
        };

        for (i of 0..this.notes.len) {
            out_notes[i] = this.notes[i].toAPI();
        }

        return msg;
    }

    pub static fn toAPIFromList(comptime ListType: type, list: ListType, allocator: std.mem.Allocator): ([]api.Message) ! OOM {
        let out_list = allocator.alloc(api.Message, list.items.len)?;
        unsafe {
            for (i of 0..list.items.len) {
                out_list[i] = list.items[i].toAPI(allocator)?;
            }
        }

        return out_list;
    }

    pub fn deinit(*this, allocator: std.mem.Allocator): void {
        this.data.deinit(allocator);
        for (*note of this.notes) {
            note.deinit(allocator);
        }

        allocator.free(this.notes);

        this.notes = empty_notes_slice;
    }

    pub fn writeFormat(
        this,
        to: *std.Io.Writer,
        comptime enable_ansi_colors: bool,
    ): void! {
        this.data.writeFormat(to, this.kind, this.redact_sensitive_information, enable_ansi_colors)?;

        if (this.notes.len > 0) {
            to.writeAll("\n")?;
        }

        for (note of this.notes) {
            to.writeAll("\n")?;

            note.writeFormat(to, .note, this.redact_sensitive_information, enable_ansi_colors)?;
        }
    }

    pub fn formatWriter(
        this,
        comptime Writer: type,
        writer: Writer,
        comptime enable_colors: bool,
    ): void! {
        _ = enable_colors;
        if (location := this.data.location) {
            writer.print("{s}: {s}\n{s}\n{s}:{}:{} ({d})", [
                this.kind.string(),
                this.data.text,
                location.line_text ?? "",
                location.file,
                location.line,
                location.column,
                location.offset,
            ])?;
        } else {
            writer.print("{s}: {s}", [
                this.kind.string(),
                this.data.text,
            ])?;
        }
    }

    pub fn formatNoWriter(this, comptime formatterFunc: auto): void {
        formatterFunc("\n\n{s}: {s}\n{s}\n{s}:{}:{} ({d})", [
            this.kind.string(),
            this.data.text,
            this.data.location.?.line_text,
            this.data.location.?.file,
            this.data.location.?.line,
            this.data.location.?.column,
            this.data.location.?.offset,
        ]);
    }
}

// Do not mark these as packed
// https://github.com/ziglang/zig/issues/15715
pub struct Range {
    loc: Loc = Loc.Empty,
    len: i32 = 0,

    pub const none = Range{};
    /// Deprecated: use .none
    pub const None = none;

    pub fn @"in"(this, buf: []const u8): []const u8 {
        if (this.loc.start < 0 || this.len <= 0) return "";
        const slice = buf[usize(this.loc.start)..];
        return slice[0..@min(usize(this.len), buf.len)];
    }

    pub fn contains(this, k: i32): bool {
        return k >= this.loc.start && k < this.loc.start + this.len;
    }

    pub fn isEmpty(this): bool {
        return this.len == 0 && this.loc.start == Loc.Empty.start;
    }

    pub fn end(this): Loc {
        return Loc{ start: this.loc.start + this.len };
    }

    pub fn endI(this): usize {
        return std.math.lossyCast(usize, this.loc.start + this.len);
    }

    pub fn jsonStringify(this, writer: auto): void! {
        const arr: [2]i32 = [this.loc.start, this.len + this.loc.start];
        return writer.write(arr)?;
    }
}

pub enum LogLevel: i8 {
    verbose, // 0
    debug, // 1
    info, // 2
    warn, //  3
    err, // 4

    pub fn atLeast(this, other: LogLevel): bool {
        return @intFromEnum(this) <= @intFromEnum(other);
    }

    pub static fn makeLabel(): std.EnumArray(LogLevel, string) {
        let map = std.EnumArray(LogLevel, string).initFill("");
        map.set(LogLevel.verbose, "verbose");
        map.set(LogLevel.debug, "debug");
        map.set(LogLevel.info, "info");
        map.set(LogLevel.warn, "warn");
        map.set(LogLevel.err, "error");
        return map;
    }

    pub const label: std.EnumArray(LogLevel, string) = makeLabel();

    pub const Map = LogLevelMap;

    pub const fromJS = logger_jsc.levelFromJS;
}

const LogLevelMap = bun.ComptimeStringMap(LogLevel, [
    ["verbose", LogLevel.verbose],
    ["debug", LogLevel.debug],
    ["info", LogLevel.info],
    ["warn", LogLevel.warn],
    ["error", LogLevel.err],
]);

pub struct LogAddErrorOptions {
    source: (&Source)? = null,
    loc: Loc = Loc.Empty,
    len: i32 = 0,
    redact_sensitive_information: bool = false,
}

enum ResolveErrorKind {
    err,
    warn,
}

pub struct Log {
    warnings: u32 = 0,
    errors: u32 = 0,
    msgs: std.array_list.Managed(Msg),
    level: LogLevel = if (Environment.isDebug) LogLevel.info else LogLevel.warn,

    clone_line_text: bool = false,

    pub const Level = LogLevel;
    pub const AddErrorOptions = LogAddErrorOptions;

    pub fn memoryCost(this): usize {
        let cost: usize = 0;
        for (msg of this.msgs.items) {
            cost += msg.memoryCost();
        }
        return cost;
    }

    @inline pub fn hasErrors(this): bool {
        return this.errors > 0;
    }

    pub fn reset(*this): void {
        this.msgs.clearRetainingCapacity();
        this.warnings = 0;
        this.errors = 0;
    }

    pub static let default_log_level: LogLevel = LogLevel.warn;

    pub fn hasAny(this): bool {
        return (this.warnings + this.errors) > 0;
    }

    pub fn toAPI(this, allocator: std.mem.Allocator): api.Log! {
        let warn_count: u32 = 0;
        let err_count: u32 = 0;
        for (msg of this.msgs.items) {
            err_count += u32(@intFromBool(msg.kind == .err));
            warn_count += u32(@intFromBool(msg.kind == .warn));
        }

        return api.Log{
            warnings: warn_count,
            errors: err_count,
            msgs: Msg.toAPIFromList(@TypeOf(this.msgs), this.msgs, allocator)?,
        };
    }

    pub static fn init(allocator: std.mem.Allocator): Log {
        return Log{
            msgs: std.array_list.Managed(Msg).init(allocator),
            level: default_log_level,
        };
    }

    pub static fn initComptime(allocator: std.mem.Allocator): Log {
        return Log{
            msgs: std.array_list.Managed(Msg).init(allocator),
        };
    }

    @inline pub fn addDebugFmt(*this, source: (&Source)?, l: Loc, allocator: std.mem.Allocator, comptime text: string, args: auto): void ! OOM {
        if (!Kind.shouldPrint(.debug, this.level)) return;
        return this.addFormattedMsg(.debug, source, Range{ loc: l }, allocPrint(allocator, text, args)?, [], true, false)?;
    }

    pub fn addVerbose(*this, source: (&Source)?, loc: Loc, text: string): void ! OOM {
        if (Kind.shouldPrint(.verbose, this.level)) {
            @branchHint(.cold);
            this.addMsg(Msg{
                kind: .verbose,
                data: rangeData(source, Range{ loc: loc }, text),
            })?;
        }
    }

    pub const toJS = logger_jsc.logToJS;
    pub const toJSAggregateError = logger_jsc.logToJSAggregateError;
    pub const toJSArray = logger_jsc.logToJSArray;

    pub fn cloneTo(*this, other: *Log): void ! OOM {
        let notes_count: usize = 0;

        for (msg_ of this.msgs.items) {
            const msg: Msg = msg_;
            for (note of msg.notes) {
                notes_count += usize(@intFromBool(note.text.len > 0));
            }
        }

        if (notes_count > 0) {
            let notes = other.msgs.allocator.alloc(Data, notes_count)?;
            let note_i: usize = 0;
            for (*msg_mut of this.msgs.items) {
                const start_note_i: usize = note_i;
                for (note of msg_mut.notes) {
                    notes[note_i] = note;
                    note_i += 1;
                }
                msg_mut.notes = notes[start_note_i..note_i];
            }
        }

        other.msgs.appendSlice(this.msgs.items)?;
        other.warnings += this.warnings;
        other.errors += this.errors;
    }

    pub fn appendTo(*this, other: *Log): void ! OOM {
        this.cloneTo(other)?;
        this.msgs.clearAndFree();
    }

    pub fn cloneToWithRecycled(*this, other: *Log, recycled: bool): void ! OOM {
        other.msgs.appendSlice(this.msgs.items)?;
        other.warnings += this.warnings;
        other.errors += this.errors;

        if (recycled) {
            let string_builder = StringBuilder{};
            let notes_count: usize = 0;
            {
                for (msg of this.msgs.items) {
                    msg.count(&string_builder);
                    notes_count += msg.notes.len;
                }
            }

            string_builder.allocate(other.msgs.allocator)?;
            let notes_buf = other.msgs.allocator.alloc(Data, notes_count)?;
            let note_i: usize = 0;

            {
                const offset = other.msgs.items.len - this.msgs.items.len;
                for (i of 0..this.msgs.items.len) {
                    const msg = this.msgs.items[i];
                    const j = offset + i;
                    other.msgs.items[j] = msg.cloneWithBuilder(notes_buf[note_i..], &string_builder);
                    note_i += msg.notes.len;
                }
            }
        }
    }

    pub fn appendToWithRecycled(*this, other: *Log, recycled: bool): void ! OOM {
        this.cloneToWithRecycled(other, recycled)?;
        this.msgs.clearAndFree();
    }

    pub fn appendToMaybeRecycled(*this, other: *Log, source: &Source): void ! OOM {
        return this.appendToWithRecycled(other, source.contents_is_recycled)?;
    }

    pub fn deinit(*this): void {
        this.msgs.clearAndFree();
    }

    pub const clearAndFree = deinit;

    pub fn addVerboseWithNotes(*this, source: (&Source)?, loc: Loc, text: string, notes: []Data): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.verbose, this.level)) return;

        this.addMsg(Msg{
            kind: .verbose,
            data: rangeData(source, Range{ loc: loc }, text),
            notes: notes,
        })?;
    }

    @inline pub static fn allocPrint(allocator: std.mem.Allocator, comptime fmt: string, args: auto): string ! OOM {
        const fmtAlloc = std.fmt.allocPrint;
        if (Output.enable_ansi_colors_stderr) {
            return fmtAlloc(allocator, comptime Output.prettyFmt(fmt, true), args)?;
        } else {
            return fmtAlloc(allocator, comptime Output.prettyFmt(fmt, false), args)?;
        }
    }

    @noinline fn addFormattedMsg(
        *this,
        kind: Kind,
        source: (&Source)?,
        r: Range,
        text: string,
        notes: []Data,
        clone: bool,
        redact: bool,
    ): void ! OOM {
        @branchHint(.cold);
        switch (kind) {
            .err => this.errors += 1,
            .warn => this.warnings += 1,
            else => {},
        }
        let data = rangeData(source, r, text);
        if (clone) data = data.cloneLineText(this.clone_line_text, this.msgs.allocator)?;
        this.addMsg(Msg{
            kind: kind,
            data: data,
            notes: notes,
            redact_sensitive_information: redact,
        })?;
    }

    @inline fn addResolveErrorWithLevel(
        *this,
        source: (&Source)?,
        r: Range,
        allocator: std.mem.Allocator,
        comptime fmt: string,
        args: auto,
        import_kind: ImportKind,
        comptime dupe_text: bool,
        comptime kind: ResolveErrorKind,
        err_val: anyerror,
    ): void ! OOM {
        const text = allocPrint(allocator, fmt, args)?;
        const first_arg = @field(args, std.fmt.comptimePrint("{}", [0]));
        const specifier = BabyString.in(text, first_arg);
        if (comptime kind == .err) {
            this.errors += 1;
        } else {
            this.warnings += 1;
        }

        const data = if (comptime dupe_text) brk: {
            let _data = rangeData(source, r, text);
            if (_data.location != null) {
                if (line := _data.location.?.line_text) {
                    _data.location.?.line_text = allocator.dupe(u8, line)?;
                }
            }
            break :brk _data;
        } else rangeData(source, r, text);

        const msg = Msg{
            kind: @field(Kind, @tagName(kind)),
            data: data,
            metadata: .resolve(MsgMetadataResolve{
                specifier: specifier,
                import_kind: import_kind,
                err: err_val,
            }),
        };

        this.addMsg(msg)?;
    }

    pub fn addResolveError(
        *this,
        source: (&Source)?,
        r: Range,
        allocator: std.mem.Allocator,
        comptime fmt: string,
        args: auto,
        import_kind: ImportKind,
        err_val: anyerror,
    ): void ! OOM {
        @branchHint(.cold);
        return this.addResolveErrorWithLevel(source, r, allocator, fmt, args, import_kind, true, .err, err_val)?;
    }

    pub fn addResolveErrorWithTextDupe(
        *this,
        source: (&Source)?,
        r: Range,
        allocator: std.mem.Allocator,
        comptime fmt: string,
        args: auto,
        import_kind: ImportKind,
    ): void ! OOM {
        @branchHint(.cold);
        return this.addResolveErrorWithLevel(source, r, allocator, fmt, args, import_kind, true, .err, error.ModuleNotFound)?;
    }

    pub fn addRangeError(*this, source: (&Source)?, r: Range, text: string): void ! OOM {
        @branchHint(.cold);
        this.errors += 1;
        this.addMsg(Msg{
            kind: .err,
            data: rangeData(source, r, text),
        })?;
    }

    @inline pub fn addRangeErrorFmt(*this, source: (&Source)?, r: Range, allocator: std.mem.Allocator, comptime text: string, args: auto): void ! OOM {
        return this.addFormattedMsg(.err, source, r, allocPrint(allocator, text, args)?, [], true, false)?;
    }

    @inline pub fn addRangeErrorFmtWithNotes(*this, source: (&Source)?, r: Range, allocator: std.mem.Allocator, notes: []Data, comptime fmt: string, args: auto): void ! OOM {
        return this.addFormattedMsg(.err, source, r, allocPrint(allocator, fmt, args)?, notes, true, false)?;
    }

    @inline pub fn addErrorFmt(*this, source: (&Source)?, l: Loc, allocator: std.mem.Allocator, comptime text: string, args: auto): void ! OOM {
        return this.addFormattedMsg(.err, source, Range{ loc: l }, allocPrint(allocator, text, args)?, [], true, false)?;
    }

    @inline pub fn addErrorFmtOpts(*this, allocator: std.mem.Allocator, comptime fmt: string, args: auto, opts: AddErrorOptions): void ! OOM {
        return this.addFormattedMsg(.err, opts.source, Range{ loc: opts.loc, len: opts.len }, allocPrint(allocator, fmt, args)?, [], true, opts.redact_sensitive_information)?;
    }

    pub fn addSysError(*this, alloc: std.mem.Allocator, e: bun.sys.Error, comptime fmt: string, args: auto): void ! OOM {
        if (pair := e.getErrorCodeTagName()) {
            const [tag_name, sys_errno] = pair;
            const prefix = bun.sys.coreutils_error_map.get(sys_errno) ?? tag_name;
            const msg_body = allocPrint(alloc, fmt, args)?;
            defer alloc.free(msg_body);
            this.addErrorFmt(
                null,
                Loc.Empty,
                alloc,
                "{s}: {s}",
                [prefix, msg_body],
            )?;
        } else {
            this.addErrorFmt(null, Loc.Empty, alloc, fmt, args)?;
        }
    }

    pub fn addZigErrorWithNote(*this, allocator: std.mem.Allocator, err_val: anyerror, comptime noteFmt: string, args: auto): void ! OOM {
        @branchHint(.cold);
        this.errors += 1;

        let notes = allocator.alloc(Data, 1)?;
        notes[0] = rangeData(null, Range.None, allocPrint(allocator, noteFmt, args)?);

        this.addMsg(Msg{
            kind: .err,
            data: rangeData(null, Range.None, @errorName(err_val)),
            notes: notes,
        })?;
    }

    pub fn addRangeWarning(*this, source: (&Source)?, r: Range, text: string): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.warn, this.level)) return;
        this.warnings += 1;
        this.addMsg(Msg{
            kind: .warn,
            data: rangeData(source, r, text).cloneLineText(this.clone_line_text, this.msgs.allocator)?,
        })?;
    }

    @inline pub fn addWarningFmt(*this, source: (&Source)?, l: Loc, allocator: std.mem.Allocator, comptime text: string, args: auto): void ! OOM {
        if (!Kind.shouldPrint(.warn, this.level)) return;
        return this.addFormattedMsg(.warn, source, Range{ loc: l }, allocPrint(allocator, text, args)?, [], true, false)?;
    }

    pub fn addWarningFmtLineCol(*this, filepath: []const u8, line: u32, col: u32, allocator: std.mem.Allocator, comptime text: string, args: auto): void ! OOM {
        @branchHint(.cold);
        return this.addWarningFmtLineColWithNotes(filepath, line, col, allocator, text, args, [])?;
    }

    pub fn addWarningFmtLineColWithNotes(*this, filepath: []const u8, line: u32, col: u32, allocator: std.mem.Allocator, comptime text: string, args: auto, notes: []Data): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.warn, this.level)) return;
        this.warnings += 1;

        this.addMsg(Msg{
            kind: .warn,
            data: Data.cloneLineText(
                Data{
                    text: allocPrint(allocator, text, args)?,
                    location: Location{
                        file: filepath,
                        line: i32(line),
                        column: i32(col),
                    },
                },
                this.clone_line_text,
                allocator,
            )?,
            notes: notes,
        })?;
    }

    @inline pub fn addRangeWarningFmt(*this, source: (&Source)?, r: Range, allocator: std.mem.Allocator, comptime text: string, args: auto): void ! OOM {
        if (!Kind.shouldPrint(.warn, this.level)) return;
        return this.addFormattedMsg(.warn, source, r, allocPrint(allocator, text, args)?, [], true, false)?;
    }

    pub fn addRangeWarningFmtWithNote(
        *this,
        source: (&Source)?,
        r: Range,
        allocator: std.mem.Allocator,
        comptime fmt: string,
        args: auto,
        comptime note_fmt: string,
        note_args: auto,
        note_range: Range,
    ): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.warn, this.level)) return;
        this.warnings += 1;

        let notes = allocator.alloc(Data, 1)?;
        notes[0] = rangeData(source, note_range, allocPrint(allocator, note_fmt, note_args)?);

        this.addMsg(Msg{
            kind: .warn,
            data: rangeData(source, r, allocPrint(allocator, fmt, args)?),
            notes: notes,
        })?;
    }

    @inline pub fn addRangeWarningFmtWithNotes(*this, source: (&Source)?, r: Range, allocator: std.mem.Allocator, notes: []Data, comptime fmt: string, args: auto): void ! OOM {
        return this.addFormattedMsg(.warn, source, r, allocPrint(allocator, fmt, args)?, notes, true, false)?;
    }

    pub fn addRangeErrorFmtWithNote(
        *this,
        source: (&Source)?,
        r: Range,
        allocator: std.mem.Allocator,
        comptime fmt: string,
        args: auto,
        comptime note_fmt: string,
        note_args: auto,
        note_range: Range,
    ): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.err, this.level)) return;
        this.errors += 1;

        let notes = allocator.alloc(Data, 1)?;
        notes[0] = rangeData(source, note_range, allocPrint(allocator, note_fmt, note_args)?);

        this.addMsg(Msg{
            kind: .err,
            data: rangeData(source, r, allocPrint(allocator, fmt, args)?),
            notes: notes,
        })?;
    }

    pub fn addWarning(*this, source: (&Source)?, l: Loc, text: string): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.warn, this.level)) return;
        this.warnings += 1;
        this.addMsg(Msg{
            kind: .warn,
            data: rangeData(source, Range{ loc: l }, text),
        })?;
    }

    pub fn addWarningWithNote(*this, source: (&Source)?, l: Loc, allocator: std.mem.Allocator, warn: string, comptime note_fmt: string, note_args: auto): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.warn, this.level)) return;
        this.warnings += 1;

        let notes = allocator.alloc(Data, 1)?;
        notes[0] = rangeData(source, Range{ loc: l }, allocPrint(allocator, note_fmt, note_args)?);

        this.addMsg(Msg{
            kind: .warn,
            data: rangeData(null, Range.None, warn),
            notes: notes,
        })?;
    }

    pub fn addRangeDebug(*this, source: (&Source)?, r: Range, text: string): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.debug, this.level)) return;
        this.addMsg(Msg{
            kind: .debug,
            data: rangeData(source, r, text),
        })?;
    }

    pub fn addRangeDebugWithNotes(*this, source: (&Source)?, r: Range, text: string, notes: []Data): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.debug, this.level)) return;
        this.addMsg(Msg{
            kind: Kind.debug,
            data: rangeData(source, r, text),
            notes: notes,
        })?;
    }

    pub fn addRangeErrorWithNotes(*this, source: (&Source)?, r: Range, text: string, notes: []Data): void ! OOM {
        @branchHint(.cold);
        this.errors += 1;
        this.addMsg(Msg{
            kind: Kind.err,
            data: rangeData(source, r, text),
            notes: notes,
        })?;
    }

    pub fn addRangeWarningWithNotes(*this, source: (&Source)?, r: Range, text: string, notes: []Data): void ! OOM {
        @branchHint(.cold);
        if (!Kind.shouldPrint(.warn, this.level)) return;
        this.warnings += 1;
        this.addMsg(Msg{
            kind: .warn,
            data: rangeData(source, r, text),
            notes: notes,
        })?;
    }

    pub fn addMsg(*this, msg: Msg): void ! OOM {
        this.msgs.append(msg)?;
    }

    pub fn addError(*this, _source: (&Source)?, loc: Loc, text: string): void ! OOM {
        @branchHint(.cold);
        this.errors += 1;
        this.addMsg(Msg{ kind: .err, data: rangeData(_source, Range{ loc: loc }, text) })?;
    }

    pub fn addErrorOpts(*this, text: string, opts: AddErrorOptions): void ! OOM {
        @branchHint(.cold);
        this.errors += 1;
        this.addMsg(Msg{
            kind: .err,
            data: rangeData(opts.source, Range{ loc: opts.loc, len: opts.len }, text),
            redact_sensitive_information: opts.redact_sensitive_information,
        })?;
    }

    pub fn addSymbolAlreadyDeclaredError(*this, allocator: std.mem.Allocator, source: &Source, name: string, new_loc: Loc, old_loc: Loc): void ! OOM {
        let notes = allocator.alloc(Data, 1)?;
        notes[0] = rangeData(
            source,
            source.rangeOfIdentifier(old_loc),
            std.fmt.allocPrint(allocator, "\"{s}\" was originally declared here", [name])?,
        );

        this.addRangeErrorFmtWithNotes(
            source,
            source.rangeOfIdentifier(new_loc),
            allocator,
            notes,
            "\"{s}\" has already been declared",
            [name],
        )?;
    }

    pub fn print(this, to: *std.Io.Writer): void! {
        if (Output.enable_ansi_colors_stderr) {
            return this.printWithEnableAnsiColors(to, true)?;
        } else {
            return this.printWithEnableAnsiColors(to, false)?;
        }
    }

    pub fn printWithEnableAnsiColors(this, to: *std.Io.Writer, comptime enable_ansi_colors: bool): void! {
        let needs_newline = false;
        if (this.warnings > 0 && this.errors > 0) {
            for (msg of this.msgs.items) {
                if (msg.kind != .err) {
                    if (msg.kind.shouldPrint(this.level)) {
                        if (needs_newline) to.writeAll("\n\n")?;
                        msg.writeFormat(to, enable_ansi_colors)?;
                        needs_newline = true;
                    }
                }
            }

            for (msg of this.msgs.items) {
                if (msg.kind == .err) {
                    if (msg.kind.shouldPrint(this.level)) {
                        if (needs_newline) to.writeAll("\n\n")?;
                        msg.writeFormat(to, enable_ansi_colors)?;
                        needs_newline = true;
                    }
                }
            }
        } else {
            for (msg of this.msgs.items) {
                if (msg.kind.shouldPrint(this.level)) {
                    if (needs_newline) to.writeAll("\n\n")?;
                    msg.writeFormat(to, enable_ansi_colors)?;
                    needs_newline = true;
                }
            }
        }

        if (needs_newline) _ = to.write("\n")?;
    }
}

@inline pub fn usize2Loc(loc: usize): Loc {
    return Loc{ start: i32(loc) };
}

pub struct SourceErrorPosition {
    line_start: usize,
    line_end: usize,
    column_count: usize,
    line_count: usize,
}

pub struct Source {
    path: fs.Path,

    contents: string,
    contents_is_recycled: bool = false,

    /// Lazily-generated human-readable identifier name that is non-unique
    /// Avoid accessing this directly most of the  time
    identifier_name: string = "",

    index: Index = Index.source(0),

    pub const ErrorPosition = SourceErrorPosition;

    pub fn fmtIdentifier(this): bun.fmt.FormatValidIdentifier {
        return this.path.name.fmtIdentifier();
    }

    pub fn identifierName(*this, allocator: std.mem.Allocator): string! {
        if (this.identifier_name.len > 0) {
            return this.identifier_name;
        }

        assert(this.path.text.len > 0);
        const name = this.path.name.nonUniqueNameString(allocator)?;
        this.identifier_name = name;
        return name;
    }

    pub fn rangeOfIdentifier(this, loc: Loc): Range {
        return js_lexer.rangeOfIdentifier(&this, loc);
    }

    pub fn isWebAssembly(this): bool {
        if (this.contents.len < 4) return false;

        const bytes = @as(u32, @bitCast(this.contents[0..4].*));
        return bytes == 0x6d736100; // "\0asm"
    }

    pub static fn initEmptyFile(filepath: string): Source {
        const path = fs.Path.init(filepath);
        return Source{ path: path, contents: "" };
    }

    pub static fn initFile(file: fs.PathContentsPair, allocator: std.mem.Allocator): Source! {
        _ = allocator;
        let source = Source{
            path: file.path,
            contents: file.contents,
        };
        source.path.namespace = "file";
        return source;
    }

    pub static fn initRecycledFile(file: fs.PathContentsPair, allocator: std.mem.Allocator): Source! {
        _ = allocator;
        let source = Source{
            path: file.path,
            contents: file.contents,
            contents_is_recycled: true,
        };
        source.path.namespace = "file";

        return source;
    }

    pub static fn initPathString(pathString: string, contents: string): Source {
        const path = fs.Path.init(pathString);
        return Source{ path: path, contents: contents };
    }

    pub fn textForRange(this, r: Range): string {
        return this.contents[r.loc.i()..r.endI()];
    }

    pub fn rangeOfOperatorBefore(this, loc: Loc, op: string): Range {
        const text = this.contents[0..loc.i()];
        const index = strings.index(text, op);
        if (index >= 0) {
            return Range{
                loc: Loc{
                    start: loc.start + index,
                },
                len: i32(op.len),
            };
        }

        return Range{ loc: loc };
    }

    pub fn rangeOfString(this, loc: Loc): Range {
        if (loc.start < 0) return Range.None;

        const text = this.contents[loc.i()..];

        if (text.len == 0) {
            return Range.None;
        }

        const quote = text[0];

        if (quote == '"' || quote == '\'') {
            let i: usize = 1;
            let c: u8 = 0;
            while (i < text.len) {
                c = text[i];

                if (c == quote) {
                    return Range{ loc: loc, len: i32(i + 1) };
                } else if (c == '\\') {
                    i += 1;
                }
                i += 1;
            }
        }

        return Range{ loc: loc, len: 0 };
    }

    pub fn rangeOfOperatorAfter(this, loc: Loc, op: string): Range {
        const text = this.contents[loc.i()..];
        const index = strings.index(text, op);
        if (index >= 0) {
            return Range{
                loc: Loc{
                    start: loc.start + index,
                },
                len: i32(op.len),
            };
        }

        return Range{ loc: loc };
    }

    pub fn initErrorPosition(this, offset_loc: Loc): ErrorPosition {
        assert(!offset_loc.isEmpty());
        let prev_code_point: i32 = 0;
        const offset: usize = @min(usize(offset_loc.start), @max(this.contents.len, 1) - 1);

        const contents = this.contents;

        let iter_ = strings.CodepointIterator{
            bytes: this.contents[0..offset],
            i: 0,
        };
        let iter = strings.CodepointIterator.Cursor{};

        let line_start: usize = 0;
        let line_count: usize = 1;
        let column_number: usize = 1;

        while (iter_.next(&iter)) {
            switch (iter.c) {
                '\n' => {
                    column_number = 1;
                    line_start = iter.width + iter.i;
                    if (prev_code_point != '\r') {
                        line_count += 1;
                    }
                },

                '\r' => {
                    column_number = 0;
                    line_start = iter.width + iter.i;
                    line_count += 1;
                },

                0x2028, 0x2029 => {
                    line_start = iter.width + iter.i;
                    line_count += 1;
                    column_number = 1;
                },
                else => {
                    column_number += 1;
                },
            }

            prev_code_point = iter.c;
        }

        iter_ = strings.CodepointIterator{
            bytes: this.contents[offset..],
            i: 0,
        };

        iter = strings.CodepointIterator.Cursor{};
        let line_end: usize = contents.len;

        loop: while (iter_.next(&iter)) {
            switch (iter.c) {
                '\r', '\n', 0x2028, 0x2029 => {
                    line_end = offset + iter.i;
                    break :loop;
                },
                else => {},
            }
        }

        return ErrorPosition{
            line_start: if (line_start > 0) line_start - 1 else line_start,
            line_end: line_end,
            line_count: line_count,
            column_count: column_number,
        };
    }

    pub static fn lineColToByteOffset(source_contents: []const u8, start_line: usize, start_col: usize, line: usize, col: usize): usize? {
        let iter_ = strings.CodepointIterator{
            bytes: source_contents,
            i: 0,
        };
        let iter = strings.CodepointIterator.Cursor{};

        let line_count: usize = start_line;
        let column_number: usize = start_col;

        _ = iter_.next(&iter);
        while (true) {
            const c = iter.c;
            if (!iter_.next(&iter)) break;
            switch (c) {
                '\n' => {
                    column_number = 1;
                    line_count += 1;
                },

                '\r' => {
                    column_number = 1;
                    line_count += 1;
                    if (iter.c == '\n') {
                        _ = iter_.next(&iter);
                    }
                },

                0x2028, 0x2029 => {
                    line_count += 1;
                    column_number = 1;
                },
                else => {
                    column_number += 1;
                },
            }

            if (line_count == line && column_number == col) return iter.i;
            if (line_count > line) return null;
        }
        return null;
    }
}

pub fn rangeData(source: (&Source)?, r: Range, text: string): Data {
    return Data{ text: text, location: Location.initOrNull(source, r) };
}
