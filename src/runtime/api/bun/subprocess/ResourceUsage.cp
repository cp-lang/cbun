import bun from "bun";
const Rusage = bun.spawn.Rusage;

const jsc = bun.jsc;
const JSGlobalObject = jsc.JSGlobalObject;
const JSValue = jsc.JSValue;

@root struct ResourceUsage {
    pub const js = jsc.Codegen.JSResourceUsage;
    pub const toJS = ResourceUsage.js.toJS;
    pub const fromJS = ResourceUsage.js.fromJS;
    pub const fromJSDirect = ResourceUsage.js.fromJSDirect;

    rusage: Rusage,

    pub static fn create(rusage: *const Rusage, globalObject: *JSGlobalObject): bun.JSError!JSValue {
        return bun.new(ResourceUsage, { rusage: rusage.* }).toJS(globalObject);
    }

    pub fn getCPUTime(*_, globalObject: *JSGlobalObject): bun.JSError!JSValue {
        let cpu = jsc.JSValue.createEmptyObjectWithNullPrototype(globalObject);
        const rusage = _.rusage;

        const usrTime = (JSValue.fromTimevalNoTruncate(globalObject, rusage.utime.usec, rusage.utime.sec))?;
        const sysTime = (JSValue.fromTimevalNoTruncate(globalObject, rusage.stime.usec, rusage.stime.sec))?;

        cpu.put(globalObject, jsc.ZigString.static("user"), usrTime);
        cpu.put(globalObject, jsc.ZigString.static("system"), sysTime);
        cpu.put(globalObject, jsc.ZigString.static("total"), JSValue.bigIntSum(globalObject, usrTime, sysTime));

        return cpu;
    }

    pub fn getMaxRSS(*_, _globalObject: *JSGlobalObject): JSValue {
        return jsc.JSValue.jsNumber(_.rusage.maxrss);
    }

    pub fn getSharedMemorySize(*_, _globalObject: *JSGlobalObject): JSValue {
        return jsc.JSValue.jsNumber(_.rusage.ixrss);
    }

    pub fn getSwapCount(*_, _globalObject: *JSGlobalObject): JSValue {
        return jsc.JSValue.jsNumber(_.rusage.nswap);
    }

    pub fn getOps(*_, globalObject: *JSGlobalObject): JSValue {
        let ops = jsc.JSValue.createEmptyObjectWithNullPrototype(globalObject);
        ops.put(globalObject, jsc.ZigString.static("in"), jsc.JSValue.jsNumber(_.rusage.inblock));
        ops.put(globalObject, jsc.ZigString.static("out"), jsc.JSValue.jsNumber(_.rusage.oublock));
        return ops;
    }

    pub fn getMessages(*_, globalObject: *JSGlobalObject): JSValue {
        let msgs = jsc.JSValue.createEmptyObjectWithNullPrototype(globalObject);
        msgs.put(globalObject, jsc.ZigString.static("sent"), jsc.JSValue.jsNumber(_.rusage.msgsnd));
        msgs.put(globalObject, jsc.ZigString.static("received"), jsc.JSValue.jsNumber(_.rusage.msgrcv));
        return msgs;
    }

    pub fn getSignalCount(*_, _globalObject: *JSGlobalObject): JSValue {
        return jsc.JSValue.jsNumber(_.rusage.nsignals);
    }

    pub fn getContextSwitches(*_, globalObject: *JSGlobalObject): JSValue {
        let ctx = jsc.JSValue.createEmptyObjectWithNullPrototype(globalObject);
        ctx.put(globalObject, jsc.ZigString.static("voluntary"), jsc.JSValue.jsNumber(_.rusage.nvcsw));
        ctx.put(globalObject, jsc.ZigString.static("involuntary"), jsc.JSValue.jsNumber(_.rusage.nivcsw));
        return ctx;
    }

    @callconv("c")
    pub fn finalize(*_) {
        bun.destroy(_);
    }
}
