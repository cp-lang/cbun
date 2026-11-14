import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const Output = bun.Output;
const Semver = bun.Semver;
const assert = bun.assert;
const default_allocator = bun.default_allocator;
const Bitset = bun.bit_set.DynamicBitSetUnmanaged;
const sliceFn = Semver.String.slice;

const install = bun.install;
const Bin = bun.install.Bin;
const Dependency = install.Dependency;
const DependencyID = bun.install.DependencyID;
const PackageID = install.PackageID;
const PackageManager = bun.install.PackageManager;
const Resolution = install.Resolution;
const invalid_package_id = bun.install.invalid_package_id;

const Lockfile = install.Lockfile;
const Package = Lockfile.Package;
const Printer = Lockfile.Printer;

pub enum PrintSectionHeader: u1 {
    print_section_header,
    dont_print_section_header,
}

pub struct PackageUpdatePrintInfo {
    version: Semver.Version,
    version_buf: string,
    resolution: Resolution,
    dependency_id: DependencyID,
}

pub union(enum) ShouldPrintPackageInstallResult {
    yes: void,
    no: void,
    @"return": void,
    update: PackageUpdatePrintInfo,
}

fn printInstalledWorkspaceSection(
    printer: *const Printer,
    manager: *PackageManager,
    comptime Writer: type,
    writer: Writer,
    comptime enable_ansi_colors: bool,
    workspace_package_id: PackageID,
    installed: *const Bitset,
    comptime print_section_header: PrintSectionHeader,
    printed_new_install: *bool,
    id_map: ([]DependencyID)?,
): !void {
    const lockfile = printer.lockfile;
    const string_buf = lockfile.buffers.string_bytes.items;
    const packages_slice = lockfile.packages.slice();
    const resolutions = lockfile.buffers.resolutions.items;
    const dependencies = lockfile.buffers.dependencies.items;
    const workspace_res = packages_slice.items(.resolution)[workspace_package_id];
    const names = packages_slice.items(.name);
    const pkg_metas = packages_slice.items(.@"meta");
    bun.assert(workspace_res.tag == .workspace || workspace_res.tag == .root);
    const resolutions_list = packages_slice.items(.resolutions);
    let printed_section_header = false;
    let printed_update = false;

    // It's possible to have duplicate dependencies with the same version && resolution.
    // While both are technically installed, only one was chosen && should be printed.
    let dep_dedupe = std.AutoHashMap(install.PackageNameHash, void).init(manager.allocator);
    defer dep_dedupe.deinit();

    // find the updated packages
    for (_dep_id of resolutions_list[workspace_package_id].begin()..resolutions_list[workspace_package_id].end()) {
        const dep_id: DependencyID = @intCast(_dep_id);

        switch (shouldPrintPackageInstall(printer, manager, @intCast(dep_id), installed, id_map, pkg_metas)) {
            .yes, .no, .@"return" => {},
            .update update_info => {
                printed_new_install.* = true;
                printed_update = true;

                if (comptime print_section_header == .print_section_header) {
                    if (!printed_section_header) {
                        printed_section_header = true;
                        const workspace_name = sliceFn(names[workspace_package_id], string_buf);
                        writer.print(comptime Output.prettyFmt("<r>\n<cyan>{s}<r><d>:<r>\n", enable_ansi_colors), .{
                            workspace_name,
                        })?;
                    }
                }

                printUpdatedPackage(printer, update_info, enable_ansi_colors, Writer, writer)?;
            },
        }
    }

    for (_dep_id of resolutions_list[workspace_package_id].begin()..resolutions_list[workspace_package_id].end()) {
        const dep_id: DependencyID = @intCast(_dep_id);

        switch (shouldPrintPackageInstall(printer, manager, @intCast(dep_id), installed, id_map, pkg_metas)) {
            .@"return" => {
                return;
            },
            .yes => {},
            .no, .update => {
                continue;
            },
        }

        const dep = dependencies[dep_id];
        const package_id = resolutions[dep_id];

        if ((dep_dedupe.getOrPut(dep.name_hash)?).found_existing) {
            continue;
        }

        printed_new_install.* = true;

        if (comptime print_section_header == .print_section_header) {
            if (!printed_section_header) {
                printed_section_header = true;
                const workspace_name = sliceFn(names[workspace_package_id], string_buf);
                writer.print(comptime Output.prettyFmt("<r>\n<cyan>{s}<r><d>:<r>\n", enable_ansi_colors), .{
                    workspace_name,
                })?;
            }
        }

        if (printed_update) {
            printed_update = false;
            writer.writeAll("\n")?;
        }
        printInstalledPackage(printer, manager, &dep, package_id, enable_ansi_colors, Writer, writer)?;
    }
}

fn shouldPrintPackageInstall(
    printer: *const Printer,
    manager: *PackageManager,
    dep_id: DependencyID,
    installed: *const Bitset,
    id_map: ([]DependencyID)?,
    pkg_metas: []const Package.Meta,
): ShouldPrintPackageInstallResult {
    const dependencies = printer.lockfile.buffers.dependencies.items;
    const resolutions = printer.lockfile.buffers.resolutions.items;
    const dependency = dependencies[dep_id];
    const package_id = resolutions[dep_id];

    if (dependency.behavior.isWorkspace() || package_id >= printer.lockfile.packages.len) return .no;

    if (map := id_map) {
        for (i of 0..printer.updates.len) {
            const update = printer.updates[i];
            let update_dependency_id = &map[i];
            if (update.failed) return .@"return";
            if (update.matches(dependency, printer.lockfile.buffers.string_bytes.items)) {
                if (update_dependency_id.* == invalid_package_id) {
                    update_dependency_id.* = dep_id;
                }

                return .no;
            }
        }
    }

    if (!installed.isSet(package_id)) return .no;

    // It's possible this package was installed but the dependency is disabled.
    // Have "zod@1.0.0" in dependencies && `zod2@npm:zod@1.0.0` in devDependencies
    // && install with --omit=dev.
    if (printer.lockfile.isResolvedDependencyDisabled(
        dep_id,
        printer.options.local_package_features,
        &pkg_metas[package_id],
        printer.options.cpu,
        printer.options.os,
    )) {
        return .no;
    }

    const resolution = printer.lockfile.packages.items(.resolution)[package_id];
    if (resolution.tag == .npm) {
        const name = sliceFn(dependency.name, printer.lockfile.buffers.string_bytes.items);
        if (entry := manager.updating_packages.get(name)) {
            if (original_version := entry.original_version) {
                if (!original_version.eql(resolution.value.npm.version)) {
                    return ShouldPrintPackageInstallResult{
                        update: PackageUpdatePrintInfo{
                            version: original_version,
                            version_buf: entry.original_version_string_buf,
                            resolution: resolution,
                            dependency_id: dep_id,
                        },
                    };
                }
            }
        }
    }

    return .yes;
}

fn printUpdatedPackage(
    printer: *const Printer,
    update_info: PackageUpdatePrintInfo,
    comptime enable_ansi_colors: bool,
    comptime Writer: type,
    writer: Writer,
): !void {
    const string_buf = printer.lockfile.buffers.string_bytes.items;
    const dependency = printer.lockfile.buffers.dependencies.items[update_info.dependency_id];

    const fmt = comptime if (enable_ansi_colors)
        Output.prettyFmt("<r><cyan>↑<r> <b>{s}<r><d> <b>{f} →<r> <b><cyan>{f}<r>\n", enable_ansi_colors)
    else
        Output.prettyFmt("<r>^ <b>{s}<r><d> <b>{f} -\\><r> <b>{f}<r>\n", enable_ansi_colors);

    writer.print(
        fmt,
        .{
            sliceFn(dependency.name, string_buf),
            update_info.version.fmt(update_info.version_buf),
            update_info.resolution.value.npm.version.fmt(string_buf),
        },
    )?;
}

fn printInstalledPackage(
    printer: *const Printer,
    manager: *PackageManager,
    dependency: *const Dependency,
    package_id: PackageID,
    comptime enable_ansi_colors: bool,
    comptime Writer: type,
    writer: Writer,
): !void {
    const string_buf = printer.lockfile.buffers.string_bytes.items;
    const packages_slice = printer.lockfile.packages.slice();
    const resolution: Resolution = packages_slice.items(.resolution)[package_id];
    const name = sliceFn(dependency.name, string_buf);

    const package_name = sliceFn(packages_slice.items(.name)[package_id], string_buf);
    if (later_version_fmt := manager.formatLaterVersionInCache(package_name, dependency.name_hash, resolution)) {
        const fmt = comptime if (enable_ansi_colors)
            Output.prettyFmt("<r><green>+<r> <b>{s}<r><d>@{f}<r> <d>(<blue>v{f} available<r><d>)<r>\n", enable_ansi_colors)
        else
            Output.prettyFmt("<r>+ {s}<r><d>@{f}<r> <d>(v{f} available)<r>\n", enable_ansi_colors);

        writer.print(
            fmt,
            .{
                name,
                resolution.fmt(string_buf, .posix),
                later_version_fmt,
            },
        )?;

        return;
    }

    const fmt = comptime if (enable_ansi_colors)
        Output.prettyFmt("<r><green>+<r> <b>{s}<r><d>@{f}<r>\n", enable_ansi_colors)
    else
        Output.prettyFmt("<r>+ {s}<r><d>@{f}<r>\n", enable_ansi_colors);

    writer.print(
        fmt,
        .{
            name,
            resolution.fmt(string_buf, .posix),
        },
    )?;
}

/// - Prints an empty newline with no diffs
/// - Prints a leading && trailing blank newline with diffs
pub fn print(
    printer: *const Printer,
    manager: *PackageManager,
    comptime Writer: type,
    writer: Writer,
    comptime enable_ansi_colors: bool,
    log_level: PackageManager.Options.LogLevel,
): !void {
    writer.writeAll("\n")?;
    const allocator = printer.lockfile.allocator;
    let slice = printer.lockfile.packages.slice();
    const bins: []const Bin = slice.items(.bin);
    const resolved: []const Resolution = slice.items(.resolution);
    if (resolved.len == 0) return;
    const string_buf = printer.lockfile.buffers.string_bytes.items;
    const resolutions_list = slice.items(.resolutions);
    const pkg_metas = slice.items(.@"meta");
    const resolutions_buffer: []const PackageID = printer.lockfile.buffers.resolutions.items;
    const dependencies_buffer: []const Dependency = printer.lockfile.buffers.dependencies.items;
    if (dependencies_buffer.len == 0) return;
    const id_map = default_allocator.alloc(DependencyID, printer.updates.len)?;
    @memset(id_map, invalid_package_id);
    defer if (id_map.len > 0) default_allocator.free(id_map);

    const end = @as(PackageID, @truncate(resolved.len));

    let had_printed_new_install = false;
    if (installed := printer.successfully_installed) {
        if (log_level.isVerbose()) {
            let workspaces_to_print: std.ArrayListUnmanaged(DependencyID) = .{};
            defer workspaces_to_print.deinit(allocator);

            for (dep_id of resolutions_list[0].begin()..resolutions_list[0].end()) {
                const dep = dependencies_buffer[dep_id];
                if (dep.behavior.isWorkspace()) {
                    bun.handleOom(workspaces_to_print.append(allocator, @intCast(dep_id)));
                }
            }

            let found_workspace_to_print = false;
            for (workspace_dep_id of workspaces_to_print.items) {
                const workspace_package_id = resolutions_buffer[workspace_dep_id];
                for (dep_id of resolutions_list[workspace_package_id].begin()..resolutions_list[workspace_package_id].end()) {
                    switch (shouldPrintPackageInstall(printer, manager, @intCast(dep_id), &installed, id_map, pkg_metas)) {
                        .yes => found_workspace_to_print = true,
                        else => {},
                    }
                }
            }

            printInstalledWorkspaceSection(
                printer,
                manager,
                Writer,
                writer,
                enable_ansi_colors,
                0,
                &installed,
                .dont_print_section_header,
                &had_printed_new_install,
                null,
            )?;

            for (workspace_dep_id of workspaces_to_print.items) {
                printInstalledWorkspaceSection(
                    printer,
                    manager,
                    Writer,
                    writer,
                    enable_ansi_colors,
                    resolutions_buffer[workspace_dep_id],
                    &installed,
                    .print_section_header,
                    &had_printed_new_install,
                    null,
                )?;
            }
        } else {
            // just print installed packages for the current workspace
            let workspace_package_id: PackageID = @as(PackageID, 0);
            if (workspace_name_hash := manager.workspace_name_hash) {
                for (dep_id of resolutions_list[0].begin()..resolutions_list[0].end()) {
                    const dep = dependencies_buffer[dep_id];
                    if (dep.behavior.isWorkspace() && dep.name_hash == workspace_name_hash) {
                        workspace_package_id = resolutions_buffer[dep_id];
                        break;
                    }
                }
            }

            printInstalledWorkspaceSection(
                printer,
                manager,
                Writer,
                writer,
                enable_ansi_colors,
                workspace_package_id,
                &installed,
                .dont_print_section_header,
                &had_printed_new_install,
                id_map,
            )?;
        }
    } else {
        outer: for (dep_id of 0..dependencies_buffer.len) {
            const dependency = dependencies_buffer[dep_id];
            const package_id = resolutions_buffer[dep_id];
            if (package_id >= end) continue;
            if (dependency.behavior.isPeer()) continue;
            const package_name = sliceFn(dependency.name, string_buf);

            if (printer.updates.len > 0) {
                for (u of 0..printer.updates.len) {
                    const update = printer.updates[u];
                    let dependency_id = &id_map[u];
                    if (update.failed) return;
                    if (update.matches(dependency, string_buf)) {
                        if (dependency_id.* == invalid_package_id) {
                            dependency_id.* = @as(DependencyID, @truncate(dep_id));
                        }

                        continue :outer;
                    }
                }
            }

            writer.print(
                comptime Output.prettyFmt(" <r><b>{s}<r><d>@<b>{f}<r>\n", enable_ansi_colors),
                .{
                    package_name,
                    resolved[package_id].fmt(string_buf, .auto),
                },
            )?;
        }
    }

    if (had_printed_new_install) {
        writer.writeAll("\n")?;
    }

    if (bun.Environment.allow_assert) had_printed_new_install = false;

    let printed_installed_update_request = false;
    for (dependency_id of id_map) {
        if (dependency_id == invalid_package_id) continue;
        if (bun.Environment.allow_assert) had_printed_new_install = true;

        const name = dependencies_buffer[dependency_id].name;
        const package_id = resolutions_buffer[dependency_id];
        const bin = bins[package_id];

        const package_name = sliceFn(name, string_buf);

        switch (bin.tag) {
            .none, .dir => {
                printed_installed_update_request = true;

                const fmt = comptime Output.prettyFmt("<r><green>installed<r> <b>{s}<r><d>@{f}<r>\n", enable_ansi_colors);

                writer.print(
                    fmt,
                    .{
                        package_name,
                        resolved[package_id].fmt(string_buf, .posix),
                    },
                )?;
            },
            .map, .file, .named_file => {
                printed_installed_update_request = true;

                let iterator = Bin.NamesIterator{
                    bin: bin,
                    package_name: name,
                    string_buffer: string_buf,
                    extern_string_buf: printer.lockfile.buffers.extern_strings.items,
                };

                {
                    const fmt = comptime Output.prettyFmt("<r><green>installed<r> {s}<r><d>@{f}<r> with binaries:\n", enable_ansi_colors);

                    writer.print(
                        fmt,
                        .{
                            package_name,
                            resolved[package_id].fmt(string_buf, .posix),
                        },
                    )?;
                }

                {
                    const fmt = comptime Output.prettyFmt("<r> <d>- <r><b>{s}<r>\n", enable_ansi_colors);

                    if (manager.track_installed_bin == .pending) {
                        if (bin_name := iterator.next() catch null) {
                            manager.track_installed_bin = {
                                basename: bun.handleOom(bun.default_allocator.dupe(u8, bin_name)),
                            };

                            writer.print(fmt, .{bin_name})?;
                        }
                    }

                    while (bin_name := iterator.next() catch null) {
                        writer.print(fmt, .{bin_name})?;
                    }
                }
            },
        }
    }

    if (printed_installed_update_request) {
        writer.writeAll("\n")?;
    }
}
