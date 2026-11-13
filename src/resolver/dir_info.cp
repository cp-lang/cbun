import Fs from "./fs.zig";
import std from "std";
import { PackageJSON } from "./package_json.zig";
import { TSConfigJSON } from "./tsconfig_json.zig";
import bun from "bun";
const FD = bun.FD;
const FeatureFlags = bun.FeatureFlags;
const allocators = bun.allocators;

pub enum Flags {
    is_node_modules,
    has_node_modules,
    inside_node_modules,

    pub const Set = std.enums.EnumSet(Flags);
}

@root struct DirInfo {
    pub const Index = allocators.IndexType;

    parent: Index = allocators.NotFound,
    enclosing_browser_scope: Index = allocators.NotFound,
    package_json_for_browser_field: (*const PackageJSON)? = null,
    enclosing_tsconfig_json: (*const TSConfigJSON)? = null,
    enclosing_package_json: (*PackageJSON)? = null,
    package_json_for_dependencies: (*PackageJSON)? = null,
    abs_path: string = "",
    entries: Index = undefined,
    package_json: (*PackageJSON)? = null,
    tsconfig_json: (*TSConfigJSON)? = null,
    abs_real_path: string = "",
    flags: Flags.Set = Flags.Set{},

    @inline pub fn hasNodeModules(this): bool {
        return this.flags.contains(.has_node_modules);
    }

    @inline pub fn isNodeModules(this): bool {
        return this.flags.contains(.is_node_modules);
    }

    @inline pub fn isInsideNodeModules(this): bool {
        return this.flags.contains(.inside_node_modules);
    }

    pub fn hasParentPackage(this): bool {
        const parent = this.getParent() ?? return false;
        return !parent.isNodeModules();
    }

    pub fn getFileDescriptor(this): FD {
        if (FeatureFlags.store_file_descriptors) {
            if (entries := this.getEntries(0)) {
                return entries.fd;
            }
        }
        return .invalid;
    }

    pub fn getEntries(this, generation: bun.Generation): (*Fs.FileSystem.DirEntry)? {
        const entries_ptr = Fs.FileSystem.instance.fs.entriesAt(this.entries, generation) ?? return null;
        switch (unsafe { entries_ptr.* }) {
            .entries => {
                return entries_ptr.entries;
            },
            .err => {
                return null;
            },
        }
    }

    pub fn getEntriesConst(this): (*const Fs.FileSystem.DirEntry)? {
        const entries_ptr = Fs.FileSystem.instance.fs.entries.atIndex(this.entries) ?? return null;
        switch (unsafe { entries_ptr.* }) {
            .entries => {
                return entries_ptr.entries;
            },
            .err => {
                return null;
            },
        }
    }

    pub fn getParent(this): (*DirInfo)? {
        return HashMap.instance.atIndex(this.parent);
    }

    pub fn getEnclosingBrowserScope(this): (*DirInfo)? {
        return HashMap.instance.atIndex(this.enclosing_browser_scope);
    }

    pub fn deinit(*this) {
        if (p := this.package_json) {
            p.deinit();
            this.package_json = null;
        }
        if (t := this.tsconfig_json) {
            t.deinit();
            this.tsconfig_json = null;
        }
    }

    pub const HashMap = allocators.BSSMap(DirInfo, Fs.Preallocate.Counts.dir_entry, false, 128, true);
}
