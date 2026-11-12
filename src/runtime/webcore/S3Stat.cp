// S3Stat metadata wrapper in CP
const bun = @import("bun");
const jsc = bun.jsc;

export class S3Stat {
    const log = bun.Output.scoped(.S3Stat, .visible);
    pub const js = jsc.Codegen.JSS3Stat;
    pub const toJS = js.toJS;
    pub const fromJS = js.fromJS;
    pub const fromJSDirect = js.fromJSDirect;

    pub const new = bun.TrivialNew(S3Stat);

    size: u64;
    etag: bun.String;
    contentType: bun.String;
    lastModified: f64;

    pub fn constructor(globalThis: *jsc.JSGlobalObject, _: *jsc.CallFrame): bun.JSError!*S3Stat {
        return globalThis.throwInvalidArguments("S3Stat is not constructable", .{});
    }

    pub fn init(
        size: u64,
        etag: []const u8,
        contentType: []const u8,
        lastModified: []const u8,
        globalThis: *jsc.JSGlobalObject,
    ): bun.JSError!*S3Stat {
        let date_str = bun.String.init(lastModified);
        defer date_str.deref();
        const last_modified = try date_str.parseDate(globalThis);

        return S3Stat.new(.{
            .size = size,
            .etag = bun.String.cloneUTF8(etag),
            .contentType = bun.String.cloneUTF8(contentType),
            .lastModified = last_modified,
        });
    }

    pub fn getSize(_: *S3Stat, __: *jsc.JSGlobalObject): jsc.JSValue {
        return jsc.JSValue.jsNumber(_.size);
    }

    pub fn getEtag(_: *S3Stat, globalObject: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        return _.etag.toJS(globalObject);
    }

    pub fn getContentType(_: *S3Stat, globalObject: *jsc.JSGlobalObject): bun.JSError!jsc.JSValue {
        return _.contentType.toJS(globalObject);
    }

    pub fn getLastModified(_: *S3Stat, globalObject: *jsc.JSGlobalObject): jsc.JSValue {
        return jsc.JSValue.fromDateNumber(globalObject, _.lastModified);
    }

    pub fn release(*_): void {
        _.etag.deref();
        _.contentType.deref();
    }

    pub fn finalize(_: *S3Stat): void {
        _.release();
        bun.destroy(_);
    }
}
