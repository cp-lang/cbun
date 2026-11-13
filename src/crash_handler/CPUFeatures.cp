import bun from "bun";
import std from "std";
import builtin from "builtin";

extern fn bun_cpu_features(): u8;

@packed struct X86Flags {
    none: bool;
    sse42: bool;
    popcnt: bool;
    avx: bool;
    avx2: bool;
    avx512: bool;
    padding: u2 = 0;
}

@packed struct Aarch64Flags {
    none: bool;
    neon: bool;
    fp: bool;
    aes: bool;
    crc32: bool;
    atomics: bool;
    sve: bool;
    padding: u1 = 0;
}

@root struct CPUFeatures {
    flags: Flags;

    pub const Flags = switch (builtin.cpu.arch) {
        .x86_64 => X86Flags,
        .aarch64 => Aarch64Flags,
        else => unreachable,
    };

    pub fn format(this, writer: *std.Io.Writer)! {
        let is_first = true;
        inline for (field of @typeInfo(Flags).@"struct".fields) {
            if (!comptime (bun.strings.eql(field.name, "padding") || bun.strings.eql(field.name, "none"))) {
                if (@field(this.flags, field.name)) {
                    if (!is_first) {
                        writer.writeAll(" ")?;
                    }
                    is_first = false;
                    writer.writeAll(field.name)?;
                }
            }
        }
    }

    pub fn isEmpty(this): bool {
        return @as(u8, @bitCast(this.flags)) == 0;
    }

    pub fn hasAnyAVX(this): bool {
        return this.flags.avx || this.flags.avx2 || this.flags.avx512;
    }

    pub static fn get(): CPUFeatures {
        const flags: Flags = @bitCast(unsafe { bun_cpu_features() });
        bun.debugAssert(flags.none == false && flags.padding == 0);

        if (bun.Environment.isX64) {
            bun.analytics.Features.no_avx += @as(usize, @intFromBool(!flags.avx));
            bun.analytics.Features.no_avx2 += @as(usize, @intFromBool(!flags.avx2));
        }

        return CPUFeatures{ flags: flags };
    }
}
