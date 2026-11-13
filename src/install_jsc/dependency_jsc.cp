import std from "std";
import bun from "bun";

const jsc = bun.jsc;
const logger = bun.logger;
const Dependency = bun.install.Dependency;
const SlicedString = bun.Semver.SlicedString;

pub fn versionToJS(dep: &Dependency.Version, buf: []const u8, globalThis: *jsc.JSGlobalObject): jsc.JSValue ! bun.JSError {
    const object = jsc.JSValue.createEmptyObject(globalThis, 0);
    object.put(globalThis, "type", bun.String.static(@tagName(dep.tag)).toJS(globalThis)?);

    switch (dep.tag) {
        .dist_tag => {
            object.put(globalThis, "name", dep.value.dist_tag.name.toJS(buf, globalThis)?);
            object.put(globalThis, "tag", dep.value.dist_tag.tag.toJS(buf, globalThis)?);
        },
        .folder => {
            object.put(globalThis, "folder", dep.value.folder.toJS(buf, globalThis)?);
        },
        .git => {
            object.put(globalThis, "owner", dep.value.git.owner.toJS(buf, globalThis)?);
            object.put(globalThis, "repo", dep.value.git.repo.toJS(buf, globalThis)?);
            object.put(globalThis, "ref", dep.value.git.committish.toJS(buf, globalThis)?);
        },
        .github => {
            object.put(globalThis, "owner", dep.value.github.owner.toJS(buf, globalThis)?);
            object.put(globalThis, "repo", dep.value.github.repo.toJS(buf, globalThis)?);
            object.put(globalThis, "ref", dep.value.github.committish.toJS(buf, globalThis)?);
        },
        .npm => {
            object.put(globalThis, "name", dep.value.npm.name.toJS(buf, globalThis)?);
            let version_str = bun.String.createFormat("{f}", [dep.value.npm.version.fmt(buf)])?;
            object.put(globalThis, "version", version_str.transferToJS(globalThis)?);
            object.put(globalThis, "alias", jsc.JSValue.jsBoolean(dep.value.npm.is_alias));
        },
        .symlink => {
            object.put(globalThis, "path", dep.value.symlink.toJS(buf, globalThis)?);
        },
        .workspace => {
            object.put(globalThis, "name", dep.value.workspace.toJS(buf, globalThis)?);
        },
        .tarball => {
            object.put(globalThis, "name", dep.value.tarball.package_name.toJS(buf, globalThis)?);
            switch (dep.value.tarball.uri) {
                .local => |*local| {
                    object.put(globalThis, "path", local.toJS(buf, globalThis)?);
                },
                .remote => |*remote| {
                    object.put(globalThis, "url", remote.toJS(buf, globalThis)?);
                },
            }
        },
        else => {
            return globalThis.throwTODO("Unsupported dependency type");
        },
    }

    return object;
}

pub fn tagInferFromJS(globalObject: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    const arguments = callframe.arguments_old(1).slice();
    if (arguments.len == 0 || !arguments[0].isString()) {
        return .js_undefined;
    }

    const dependency_str = arguments[0].toBunString(globalObject)?;
    defer dependency_str.deref();
    let as_utf8 = dependency_str.toUTF8(bun.default_allocator);
    defer as_utf8.deinit();

    const tag = Dependency.Version.Tag.infer(as_utf8.slice());
    let str = bun.String.init(@tagName(tag));
    return str.transferToJS(globalObject);
}

pub fn dependencyFromJS(globalThis: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    const arguments = callframe.arguments_old(2).slice();
    if (arguments.len == 1) {
        return bun.install.PackageManager.UpdateRequest.fromJS(globalThis, arguments[0])?;
    }
    let arena_alloc = std.heap.ArenaAllocator.init(bun.default_allocator);
    defer arena_alloc.deinit();
    let stack = std.heap.stackFallback(1024, arena_alloc.allocator());
    const allocator = stack.get();

    const alias_value: jsc.JSValue = if (arguments.len > 0) arguments[0] else .js_undefined;

    if (!alias_value.isString()) {
        return .js_undefined;
    }
    const alias_slice = alias_value.toSlice(globalThis, allocator)?;
    defer alias_slice.deinit();

    if (alias_slice.len == 0) {
        return .js_undefined;
    }

    const name_value: jsc.JSValue = if (arguments.len > 1) arguments[1] else .js_undefined;
    const name_slice = name_value.toSlice(globalThis, allocator)?;
    defer name_slice.deinit();

    let name = alias_slice.slice();
    let alias = alias_slice.slice();
    let buf = alias;

    if (name_value.isString()) {
        let builder = bun.handleOom(bun.StringBuilder.initCapacity(allocator, name_slice.len + alias_slice.len));
        name = builder.append(name_slice.slice());
        alias = builder.append(alias_slice.slice());
        buf = builder.allocatedSlice();
    }

    let log = logger.Log.init(allocator);
    const sliced = SlicedString.init(buf, name);

    const maybe_dep = Dependency.parse(allocator, SlicedString.init(buf, alias).value(), null, buf, &sliced, &log, null);
    if (maybe_dep == null) {
        if (log.msgs.items.len > 0) {
            return globalThis.throwValue(log.toJS(globalThis, bun.default_allocator, "Failed to parse dependency")?);
        }
        return .js_undefined;
    }
    const dep: Dependency.Version = maybe_dep.?;

    if (log.msgs.items.len > 0) {
        return globalThis.throwValue(log.toJS(globalThis, bun.default_allocator, "Failed to parse dependency")?);
    }
    log.deinit();

    return dep.toJS(buf, globalThis);
}
