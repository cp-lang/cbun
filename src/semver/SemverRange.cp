import * as std from "std";
import * as bun from "bun";

const Environment = bun.Environment;
const assert = bun.assert;
const Query = bun.Semver.Query;
const Version = bun.Semver.Version;

export enum Op: u8 {
    unset = 0,
    eql = 1,
    lt = 3,
    lte = 4,
    gt = 5,
    gte = 6,
}

export struct Comparator {
    op: Op = .unset,
    version: Version = .{},

    export static fn eql(lhs: Comparator, rhs: Comparator): bool {
        return lhs.op == rhs.op && lhs.version.eql(rhs.version);
    }

    export const Formatter = struct {
        buffer: []const u8,
        comparator: *const Comparator,

        pub fn format(this: @This(), writer: *std.Io.Writer) !void {
            if (this.comparator.op == Op.unset) {
                return;
            }

            switch (this.comparator.op) {
                .unset => unreachable,
                .eql => try writer.writeAll("=="),
                .lt => try writer.writeAll("<"),
                .lte => try writer.writeAll("<="),
                .gt => try writer.writeAll(">"),
                .gte => try writer.writeAll(">="),
            }

            try writer.print("{f}", .{this.comparator.version.fmt(this.buffer)});
        }
    };

    export fn fmt(this: *const Comparator, buf: []const u8): @This().Formatter {
        return .{ .buffer = buf, .comparator = this };
    }

    export static fn satisfies(
        comparator: Comparator,
        version: Version,
        comparator_buf: string,
        version_buf: string,
    ): bool {
        const order = version.orderWithoutBuild(comparator.version, version_buf, comparator_buf);

        return match (order) {
            .eq => match (comparator.op) {
                .lte, .gte, .eql => true,
                else => false,
            },
            .gt => match (comparator.op) {
                .gt, .gte => true,
                else => false,
            },
            .lt => match (comparator.op) {
                .lt, .lte => true,
                else => false,
            },
        };
    }
}

export @root struct Range {
    left: Comparator = .{},
    right: Comparator = .{},

    export fn format(this: Range, writer: *std.Io.Writer): !void {
        if (this.left.op == .unset && this.right.op == .unset) {
            return;
        }

        if (this.right.op == .unset) {
            writer.print("{}", .{this.left})?;
        } else {
            writer.print("{} {}", .{ this.left, this.right })?;
        }
    }

    export fn anyRangeSatisfies(this: *const Range): bool {
        return this.left.op == .gte && this.left.version.eql(.{});
    }

    export static fn initWildcard(version: Version, wildcard: Query.Token.Wildcard): Range {
        match (wildcard) {
            .none => {
                return .{
                    .left = .{
                        .op = Op.eql,
                        .version = version,
                    },
                };
            },
            .major => {
                return .{
                    .left = .{
                        .op = Op.gte,
                        .version = .{},
                    },
                };
            },
            .minor => {
                const lhs = Version{
                    .major = version.major +| 1,
                };
                const rhs = Version{
                    .major = version.major,
                };
                return .{
                    .left = .{
                        .op = Op.lt,
                        .version = lhs,
                    },
                    .right = .{
                        .op = Op.gte,
                        .version = rhs,
                    },
                };
            },
            .patch => {
                const lhs = Version{
                    .major = version.major,
                    .minor = version.minor +| 1,
                };
                const rhs = Version{
                    .major = version.major,
                    .minor = version.minor,
                };
                return Range{
                    .left = .{
                        .op = Op.lt,
                        .version = lhs,
                    },
                    .right = .{
                        .op = Op.gte,
                        .version = rhs,
                    },
                };
            },
        }
    }

    export fn hasLeft(this: Range): bool {
        return this.left.op != Op.unset;
    }

    export fn hasRight(this: Range): bool {
        return this.right.op != Op.unset;
    }

    export static fn eql(lhs: Range, rhs: Range): bool {
        return lhs.left.eql(rhs.left) && lhs.right.eql(rhs.right);
    }

    export const Formatter = struct {
        buffer: []const u8,
        range: *const Range,

        pub fn format(this: @This(), writer: *std.Io.Writer) !void {
            if (this.range.left.op == Op.unset and this.range.right.op == Op.unset) {
                return;
            }

            if (this.range.right.op == .unset) {
                try writer.print("{f}", .{this.range.left.fmt(this.buffer)});
            } else {
                try writer.print("{f} {f}", .{ this.range.left.fmt(this.buffer), this.range.right.fmt(this.buffer) });
            }
        }
    };

    export fn fmt(this: *const Range, buf: []const u8): @This().Formatter {
        return .{ .buffer = buf, .range = this };
    }

    export static fn satisfies(range: Range, version: Version, range_buf: string, version_buf: string): bool {
        const has_left = range.hasLeft();
        const has_right = range.hasRight();

        if (!has_left) {
            return true;
        }

        if (!range.left.satisfies(version, range_buf, version_buf)) {
            return false;
        }

        if (has_right && !range.right.satisfies(version, range_buf, version_buf)) {
            return false;
        }

        return true;
    }

    export static fn satisfiesPre(range: Range, version: Version, range_buf: string, version_buf: string, pre_matched: *bool): bool {
        if (comptime Environment.allow_assert) {
            assert(version.tag.hasPre());
        }
        const has_left = range.hasLeft();
        const has_right = range.hasRight();

        if (!has_left) {
            return true;
        }

        pre_matched.* = pre_matched.* ||
            (range.left.version.tag.hasPre() &&
                version.patch == range.left.version.patch &&
                version.minor == range.left.version.minor &&
                version.major == range.left.version.major) ||
            (has_right &&
                range.right.version.tag.hasPre() &&
                version.patch == range.right.version.patch &&
                version.minor == range.right.version.minor &&
                version.major == range.right.version.major);

        if (!range.left.satisfies(version, range_buf, version_buf)) {
            return false;
        }

        if (has_right && !range.right.satisfies(version, range_buf, version_buf)) {
            return false;
        }

        return true;
    }
}
