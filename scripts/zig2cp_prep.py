#!/usr/bin/env python3
"""
Fast deterministic Zig -> CP pre-transpiler for Bun migration.
Usage: python3 scripts/zig2cp_prep.py <file1.zig> [file2.zig ...]
1. Backs up <stem>.zig to <stem>_.zig if <stem>_.zig does not exist yet.
2. Reads <stem>_.zig and writes an initial <stem>.cp with ~80% mechanical Zig->CP conversions done.
"""
import re
import shutil
import sys
from pathlib import Path

CP_KEYWORDS_AS_FIELDS = {
    "in", "as", "let", "import", "arena", "from", "impl", "meta", "macro", "type"
}

def pre_transpile(src: str) -> str:
    # 1. Top-level @import("std") / @import("bun") / @import("./...")
    src = re.sub(
        r'^(?:pub\s+)?const\s+([A-Za-z0-9_]+)\s*=\s*@import\("([^"]+)"\);',
        r'import \1 from "\2";',
        src,
        flags=re.MULTILINE,
    )
    # 2. Replace var with let at statement start
    src = re.sub(r'^(\s*)var\b', r'\1let', src, flags=re.MULTILINE)
    # 3. Replace threadlocal let/const with @tls
    src = re.sub(r'\bpub\s+threadlocal\s+(let|const)\b', r'@tls pub \1', src)
    src = re.sub(r'\bthreadlocal\s+(let|const)\b', r'@tls \1', src)
    # 4. Replace and / or / orelse
    src = re.sub(r'\band\b', '&&', src)
    src = re.sub(r'\bor\b', '||', src)
    src = re.sub(r'\borelse\b', '??', src)
    # 5. Replace *anyopaque / ?*anyopaque
    src = src.replace('?*anyopaque', '*!?').replace('*anyopaque', '*!')
    src = src.replace('?*const anyopaque', '*const !?').replace('*const anyopaque', '*const !')
    # 6. Replace tuple field access .@"0", .@"1", etc.
    src = re.sub(r'\.@"([0-9]+)"', r'[\1]', src)
    # 7. Rename catch |err| -> catch |e|
    src = re.sub(r'\bcatch\s*\|\s*err\s*\|', 'catch |e|', src)
    # 8. Struct field initialization: .{ .field = val } -> { field: val } (simple single-line cases)
    src = re.sub(r'\.\{\s*\}', '{}', src)
    src = re.sub(r'^(\s*)\.([A-Za-z_][A-Za-z0-9_]*)\s*=(?!>)\s*', r'\1\2: ', src, flags=re.MULTILINE)
    return src

def main():
    for arg in sys.argv[1:]:
        p = Path(arg)
        if p.name.endswith("_.zig"):
            continue
        if p.suffix == ".cp":
            p = p.with_suffix(".zig")
        bak = p.with_name(p.stem + "_.zig")
        cp = p.with_suffix(".cp")
        if not bak.exists() and p.exists():
            shutil.copy2(p, bak)
        source_file = bak if bak.exists() else p
        content = source_file.read_text(encoding="utf-8")
        cp.write_text(pre_transpile(content), encoding="utf-8")
        print(f"[PREP] {source_file} -> {cp} ({len(content.splitlines())} lines)")

if __name__ == "__main__":
    main()
