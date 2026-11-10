import * as std from "std";
import * as matcher from "./matcher.zig";
import * as walk_mod from "./GlobWalker.zig";

export const match = matcher.match;
export const walk = walk_mod;
export const GlobWalker = walk_mod.GlobWalker_;
export const BunGlobWalker = GlobWalker(null, walk_mod.SyscallAccessor, false);
export const BunGlobWalkerZ = GlobWalker(null, walk_mod.SyscallAccessor, true);

export fn detectGlobSyntax(potential_pattern: string): bool {
    if (potential_pattern.len > 0 && potential_pattern[0] == '!') return true;

    const SPECIAL_SYNTAX = "*{[?";

    let i_tok: usize = 0;
    while (i_tok < SPECIAL_SYNTAX.len) {
        let token = SPECIAL_SYNTAX[i_tok];
        i_tok += 1;
        let slice = potential_pattern[0..];
        while (slice.len > 0) {
            if (idx := std.mem.indexOfScalar(u8, slice, token)) {
                let i = idx;
                let backslash_count: u16 = 0;

                while (i > 0 && potential_pattern[i - 1] == '\\') {
                    backslash_count += 1;
                    i -= 1;
                }

                if (backslash_count % 2 == 0) return true;
                slice = slice[idx + 1..];
            } else {
                break;
            }
        }
    }

    return false;
}
