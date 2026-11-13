import std from "std";
import bun from "bun";
import color from "../css/values/color.zig";

const HSL = color.HSL;
const LAB = color.LAB;
const RGBA = color.RGBA;
const SRGB = color.SRGB;
const css = bun.css;
const jsc = bun.jsc;

enum OutputColorFormat {
    ansi,
    ansi_16,
    ansi_16m,
    ansi_256,
    css,
    hex,
    HEX,
    hsl,
    lab,
    number,
    rgb,
    rgba,
    @"[rgb]",
    @"[rgba]",
    @"{rgb}",
    @"{rgba}",
}

const OutputColorFormatMap = bun.ComptimeStringMap(OutputColorFormat, [
    ["[r,g,b,a]", OutputColorFormat.@"[rgba]"],
    ["[rgb]", OutputColorFormat.@"[rgb]"],
    ["[rgba]", OutputColorFormat.@"[rgba]"],
    ["{r,g,b}", OutputColorFormat.@"{rgb}"],
    ["{rgb}", OutputColorFormat.@"{rgb}"],
    ["{rgba}", OutputColorFormat.@"{rgba}"],
    ["ansi_256", OutputColorFormat.ansi_256],
    ["ansi-256", OutputColorFormat.ansi_256],
    ["ansi_16", OutputColorFormat.ansi_16],
    ["ansi-16", OutputColorFormat.ansi_16],
    ["ansi_16m", OutputColorFormat.ansi_16m],
    ["ansi-16m", OutputColorFormat.ansi_16m],
    ["ansi-24bit", OutputColorFormat.ansi_16m],
    ["ansi-truecolor", OutputColorFormat.ansi_16m],
    ["ansi", OutputColorFormat.ansi],
    ["ansi256", OutputColorFormat.ansi_256],
    ["css", OutputColorFormat.css],
    ["hex", OutputColorFormat.hex],
    ["HEX", OutputColorFormat.HEX],
    ["hsl", OutputColorFormat.hsl],
    ["lab", OutputColorFormat.lab],
    ["number", OutputColorFormat.number],
    ["rgb", OutputColorFormat.rgb],
    ["rgba", OutputColorFormat.rgba],
]);

@packed struct PackedColor {
    blue: u8,
    green: u8,
    red: u8,
    alpha: u8,
}

fn colorIntFromJS(globalThis: *jsc.JSGlobalObject, input: jsc.JSValue, @comptime property: []const u8): i32 ! bun.JSError {
    if (input == .zero || input.isUndefined() || !input.isNumber()) {
        return globalThis.throwInvalidArgumentType("color", property, "integer");
    }

    return std.math.clamp(input.coerce(i32, globalThis)?, 0, 255);
}

const ansi256_q2c: [6]u32 = [0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff];

const ansi256_table: [256]u8 = [
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
    0,  4,  4,  4,  12, 12, 2,  6,  4,  4,  12, 12, 2,  2,  6,  4,
    12, 12, 2,  2,  2,  6,  12, 12, 10, 10, 10, 10, 14, 12, 10, 10,
    10, 10, 10, 14, 1,  5,  4,  4,  12, 12, 3,  8,  4,  4,  12, 12,
    2,  2,  6,  4,  12, 12, 2,  2,  2,  6,  12, 12, 10, 10, 10, 10,
    14, 12, 10, 10, 10, 10, 10, 14, 1,  1,  5,  4,  12, 12, 1,  1,
    5,  4,  12, 12, 3,  3,  8,  4,  12, 12, 2,  2,  2,  6,  12, 12,
    10, 10, 10, 10, 14, 12, 10, 10, 10, 10, 10, 14, 1,  1,  1,  5,
    12, 12, 1,  1,  1,  5,  12, 12, 1,  1,  1,  5,  12, 12, 3,  3,
    3,  7,  12, 12, 10, 10, 10, 10, 14, 12, 10, 10, 10, 10, 10, 14,
    9,  9,  9,  9,  13, 12, 9,  9,  9,  9,  13, 12, 9,  9,  9,  9,
    13, 12, 9,  9,  9,  9,  13, 12, 11, 11, 11, 11, 7,  12, 10, 10,
    10, 10, 10, 14, 9,  9,  9,  9,  9,  13, 9,  9,  9,  9,  9,  13,
    9,  9,  9,  9,  9,  13, 9,  9,  9,  9,  9,  13, 9,  9,  9,  9,
    9,  13, 11, 11, 11, 11, 11, 15, 0,  0,  0,  0,  0,  0,  8,  8,
    8,  8,  8,  8,  7,  7,  7,  7,  7,  7,  15, 15, 15, 15, 15, 15,
];

pub type Ansi256Buffer = [24]u8;

pub struct Ansi256 {
    pub const Buffer = Ansi256Buffer;

    static fn sqdist(R: u32, G: u32, B: u32, r: u32, g: u32, b: u32): u32 {
        return ((R -% r) *% (R -% r) +% (G -% g) *% (G -% g) +% (B -% b) *% (B -% b));
    }

    static fn to6Cube(v: u32): u32 {
        if (v < 48) return 0;
        if (v < 114) return 1;
        return (v - 35) / 40;
    }

    static fn get(r: u32, g: u32, b: u32): u32 {
        const qr = Ansi256.to6Cube(r);
        const cr = ansi256_q2c[qr as usize];
        const qg = Ansi256.to6Cube(g);
        const cg = ansi256_q2c[qg as usize];
        const qb = Ansi256.to6Cube(b);
        const cb = ansi256_q2c[qb as usize];

        if (cr == r && cg == g && cb == b) {
            return 16 +% (36 *% qr) +% (6 *% qg) +% qb;
        }

        const grey_avg = (r +% g +% b) / 3;
        const grey_idx = if (grey_avg > 238) 23 else (grey_avg -% 3) / 10;
        const grey = 8 +% (10 *% grey_idx);

        const d = Ansi256.sqdist(cr, cg, cb, r, g, b);
        const idx = if (Ansi256.sqdist(grey, grey, grey, r, g, b) < d) 232 +% grey_idx else 16 +% (36 *% qr) +% (6 *% qg) +% qb;
        return idx;
    }

    pub static fn get16(r: u32, g: u32, b: u32): u8 {
        const val = Ansi256.get(r, g, b);
        return ansi256_table[val & 0xff];
    }

    pub static fn from(rgba: RGBA, buf: *Ansi256Buffer): []u8 {
        const val = Ansi256.get(rgba.red, rgba.green, rgba.blue);
        buf[0] = 0x1b;
        buf[1] = '[';
        buf[2] = '3';
        buf[3] = '8';
        buf[4] = ';';
        buf[5] = '5';
        buf[6] = ';';
        const extra = std.fmt.bufPrint(buf[7..], "{d}m", [val]) catch unreachable;
        return buf[0 .. 7 + extra.len];
    }
}

pub fn jsFunctionColor(globalThis: *jsc.JSGlobalObject, callFrame: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    const args = callFrame.argumentsAsArray(2);
    if (args[0].isUndefined()) {
        return globalThis.throwInvalidArgumentType("color", "input", "string, number, or object");
    }

    let arena_alloc: std.heap.ArenaAllocator = std.heap.ArenaAllocator.init(bun.default_allocator);
    defer arena_alloc.deinit();
    let stack_fallback = std.heap.stackFallback(4096, arena_alloc.allocator());
    const allocator = stack_fallback.get();

    let log: bun.logger.Log = bun.logger.Log.init(allocator);
    defer log.deinit();

    const unresolved_format: OutputColorFormat = brk: {
        if (!args[1].isEmptyOrUndefinedOrNull()) {
            if (!args[1].isString()) {
                return globalThis.throwInvalidArgumentType("color", "format", "string");
            }

            break :brk args[1].toEnumFromMap(globalThis, "format", OutputColorFormat, OutputColorFormatMap)?;
        }

        break :brk OutputColorFormat.css;
    };
    let input: jsc.ZigString.Slice = jsc.ZigString.Slice.empty;
    defer input.deinit();

    let parsed_color: css.CssColor.ParseResult = brk: {
        if (args[0].isNumber()) {
            const number: i64 = args[0].toInt64();
            const int_val: u32 = @abs(@mod(number, std.math.maxInt(u32))) as u32;
            const rgba: PackedColor = @bitCast(int_val);

            break :brk { result: css.CssColor{ rgba: { alpha: rgba.alpha, red: rgba.red, green: rgba.green, blue: rgba.blue } } };
        } else if (args[0].jsType().isArrayLike()) {
            switch (args[0].getLength(globalThis)?) {
                3 => {
                    const r = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 0)?, "[0]")?;
                    const g = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 1)?, "[1]")?;
                    const b = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 2)?, "[2]")?;
                    break :brk { result: css.CssColor{ rgba: { alpha: 255, red: r as u8, green: g as u8, blue: b as u8 } } };
                },
                4 => {
                    const r = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 0)?, "[0]")?;
                    const g = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 1)?, "[1]")?;
                    const b = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 2)?, "[2]")?;
                    const a = colorIntFromJS(globalThis, args[0].getIndex(globalThis, 3)?, "[3]")?;
                    break :brk { result: css.CssColor{ rgba: { alpha: a as u8, red: r as u8, green: g as u8, blue: b as u8 } } };
                },
                else => {
                    return globalThis.throw("Expected array length 3 or 4", []);
                },
            }
        } else if (args[0].isObject()) {
            const r_val = args[0].get(globalThis, "r")? ?? jsc.JSValue.zero;
            const g_val = args[0].get(globalThis, "g")? ?? jsc.JSValue.zero;
            const b_val = args[0].get(globalThis, "b")? ?? jsc.JSValue.zero;
            const r = colorIntFromJS(globalThis, r_val, "r")?;
            const g = colorIntFromJS(globalThis, g_val, "g")?;
            const b = colorIntFromJS(globalThis, b_val, "b")?;

            let a: u8? = null;
            if (a_value := args[0].getTruthy(globalThis, "a")?) {
                if (a_value.isNumber()) {
                    a = @mod(@as(i64, bun.intFromFloat(i64, a_value.asNumber() * 255.0)), 256) as u8;
                }
            }
            if (globalThis.hasException()) {
                return jsc.JSValue.zero;
            }

            break :brk {
                result: css.CssColor{
                    rgba: {
                        alpha: if (a != null) a.? else 255,
                        red: r as u8,
                        green: g as u8,
                        blue: b as u8,
                    },
                },
            };
        }

        input = args[0].toSlice(globalThis, bun.default_allocator)?;

        let parser_input: css.ParserInput = css.ParserInput.new(allocator, input.slice());
        let parser: css.Parser = css.Parser.new(&parser_input, null, {}, null);
        break :brk css.CssColor.parse(&parser);
    };

    switch (parsed_color) {
        .err => |e| {
            if (log.msgs.items.len == 0) {
                return jsc.JSValue.null;
            }

            return globalThis.throw("color() failed to parse {s}", [@tagName(e.basic().kind)]);
        },
        .result => |*result| {
            const format: OutputColorFormat = if (unresolved_format == .ansi) switch (bun.Output.Source.colorDepth()) {
                .none => return jsc.JSValue.jsEmptyString(globalThis),
                .@"16" => .ansi_16,
                .@"16m" => .ansi_16m,
                .@"256" => .ansi_256,
            } else unresolved_format;

            formatted: {
                let str = color: {
                    switch (format) {
                        .ansi => unreachable,
                        .css => break :formatted,
                        .number | .rgb | .rgba | .hex | .HEX | .ansi_16 | .ansi_16m | .ansi_256 | .@"{rgba}" | .@"{rgb}" | .@"[rgba]" | .@"[rgb]" => |tag| {
                            const srgba = unsafe {
                                switch (result.*) {
                                    .float => |float_ptr| switch (float_ptr.*) {
                                        .rgb => |rgb_val| rgb_val,
                                        inline else => |*val| val.into(.SRGB),
                                    },
                                    .rgba => |*rgba_val| rgba_val.into(.SRGB),
                                    .lab => |lab_ptr| switch (lab_ptr.*) {
                                        inline else => |entry| entry.into(.SRGB),
                                    },
                                    else => break :formatted,
                                }
                            };
                            const rgba = srgba.into(.RGBA);
                            switch (tag) {
                                .@"{rgba}" => {
                                    const object = jsc.JSValue.createEmptyObject(globalThis, 4);
                                    object.put(globalThis, "r", jsc.JSValue.jsNumber(rgba.red));
                                    object.put(globalThis, "g", jsc.JSValue.jsNumber(rgba.green));
                                    object.put(globalThis, "b", jsc.JSValue.jsNumber(rgba.blue));
                                    object.put(globalThis, "a", jsc.JSValue.jsNumber(rgba.alphaF32()));
                                    return object;
                                },
                                .@"{rgb}" => {
                                    const object = jsc.JSValue.createEmptyObject(globalThis, 3);
                                    object.put(globalThis, "r", jsc.JSValue.jsNumber(rgba.red));
                                    object.put(globalThis, "g", jsc.JSValue.jsNumber(rgba.green));
                                    object.put(globalThis, "b", jsc.JSValue.jsNumber(rgba.blue));
                                    return object;
                                },
                                .@"[rgb]" => {
                                    const object = jsc.JSValue.createEmptyArray(globalThis, 3)?;
                                    object.putIndex(globalThis, 0, jsc.JSValue.jsNumber(rgba.red))?;
                                    object.putIndex(globalThis, 1, jsc.JSValue.jsNumber(rgba.green))?;
                                    object.putIndex(globalThis, 2, jsc.JSValue.jsNumber(rgba.blue))?;
                                    return object;
                                },
                                .@"[rgba]" => {
                                    const object = jsc.JSValue.createEmptyArray(globalThis, 4)?;
                                    object.putIndex(globalThis, 0, jsc.JSValue.jsNumber(rgba.red))?;
                                    object.putIndex(globalThis, 1, jsc.JSValue.jsNumber(rgba.green))?;
                                    object.putIndex(globalThis, 2, jsc.JSValue.jsNumber(rgba.blue))?;
                                    object.putIndex(globalThis, 3, jsc.JSValue.jsNumber(rgba.alpha))?;
                                    return object;
                                },
                                .number => {
                                    let int_res: u32 = 0;
                                    int_res |= @as(u32, rgba.red) << 16;
                                    int_res |= @as(u32, rgba.green) << 8;
                                    int_res |= @as(u32, rgba.blue);
                                    return jsc.JSValue.jsNumber(int_res);
                                },
                                .hex => {
                                    break :color bun.String.createFormat("#{f}{f}{f}", [bun.fmt.hexIntLower(rgba.red), bun.fmt.hexIntLower(rgba.green), bun.fmt.hexIntLower(rgba.blue)]);
                                },
                                .HEX => {
                                    break :color bun.String.createFormat("#{f}{f}{f}", [bun.fmt.hexIntUpper(rgba.red), bun.fmt.hexIntUpper(rgba.green), bun.fmt.hexIntUpper(rgba.blue)]);
                                },
                                .rgb => {
                                    break :color bun.String.createFormat("rgb({d}, {d}, {d})", [rgba.red, rgba.green, rgba.blue]);
                                },
                                .rgba => {
                                    break :color bun.String.createFormat("rgba({d}, {d}, {d}, {d})", [rgba.red, rgba.green, rgba.blue, rgba.alphaF32()]);
                                },
                                .ansi_16 => {
                                    const ansi_16_color = Ansi256.get16(rgba.red, rgba.green, rgba.blue);
                                    const seq: [9]u8 = [0x1b, '[', '3', '8', ';', '5', ';', ansi_16_color, 'm'];
                                    break :color bun.String.cloneLatin1(seq[0..]);
                                },
                                .ansi_16m => {
                                    let buf: [48]u8 = undefined;
                                    buf[0] = 0x1b;
                                    buf[1] = '[';
                                    buf[2] = '3';
                                    buf[3] = '8';
                                    buf[4] = ';';
                                    buf[5] = '2';
                                    buf[6] = ';';
                                    const additional = std.fmt.bufPrint(buf[7..], "{d};{d};{d}m", [rgba.red, rgba.green, rgba.blue]) catch unreachable;
                                    break :color bun.String.cloneLatin1(buf[0 .. 7 + additional.len]);
                                },
                                .ansi_256 => {
                                    let buf: Ansi256.Buffer = undefined;
                                    const val = Ansi256.from(rgba, &buf);
                                    break :color bun.String.cloneLatin1(val);
                                },
                                else => unreachable,
                            }
                        },
                        .hsl => {
                            const hsl = unsafe {
                                switch (result.*) {
                                    .float => |float_ptr| switch (float_ptr.*) {
                                        .hsl => |hsl_val| hsl_val,
                                        inline else => |*val| val.into(.HSL),
                                    },
                                    .rgba => |*rgba_val| rgba_val.into(.HSL),
                                    .lab => |lab_ptr| switch (lab_ptr.*) {
                                        inline else => |entry| entry.into(.HSL),
                                    },
                                    else => break :formatted,
                                }
                            };

                            break :color bun.String.createFormat("hsl({d}, {d}, {d})", [hsl.h, hsl.s, hsl.l]);
                        },
                        .lab => {
                            const lab = unsafe {
                                switch (result.*) {
                                    .float => |float_ptr| switch (float_ptr.*) {
                                        inline else => |*val| val.into(.LAB),
                                    },
                                    .lab => |lab_ptr| switch (lab_ptr.*) {
                                        .lab => |lab_| lab_,
                                        inline else => |entry| entry.into(.LAB),
                                    },
                                    .rgba => |*rgba_val| rgba_val.into(.LAB),
                                    else => break :formatted,
                                }
                            };

                            break :color bun.String.createFormat("lab({d}, {d}, {d})", [lab.l, lab.a, lab.b]);
                        },
                    }
                } catch e => bun.handleOom(e);

                return str.transferToJS(globalThis);
            }

            let dest: std.Io.Writer.Allocating = std.Io.Writer.Allocating.init(allocator);
            const writer = &dest.writer;

            const symbols = bun.ast.Symbol.Map{};
            let printer: css.Printer = css.Printer.new(
                allocator,
                std.array_list.Managed(u8).init(allocator),
                writer,
                css.PrinterOptions.default(),
                null,
                null,
                &symbols,
            );

            result.toCss(&printer) catch e => {
                return globalThis.throw("color() internal error: {s}", [@errorName(e)]);
            };

            return bun.String.createUTF8ForJS(globalThis, dest.written());
        },
    }
}
