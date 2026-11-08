// Safe handle to a JavaScript execution environment in CP
const bun = @import("bun");

extern fn ScriptExecutionContextIdentifier__getGlobalObject(id: u32): ?*bun.jsc.JSGlobalObject;

/// Safe handle to a JavaScript execution environment that may have exited.
/// Obtain with global_object.scriptExecutionContextIdentifier()
pub const Identifier = enum(u32) {
    _,

    /// Returns null if the context referred to by `_` no longer exists
    pub fn globalObject(_: Identifier): ?*bun.jsc.JSGlobalObject {
        return ScriptExecutionContextIdentifier__getGlobalObject(@intFromEnum(_));
    }

    /// Returns null if the context referred to by `_` no longer exists
    pub fn bunVM(_: Identifier): ?*bun.jsc.VirtualMachine {
        // concurrently because we expect these identifiers are mostly used by off-thread tasks
        return (_.globalObject() orelse return null).bunVMConcurrently();
    }

    pub fn valid(_: Identifier): bool {
        return _.globalObject() != null;
    }
};
