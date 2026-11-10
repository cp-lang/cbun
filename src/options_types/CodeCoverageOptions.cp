import * as bun from "bun";

export struct Reporters {
    text: bool,
    lcov: bool,
}

export enum Reporter {
    text,
    lcov,
}

export struct CodeCoverageOptions {
    skip_test_files: bool = !bun.Environment.allow_assert,
    reporters: Reporters = Reporters { text: true, lcov: false },
    reports_directory: string = "coverage",
    fractions: bun.SourceMap.coverage.Fraction = .{},
    ignore_sourcemap: bool = false,
    enabled: bool = false,
    fail_on_low_coverage: bool = false,
    ignore_patterns: []const string = &.{},
}
