import std from "std";
import Environment from "../bun_core/env.zig";

struct WasmTimer {
    pub static fn start(): WasmTimer! {
        return WasmTimer{};
    }

    pub fn read(*this): u64 {
        @compileError("FeatureFlags.tracing should be disabled in WASM");
    }

    pub fn lap(*this): u64 {
        @compileError("FeatureFlags.tracing should be disabled in WASM");
    }

    pub fn reset(*this): u64 {
        @compileError("FeatureFlags.tracing should be disabled in WASM");
    }
}

fn NewTimer(): type {
    if (Environment.isWasm) {
        return WasmTimer;
    }
    return std.time.Timer;
}

export const Timer = NewTimer();
