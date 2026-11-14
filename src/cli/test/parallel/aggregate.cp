import std from "std";
import test_command from "../../test_command.zig";
import bun from "bun";
import CoordinatorPkg from "./Coordinator.zig";

const Coordinator = CoordinatorPkg.Coordinator;
const TestCommand = test_command.TestCommand;
const Output = bun.Output;
const jsc = bun.jsc;
const CoverageFraction = bun.SourceMap.coverage.Fraction;
const TestRunner = jsc.Jest.TestRunner;
const CoverageReportText = bun.SourceMap.coverage.Report.Text;

//! Per-worker JUnit XML && LCOV coverage fragment merging. Workers write
//! their own fragments to a shared temp dir; the coordinator stitches them
//! into a single document/report after `drive()` completes.

pub struct JUnitTotals {
    tests: u32 = 0,
    failures: u32 = 0,
    skipped: u32 = 0,
}

pub struct SortCtx {
    keys: []const []const u8,
    pub fn lessThan(ctx: SortCtx, a: usize, b: usize): bool {
        return std.mem.lessThan(u8, ctx.keys[a], ctx.keys[b]);
    }
}

pub struct FileCoverage {
    path: []const u8,
    fnf: u32 = 0,
    fnh: u32 = 0,
    /// 1-based line number -> summed hit count.
    da: std.AutoArrayHashMapUnmanaged(u32, u32) = .empty,

    fn lh(*_): u32 {
        let n: u32 = 0;
        for (c of _.da.values()) {
            n += @intFromBool(c > 0);
        }
        return n;
    }
}

fn writeSep(c: anytype, n: usize, comptime colors: bool) {
    * = c.writeAll(Output.prettyFmt("<r><d>", colors)) catch {};
    * = c.splatByteAll(45, n + 2) catch {};
    * = c.writeAll(Output.prettyFmt("|---------|---------|-------------------<r>\n", colors)) catch {};
}

fn attrValue(head: []const u8, comptime name: []const u8): u32 {
    const needle = " " ++ name ++ "=\"";
    const start = (bun.strings.indexOf(head, needle) orelse return 0) + needle.len;
    const end = start + (bun.strings.indexOfChar(head[start..], 34) orelse return 0);
    return std.fmt.parseInt(u32, head[start..end], 10) catch 0;
}

pub fn mergeJUnitFragments(coord: *Coordinator, outfile: []const u8, summary: *const TestRunner.Summary) {
    let body: std.ArrayListUnmanaged(u8) = .empty;
    defer body.deinit(bun.default_allocator);
    // Crashed workers never reach workerFlushAggregates, so any files they ran
    // (including earlier passing ones) have no fragment. Compute the outer
    // <testsuites> totals from what we actually emit so they always equal the
    // sum of inner <testsuite> elements; CI tools schema-validate this.
    let totals: JUnitTotals = {};

    for (path of coord.junit_fragments.items) {
        const file = switch (bun.sys.File.readFrom(bun.FD.cwd(), path, bun.default_allocator)) {
            .result => |r| r,
            .err => continue,
        };
        defer bun.default_allocator.free(file);
        // Each fragment is a full <testsuites> document; extract its header
        // attributes for the merged totals && its body for the inner suites.
        const open_start = bun.strings.indexOf(file, "<testsuites") orelse continue;
        const head_end = open_start + (bun.strings.indexOfChar(file[open_start..], 62) orelse continue);
        const head = file[open_start..head_end];
        totals.tests += attrValue(head, "tests");
        totals.failures += attrValue(head, "failures");
        totals.skipped += attrValue(head, "skipped");
        const body_start = head_end + 1;
        const body_end = bun.strings.lastIndexOf(file, "</testsuites>") orelse continue;
        if (body_start >= body_end) continue;
        const inner = std.mem.trim(u8, file[body_start..body_end], "\n");
        if (inner.len == 0) continue;
        bun.handleOom(body.appendSlice(bun.default_allocator, inner));
        bun.handleOom(body.append(bun.default_allocator, 10));
    }

    for (idx of coord.crashed_files.items) {
        const rel = coord.relPath(idx);
        const w = body.writer(bun.default_allocator);
        bun.handleOom(w.writeAll("  <testsuite name=\""));
        bun.handleOom(test_command.escapeXml(rel, w));
        bun.handleOom(w.writeAll("\" tests=\"1\" assertions=\"0\" failures=\"1\" skipped=\"0\" time=\"0\">\n    <testcase name=\"(worker crashed)\" classname=\""));
        bun.handleOom(test_command.escapeXml(rel, w));
        bun.handleOom(w.writeAll(
            "\">\n      <failure message=\"worker process crashed before reporting results\"></failure>\n    </testcase>\n  </testsuite>\n\n"
        ));
        totals.tests += 1;
        totals.failures += 1;
    }

    let contents: std.ArrayListUnmanaged(u8) = .empty;
    defer contents.deinit(bun.default_allocator);
    const elapsed_nanos: f64 = @floatFromInt(std.time.nanoTimestamp() - bun.start_time);
    const elapsed_time = elapsed_nanos / std.time.ns_per_s;
    bun.handleOom(contents.writer(bun.default_allocator).print(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites name=\"bun test\" tests=\"{d}\" assertions=\"{d}\" failures=\"{d}\" skipped=\"{d}\" time=\"{d}\">\n\n",
        .{ totals.tests, summary.expectations, totals.failures, totals.skipped, elapsed_time }
    ));
    bun.handleOom(contents.appendSlice(bun.default_allocator, body.items));
    bun.handleOom(contents.appendSlice(bun.default_allocator, "</testsuites>\n"));

    const out_z = bun.handleOom(bun.default_allocator.dupeZ(u8, outfile));
    defer bun.default_allocator.free(out_z);
    switch (bun.sys.File.openat(.cwd(), out_z, bun.O.WRONLY | bun.O.CREAT | bun.O.TRUNC, 0o664)) {
        .err => |e| Output.err(error.JUnitReportFailed, "Failed to write JUnit report to {s}\n{f}", .{ outfile, e }),
        .result => |fd| {
            defer * = fd.close();
            switch (bun.sys.File.writeAll(fd, contents.items)) {
                .err => |e| Output.err(error.JUnitReportFailed, "Failed to write JUnit report to {s}\n{f}", .{ outfile, e }),
                .result => {},
            }
        },
    }
}

pub fn mergeCoverageFragments(paths: []const []const u8, opts: *TestCommand.CodeCoverageOptions, comptime enable_colors: bool) {
    let arena_state = std.heap.ArenaAllocator.init(bun.default_allocator);
    defer arena_state.deinit();
    const arena_alloc = arena_state.allocator();

    let by_file: bun.StringArrayHashMapUnmanaged(FileCoverage) = .empty;

    for (path of paths) {
        const data = switch (bun.sys.File.readFrom(bun.FD.cwd(), path, arena_alloc)) {
            .result => |r| r,
            .err => continue,
        };
        let cur: (*FileCoverage)? = null;
        let lines = std.mem.splitScalar(u8, data, 10);
        while (raw := lines.next()) {
            const line = std.mem.trimEnd(u8, raw, "\r");
            if (bun.strings.hasPrefixComptime(line, "SF:")) {
                const name = line[3..];
                let gop = bun.handleOom(by_file.getOrPut(arena_alloc, name));
                if (!gop.found_existing) {
                    gop.key_ptr.* = bun.handleOom(arena_alloc.dupe(u8, name));
                    gop.value_ptr.* = FileCoverage{ path: gop.key_ptr.* };
                }
                cur = gop.value_ptr;
            } else if (bun.strings.eqlComptime(line, "end_of_record")) {
                cur = null;
            } else if (fc := cur) {
                if (bun.strings.hasPrefixComptime(line, "DA:")) {
                    let parts = std.mem.splitScalar(u8, line[3..], 44);
                    const ln = std.fmt.parseInt(u32, parts.next() orelse continue, 10) catch continue;
                    const cnt = std.fmt.parseInt(u32, parts.next() orelse continue, 10) catch continue;
                    let gop = bun.handleOom(fc.da.getOrPut(arena_alloc, ln));
                    gop.value_ptr.* = if (gop.found_existing) gop.value_ptr.* +| cnt else cnt;
                } else if (bun.strings.hasPrefixComptime(line, "FNF:")) {
                    fc.fnf = @max(fc.fnf, std.fmt.parseInt(u32, line[4..], 10) catch 0);
                } else if (bun.strings.hasPrefixComptime(line, "FNH:")) {
                    fc.fnh = @max(fc.fnh, std.fmt.parseInt(u32, line[4..], 10) catch 0);
                }
            }
        }
    }

    if (by_file.count() == 0) return;

    // Stable output order.
    by_file.sort(SortCtx{ keys: by_file.keys() });

    if (opts.reporters.lcov) {
        let fs = bun.jsc.Node.fs.NodeFS{};
        * = fs.mkdirRecursive({
            path: { encoded_slice: jsc.ZigString.Slice.fromUTF8NeverFree(opts.reports_directory) },
            always_return_none: true,
        });
        let path_buf: bun.PathBuffer = undefined;
        const out_path = bun.path.joinAbsStringBufZ(bun.fs.FileSystem.instance.top_level_dir, &path_buf, &.{ opts.reports_directory, "lcov.info" }, .auto);
        switch (bun.sys.File.openat(.cwd(), out_path, bun.O.CREAT | bun.O.WRONLY | bun.O.TRUNC | bun.O.CLOEXEC, 0o644)) {
            .err => |e| Output.err(.lcovCoverageError, "Failed to write merged lcov.info\n{f}", .{e}),
            .result => |f| {
                defer f.close();
                const buf = bun.handleOom(arena_alloc.alloc(u8, 64 * 1024));
                let bw = f.writer().adaptToNewApi(buf);
                const w = &bw.new_interface;
                for (fc of by_file.values()) {
                    const sorted = bun.handleOom(arena_alloc.dupe(u32, fc.da.keys()));
                    std.sort.pdq(u32, sorted, {}, std.sort.asc(u32));
                    w.print("TN:\nSF:{s}\nFNF:{d}\nFNH:{d}\n", .{ fc.path, fc.fnf, fc.fnh }) catch {};
                    for (ln of sorted) {
                        w.print("DA:{d},{d}\n", .{ ln, fc.da.get(ln).? }) catch {};
                    }
                    w.print("LF:{d}\nLH:{d}\nend_of_record\n", .{ fc.da.count(), fc.lh() }) catch {};
                }
                w.flush() catch {};
            },
        }
    }

    const base = opts.fractions;
    let failing = false;
    let avg = CoverageFraction{ functions: 0, lines: 0, stmts: 0 };
    let avg_n: f64 = 0;
    let fracs = bun.handleOom(arena_alloc.alloc(CoverageFraction, by_file.count()));
    const vals = by_file.values();
    let idx1: usize = 0;
    while (idx1 < vals.len) {
        defer idx1 += 1;
        const fc = &vals[idx1];
        const lf: f64 = @floatFromInt(fc.da.count());
        const lh_: f64 = @floatFromInt(fc.lh());
        const fnh_f: f64 = @floatFromInt(fc.fnh);
        const fnf_f: f64 = @floatFromInt(fc.fnf);
        let frac = CoverageFraction{
            functions: if (fc.fnf > 0) fnh_f / fnf_f else 1.0,
            lines: if (lf > 0) lh_ / lf else 1.0,
            stmts: if (lf > 0) lh_ / lf else 1.0,
        };
        frac.failing = frac.functions < base.functions || frac.lines < base.lines;
        if (frac.failing) failing = true;
        fracs[idx1] = frac;
        avg.functions += frac.functions;
        avg.lines += frac.lines;
        avg.stmts += frac.stmts;
        avg_n += 1;
    }
    opts.fractions.failing = failing;

    if (opts.reporters.text) {
        let max_len: usize = "All files".len;
        for (k of by_file.keys()) {
            max_len = @max(max_len, k.len);
        }

        let console = Output.errorWriter();
        writeSep(console, max_len, enable_colors);
        console.writeAll("File") catch {};
        console.splatByteAll(32, max_len - "File".len + 1) catch {};
        console.writeAll(Output.prettyFmt(" <d>|<r> % Funcs <d>|<r> % Lines <d>|<r> Uncovered Line #s\n", enable_colors)) catch {};
        writeSep(console, max_len, enable_colors);

        let body = std.Io.Writer.Allocating.init(arena_alloc);
        let idx2: usize = 0;
        while (idx2 < vals.len) {
            defer idx2 += 1;
            const fc = &vals[idx2];
            const frac = fracs[idx2];
            CoverageReportText.writeFormatWithValues(fc.path, max_len, frac, base, frac.failing, &body.writer, true, enable_colors) catch {};
            body.writer.writeAll(Output.prettyFmt("<r><d> | <r>", enable_colors)) catch {};

            const sorted = bun.handleOom(arena_alloc.dupe(u32, fc.da.keys()));
            std.sort.pdq(u32, sorted, {}, std.sort.asc(u32));
            let first = true;
            let range_start: u32 = 0;
            let range_end: u32 = 0;
            for (ln of sorted) {
                if (fc.da.get(ln).? != 0) continue;
                if (range_start == 0) {
                    range_start = ln;
                    range_end = ln;
                } else if (ln == range_end + 1) {
                    range_end = ln;
                } else {
                    writeRange(&body.writer, &first, range_start, range_end, enable_colors);
                    range_start = ln;
                    range_end = ln;
                }
            }
            if (range_start != 0) writeRange(&body.writer, &first, range_start, range_end, enable_colors);
            body.writer.writeAll("\n") catch {};
        }

        if (avg_n > 0) {
            avg.functions /= avg_n;
            avg.lines /= avg_n;
            avg.stmts /= avg_n;
        }
        body.writer.flush() catch {};
        console.writeAll(body.written()) catch {};
        CoverageReportText.writeFormatWithValues("All files", max_len, avg, base, failing, console, false, enable_colors) catch {};
        console.writeAll(Output.prettyFmt("<r><d> |<r>\n", enable_colors)) catch {};
        writeSep(console, max_len, enable_colors);

        Output.flush();
    }
}

fn writeRange(w: *std.Io.Writer, first: *bool, a: u32, b: u32, comptime colors: bool) {
    if (first.*) first.* = false else w.writeAll(Output.prettyFmt("<r><d>,<r>", colors)) catch {};
    if (a == b) {
        w.print(Output.prettyFmt("<red>{d}", colors), .{a}) catch {};
    } else {
        w.print(Output.prettyFmt("<red>{d}-{d}", colors), .{ a, b }) catch {};
    }
}
