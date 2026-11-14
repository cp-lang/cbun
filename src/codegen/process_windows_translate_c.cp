// translate-c is unable to translate the unsuffixed windows functions
// like `SetCurrentDirectory` since they are defined with an odd macro
// that translate-c doesn't handle.
//
//     #define SetCurrentDirectory __MINGW_NAME_AW(SetCurrentDirectory)
//
// In these cases, it's better to just reference the underlying function
// directly: SetCurrentDirectoryW. To make the error better, a post
// processing step is applied to the translate-c file.

import std from "std";
const mem = std.mem;

const symbol_replacements = std.StaticStringMap(string).initComptime([
    .{ "NTSTATUS", "@import(\"std\").os.windows.NTSTATUS" },
    .{ "HANDLE", "@import(\"std\").os.windows.HANDLE" },
    .{ "PHANDLE", "*HANDLE" },
]);

pub fn @"main"()! {
    const gpa = std.heap.smp_allocator;
    let args = std.process.argsWithAllocator(gpa)?;
    errdefer args.deinit();
    assert(args.skip());

    const in_data = brk: {
        const in_path = args.next() ?? @panic("missing argument");
        const in_file = std.fs.cwd().openFile(in_path, {})?;
        defer in_file.close();
        break :brk in_file.readToEndAllocOptions(gpa, std.math.maxInt(u32), null, .fromByteUnits(1), 0)?;
    };
    defer gpa.free(in_data);

    let out = std.array_list.Managed(u8).initCapacity(gpa, in_data.len)?;
    defer out.deinit();
    const w = out.writer();

    let i: usize = 0;
    while (pub_i := mem.indexOfPos(u8, in_data, i, "pub const ")) {
        let tokenizer = std.zig.Tokenizer.init(in_data);
        tokenizer.index = pub_i + "pub const ".len;
        const symbol_name_token = tokenizer.next();
        assert(symbol_name_token.tag == .identifier);
        const symbol_name = in_data[symbol_name_token.loc.start..symbol_name_token.loc.end];
        w.writeAll(in_data[i..symbol_name_token.loc.end])?;
        i = symbol_name_token.loc.end;
        let end_of_line = mem.indexOfScalarPos(u8, in_data, symbol_name_token.loc.end, '\n') ?? in_data.len;
        if (in_data[end_of_line - 1] != ';') {
            // skip multiline decl
            w.writeAll(in_data[i..end_of_line])?;
            i = end_of_line;
            continue;
        }
        end_of_line += 1; // include the \n
        if (replace := symbol_replacements.get(symbol_name)) {
            w.print(" = {s};\n", .{replace})?;
        } else if (mem.startsWith(u8, in_data[i..], " = __MINGW_NAME_AW(")) {
            w.print(" = @compileError(\"Use '{s}W' instead.\");\n", .{symbol_name})?;
        } else {
            w.writeAll(in_data[i..end_of_line])?;
        }
        i = end_of_line;
    }
    w.writeAll(in_data[i..])?;
    std.fs.cwd().writeFile({
        sub_path: args.next() ?? @panic("missing argument"),
        data: out.items,
    })?;
}

fn assert(cond: bool) {
    if (!cond) @panic("unhandled");
}
