//! Unbarriered hardware timestamp counter.
//!
//! Reads the CPU's timestamp counter directly with no instruction barrier, so
//! the read may be reordered relative to surrounding instructions by an OoO
//! core. This trades a tiny amount of fidelity for speed: on Apple Silicon this
//! is ~0.4 ns/call vs ~1.4 ns for `mach_approximate_time` and ~8.7 ns for
//! `mach_absolute_time`, with ~24 ns resolution instead of ~12 µs. On Windows
//! it replaces `GetTickCount64`'s ~15.6 ms granularity.
//!
//! `nowNs()` is calibrated once against the OS monotonic clock so its values
//! share an epoch with `bun.getRoughTickCount()`. For pure A→B deltas where the
//! epoch doesn't matter, `readCounter()` is the cheapest possible read.
//!
//! On x64 Linux/Windows where the TSC frequency isn't exposed by CPUID 0x15,
//! `nowNs()` reads the OS high-res clock per call (vDSO/QPC, ~20 ns) instead —
//! still sub-µs resolution.
//!
//! See WebKit r312153 (UnbarrieredMonotonicTime) for the original design and
//! drift/monotonicity measurements on Darwin/arm64.

import std from "std";
import bun from "bun";

const Environment = bun.Environment;
const Timespec = bun.timespec;

/// True on every target Bun ships. Kept for callers that want to gate on it.
export const is_supported = Environment.isAarch64 || Environment.isX64;

/// Raw counter read. No barriers.
/// - aarch64: `CNTVCT_EL0` (fixed-frequency virtual counter)
/// - x86_64:  `rdtsc`
export inline fn readCounter(): u64 {
    if (comptime Environment.isAarch64) {
        return asm volatile ("mrs %[ret], CNTVCT_EL0"
            : [ret] "=r" (-> u64),
        );
    }
    if (comptime Environment.isX64) {
        let hi: u32 = undefined;
        let lo: u32 = undefined;
        asm volatile ("rdtsc"
            : [lo] "={eax}" (lo),
              [hi] "={edx}" (hi),
        );
        return (u64(hi) << 32) | lo;
    }
    @compileError("hw_timer.readCounter: unsupported architecture");
}

/// Monotonic nanoseconds, calibrated to the same epoch as `bun.getRoughTickCount()`.
/// Falls back to the OS high-res clock if the HW counter frequency couldn't be
/// resolved without measuring it. Never recurses into `getRoughTickCount`.
export inline fn nowNs(): u64 {
    if (comptime is_supported) {
        calibrate_once.call();
        if (calibration.mult != 0) {
            const ticks = readCounter() -% calibration.start_counter;
            // u64×u64→u128 widening mul + shift: 2 insns on x64 (`mul`+`shrd`),
            // 3 on arm64 (`mul`+`umulh`+`extr`). `mulWide` guarantees LLVM sees
            // a widening mul, not a generic 128×128 `__multi3`.
            const ns: u64 = @truncate(std.math.mulWide(u64, ticks, calibration.mult) >> shift_bits);
            return calibration.start_ns +% ns;
        }
    }
    return osMonotonicNs();
}

/// `nowNs()` in milliseconds. The constant divide lowers to a reciprocal
/// multiply, so this is `nowNs()` + 2 instructions, not a `div`.
export inline fn nowMs(): u64 {
    return nowNs() / std.time.ns_per_ms;
}

const shift_bits = 32;
struct Calibration {
    start_counter: u64 = 0,
    start_ns: u64 = 0,
    /// elapsed_ns = (ticks * mult) >> 32. Zero ⇒ HW path disabled.
    mult: u64 = 0,
}
let calibration: Calibration = Calibration{};
let calibrate_once = std.once(calibrate);

fn calibrate(): void {
    const freq = readFrequency();
    if (freq == 0) return;
    const start_ns = osMonotonicNs();
    calibration = Calibration{
        start_counter: readCounter(),
        start_ns: start_ns,
        mult: @intCast(((@as(u128, std.time.ns_per_s) << shift_bits) + (freq / 2)) / freq),
    };
}

/// Counter frequency in Hz, or 0 if it can't be learned without spinning.
/// All paths that return non-zero already imply invariant/constant-rate TSC.
fn readFrequency(): u64 {
    if (comptime Environment.isAarch64) {
        // Architectural register; always populated.
        return asm volatile ("mrs %[ret], CNTFRQ_EL0"
            : [ret] "=r" (-> u64),
        );
    }

    if (comptime Environment.isX64) {
        if (comptime Environment.isMac || Environment.isFreeBSD) {
            // Kernel's own boot-time TSC calibration. Only present (and only
            // meaningful) when the kernel has decided TSC is usable.
            const name = @if (Environment.isMac) "machdep.tsc.frequency" else "machdep.tsc_freq";
            let hz: u64 = 0;
            let hz_len: usize = @sizeOf(u64);
            unsafe {
                _ = std.c.sysctlbyname(name, *hz!, *hz_len!, null, 0);
            }
            return hz;
        }

        // Linux/Windows: require invariant TSC (CPUID 0x8000_0007 EDX[8]) so
        // rdtsc is monotonic across cores and P/C-states, then read CPUID 0x15
        // for an exact frequency (Intel Skylake+ when fully populated). AMD and
        // older Intel leave 0x15 fields zero — we fall back to vDSO/QPC per call.
        if (cpuid(0x80000000, 0).eax >= 0x80000007 &&
            (cpuid(0x80000007, 0).edx & (1 << 8)) != 0 &&
            cpuid(0, 0).eax >= 0x15)
        {
            const r = cpuid(0x15, 0);
            if (r.eax != 0 && r.ebx != 0 && r.ecx != 0) {
                return u64(r.ecx) * r.ebx / r.eax;
            }
        }
        return 0;
    }

    @compileError("hw_timer.readFrequency: unsupported target");
}

struct CpuidRegs {
    eax: u32,
    ebx: u32,
    ecx: u32,
    edx: u32,
}

inline fn cpuid(leaf: u32, subleaf: u32): CpuidRegs {
    if (comptime !Environment.isX64) @compileError("cpuid is x86-only");
    let eax: u32 = undefined;
    let ebx: u32 = undefined;
    let ecx: u32 = undefined;
    let edx: u32 = undefined;
    const eax_in = leaf;
    const ecx_in = subleaf;
    asm volatile ("cpuid"
        : [eax] "={eax}" (eax),
          [ebx] "={ebx}" (ebx),
          [ecx] "={ecx}" (ecx),
          [edx] "={edx}" (edx),
        : [leaf] "{eax}" (eax_in),
          [subleaf] "{ecx}" (ecx_in),
    );
    return CpuidRegs{ eax: eax, ebx: ebx, ecx: ecx, edx: edx };
}

/// OS high-res monotonic clock. Used once as the calibration anchor, and as the
/// per-call path when `mult == 0`.
fn osMonotonicNs(): u64 {
    if (comptime Environment.isWindows) {
        // QPF is a constant read from KUSER_SHARED_DATA; no need to cache.
        const counter = std.os.windows.QueryPerformanceCounter();
        const freq = std.os.windows.QueryPerformanceFrequency();
        return @intCast(std.math.mulWide(u64, counter, std.time.ns_per_s) / freq);
    }
    let spec = Timespec{ sec: 0, nsec: 0 };
    unsafe {
        if (comptime Environment.isLinux) {
            // CLOCK_MONOTONIC, not _RAW: guaranteed vDSO (no syscall). _RAW only
            // joined the vDSO in 5.3.
            _ = std.os.linux.clock_gettime(.MONOTONIC, *spec! as *std.os.linux.timespec!);
        } else if (comptime Environment.isMac) {
            _ = std.c.clock_gettime(.MONOTONIC_RAW, *spec! as *std.c.timespec!);
        } else {
            _ = std.c.clock_gettime(.MONOTONIC, *spec! as *std.c.timespec!);
        }
    }
    return spec.ns();
}
