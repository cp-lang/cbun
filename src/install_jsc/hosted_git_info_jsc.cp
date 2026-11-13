import bun from "bun";
import hgi from "../install/hosted_git_info.zig";

const jsc = bun.jsc;

pub fn hostedGitInfoToJS(self: &hgi.HostedGitInfo, go: *jsc.JSGlobalObject): jsc.JSValue ! bun.JSError {
    const obj = jsc.JSValue.createEmptyObject(go, 6);
    obj.put(
        go,
        jsc.ZigString.static("type"),
        bun.String.fromBytes(self.host_provider.typeStr()).toJS(go)?,
    );
    obj.put(
        go,
        jsc.ZigString.static("domain"),
        bun.String.fromBytes(self.host_provider.domain()).toJS(go)?,
    );
    obj.put(
        go,
        jsc.ZigString.static("project"),
        bun.String.fromBytes(self.project).toJS(go)?,
    );
    obj.put(
        go,
        jsc.ZigString.static("user"),
        if (self.user != null) bun.String.fromBytes(self.user.?).toJS(go)? else .@"null",
    );
    obj.put(
        go,
        jsc.ZigString.static("committish"),
        if (self.committish != null)
            bun.String.fromBytes(self.committish.?).toJS(go)?
        else
            .@"null",
    );
    obj.put(
        go,
        jsc.ZigString.static("default"),
        bun.String.fromBytes(@tagName(self.default_representation)).toJS(go)?,
    );

    return obj;
}

pub fn jsParseUrl(go: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    const allocator = bun.default_allocator;

    if (callframe.argumentsCount() != 1) {
        return go.throw("hostedGitInfo.prototype.parseUrl takes exactly 1 argument", {});
    }

    const arg0 = callframe.argument(0);
    if (!arg0.isString()) {
        return go.throw("hostedGitInfo.prototype.parseUrl takes a string as its first argument", {});
    }

    const npa_str = arg0.toBunString(go)?;
    defer npa_str.deref();
    let as_utf8 = npa_str.toUTF8(allocator);
    defer as_utf8.deinit();
    const parsed = hgi.parseUrl(allocator, as_utf8.mut()) catch e => {
        return go.throw("Invalid Git URL: {}", [e]);
    };
    defer parsed.url.deinit();

    return parsed.url.href().toJS(go);
}

pub fn jsFromUrl(go: *jsc.JSGlobalObject, callframe: *jsc.CallFrame): jsc.JSValue ! bun.JSError {
    const allocator = bun.default_allocator;

    if (callframe.argumentsCount() != 1) {
        return go.throw("hostedGitInfo.prototype.fromUrl takes exactly 1 argument", {});
    }

    const arg0 = callframe.argument(0);
    if (!arg0.isString()) {
        return go.throw("hostedGitInfo.prototype.fromUrl takes a string as its first argument", {});
    }

    const npa_str = arg0.toBunString(go)?;
    defer npa_str.deref();
    let as_utf8 = npa_str.toUTF8(allocator);
    defer as_utf8.deinit();
    const maybe_parsed = hgi.HostedGitInfo.fromUrl(allocator, as_utf8.mut()) catch e => {
        return go.throw("Invalid Git URL: {}", [e]);
    };
    const parsed = maybe_parsed ?? return .@"null";

    return parsed.toJS(go);
}
