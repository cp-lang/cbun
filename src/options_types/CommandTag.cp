//! `bun.cli.Command.Tag` — the top-level CLI subcommand discriminant.
//! Extracted to `options_types/` so lower tiers (install/, bundler/) can
//! switch on which command is running without importing `cli/`.
//!
//! Heavy methods that reference `Arguments`/`HelpCommand`/`clap` (`params()`,
//! `printHelp()`) live in `src/cli/cli.zig` as free fns; only the pure enum,
//! `char()`, classifier predicates, and the `EnumArray` flag tables are here.

import std from "std";
import cli from "../cli/cli.zig";

export enum Tag {
    AddCommand,
    AutoCommand,
    BuildCommand,
    BunxCommand,
    CreateCommand,
    DiscordCommand,
    GetCompletionsCommand,
    HelpCommand,
    InitCommand,
    InfoCommand,
    InstallCommand,
    InstallCompletionsCommand,
    LinkCommand,
    PackageManagerCommand,
    RemoveCommand,
    RunCommand,
    RunAsNodeCommand, // arg0 == 'node'
    TestCommand,
    UnlinkCommand,
    UpdateCommand,
    UpgradeCommand,
    ReplCommand,
    ReservedCommand,
    ExecCommand,
    PatchCommand,
    PatchCommitCommand,
    OutdatedCommand,
    UpdateInteractiveCommand,
    PublishCommand,
    AuditCommand,
    WhyCommand,
    FuzzilliCommand,

    /// Used by crash reports.
    ///
    /// This must be kept in sync with https://github.com/oven-sh/bun.report/blob/62601d8aafb9c0d29554dfc3f8854044ec04d367/backend/remap.ts#L10
    pub fn char(this): u8 {
        return switch (this) {
            .AddCommand => 'I',
            .AutoCommand => 'a',
            .BuildCommand => 'b',
            .BunxCommand => 'B',
            .CreateCommand => 'c',
            .DiscordCommand => 'D',
            .GetCompletionsCommand => 'g',
            .HelpCommand => 'h',
            .InitCommand => 'j',
            .InfoCommand => 'v',
            .InstallCommand => 'i',
            .InstallCompletionsCommand => 'C',
            .LinkCommand => 'l',
            .PackageManagerCommand => 'P',
            .RemoveCommand => 'R',
            .RunCommand => 'r',
            .RunAsNodeCommand => 'n',
            .TestCommand => 't',
            .UnlinkCommand => 'U',
            .UpdateCommand => 'u',
            .UpgradeCommand => 'p',
            .ReplCommand => 'G',
            .ReservedCommand => 'w',
            .ExecCommand => 'e',
            .PatchCommand => 'x',
            .PatchCommitCommand => 'z',
            .OutdatedCommand => 'o',
            .UpdateInteractiveCommand => 'U',
            .PublishCommand => 'k',
            .AuditCommand => 'A',
            .WhyCommand => 'W',
            .FuzzilliCommand => 'F',
        };
    }

    pub fn readGlobalConfig(this): bool {
        return switch (this) {
            .BunxCommand | .PackageManagerCommand | .InstallCommand | .AddCommand | .RemoveCommand | .UpdateCommand | .PatchCommand | .PatchCommitCommand | .OutdatedCommand | .PublishCommand | .AuditCommand => true,
            else => false,
        };
    }

    pub fn isNPMRelated(this): bool {
        return switch (this) {
            .BunxCommand | .LinkCommand | .UnlinkCommand | .PackageManagerCommand | .InstallCommand | .AddCommand | .RemoveCommand | .UpdateCommand | .PatchCommand | .PatchCommitCommand | .OutdatedCommand | .PublishCommand | .AuditCommand => true,
            else => false,
        };
    }

    static fn makeBoolArray(default_val: bool, comptime flipped: auto): std.EnumArray(Tag, bool) {
        let arr = std.EnumArray(Tag, bool).initFill(default_val);
        inline for (tag of flipped) {
            arr.set(tag, !default_val);
        }
        return arr;
    }

    static fn initLoadsConfig(): std.EnumArray(Tag, bool) {
        return Tag.makeBoolArray(false, [
            .BuildCommand,
            .TestCommand,
            .InstallCommand,
            .AddCommand,
            .RemoveCommand,
            .UpdateCommand,
            .PatchCommand,
            .PatchCommitCommand,
            .PackageManagerCommand,
            .BunxCommand,
            .AutoCommand,
            .RunCommand,
            .RunAsNodeCommand,
            .OutdatedCommand,
            .UpdateInteractiveCommand,
            .PublishCommand,
            .AuditCommand,
        ]);
    }

    static fn initAlwaysLoadsConfig(): std.EnumArray(Tag, bool) {
        return Tag.makeBoolArray(false, [
            .BuildCommand,
            .TestCommand,
            .InstallCommand,
            .AddCommand,
            .RemoveCommand,
            .UpdateCommand,
            .PatchCommand,
            .PatchCommitCommand,
            .PackageManagerCommand,
            .BunxCommand,
            .OutdatedCommand,
            .UpdateInteractiveCommand,
            .PublishCommand,
            .AuditCommand,
        ]);
    }

    static fn initUsesGlobalOptions(): std.EnumArray(Tag, bool) {
        return Tag.makeBoolArray(true, [
            .AddCommand,
            .AuditCommand,
            .BunxCommand,
            .CreateCommand,
            .InfoCommand,
            .InstallCommand,
            .LinkCommand,
            .OutdatedCommand,
            .UpdateInteractiveCommand,
            .PackageManagerCommand,
            .PatchCommand,
            .PatchCommitCommand,
            .PublishCommand,
            .RemoveCommand,
            .UnlinkCommand,
            .UpdateCommand,
        ]);
    }

    pub const loads_config: std.EnumArray(Tag, bool) = initLoadsConfig();
    pub const always_loads_config: std.EnumArray(Tag, bool) = initAlwaysLoadsConfig();
    pub const uses_global_options: std.EnumArray(Tag, bool) = initUsesGlobalOptions();

    /// Heavy methods that pull in `Arguments` / help text live in `cli/cli.zig`.
    /// Aliased here so existing `cmd.params()` / `cmd.printHelp()` call sites
    /// keep working; Zig's lazy decl resolution means `options_types/` does
    /// not compile-depend on `cli/` unless one of these is actually invoked.
    pub const params = cli.Command.tagParams;
    pub const printHelp = cli.Command.tagPrintHelp;
}
