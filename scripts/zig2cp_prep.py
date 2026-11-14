#!/usr/bin/env python3
"""
Fast deterministic Zig -> CP pre-transpiler for Bun migration (v2, new cpc rules).
Usage: python3 scripts/zig2cp_prep.py <file1.zig> [file2.zig ...]
1. Backs up <stem>.zig to <stem>_.zig if <stem>_.zig does not exist yet.
2. Reads <stem>_.zig and writes an initial <stem>.cp with the mechanical conversions below.

Rules implemented (see CP_PITFALLS.md):
  - no `self`/`this` receiver: methods use `*_` (mutable) or omit (read-only); body `this.`->`_.`
    (top-level fns keep a plain param, renamed `client`)
  - `fn f(..) T {` -> `fn f(..): T {`; `: void` omitted; bare `!void` -> `)!`
  - var->let, `_ = x;` -> `* = x;`, `?T` -> `T?`, `anytype` -> `auto`, `@is_beam` -> `@is_nest`
  - `|capture|` -> `if (v := e)` / `while (v := e)` / `for (v of a)` (`*v` for mutable)
  - `try e;` -> `e?;`, `and/or/orelse` -> `&& / || / ??`, `&.{}` -> `[]`
  - top-level `const X = struct {` -> `pub struct X {` (+ `static fn`, drop inner `pub fn`)
  - `const E = error{A,B};` -> `err E { A, B }`
  - delete `const std = @import("std")`, `@import` -> `import x from "..."`
  - escape CP keywords used as fields (`.arena` -> `.@"arena"`), rename `err` -> `e`
Anything not handled is left for `cpc_brief.py` + manual edit_file.
"""
import re
import shutil
import sys
from pathlib import Path

ID_KW = ["arena", "in", "as", "from", "impl", "meta", "macro"]
CP_KW_FIELDS = ["in", "as", "let", "import", "arena", "from", "impl", "meta", "macro", "type"]


# ---------- helpers: apply a transform only to code outside strings/comments ----------
def split_code(line):
    """Yield (is_code, text) segments of a line: code vs string literal / // comment."""
    out, i, n, buf = [], 0, len(line), ""
    while i < n:
        c = line[i]
        if c == "/" and line[i:i + 2] == "//":
            if buf:
                out.append((True, buf))
            out.append((False, line[i:]))
            return out
        if c == '"':
            if buf:
                out.append((True, buf))
                buf = ""
            j = i + 1
            while j < n and line[j] != '"':
                j += 2 if line[j] == "\\" else 1
            out.append((False, line[i:j + 1]))
            i = j + 1
            continue
        if c == "'" and i + 2 < n:
            j = line.find("'", i + 2 if line[i + 1] == "\\" else i + 2)
            if j != -1 and j - i <= 4:
                if buf:
                    out.append((True, buf))
                    buf = ""
                out.append((False, line[i:j + 1]))
                i = j + 1
                continue
        buf += c
        i += 1
    if buf:
        out.append((True, buf))
    return out


def on_code(line, fn):
    return "".join(fn(t) if is_code else t for is_code, t in split_code(line))


def match_paren(s, start):
    """index of ')' matching '(' at s[start], ignoring strings; -1 if not on this line."""
    depth = 0
    for is_code_idx, (is_code, t) in enumerate(split_code(s)):
        pass
    depth, i, in_str = 0, start, False
    while i < len(s):
        c = s[i]
        if in_str:
            if c == "\\":
                i += 1
            elif c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


# ---------- per-segment code transforms ----------
def code_tx(t):
    t = re.sub(r"\band\b", "&&", t)
    t = re.sub(r"\bor\b", "||", t)
    t = re.sub(r"\borelse\b", "??", t)
    t = t.replace("?*const anyopaque", "*const !?").replace("*const anyopaque", "*const !")
    t = t.replace("?*anyopaque", "*!?").replace("*anyopaque", "*!")
    t = re.sub(r'\.@"([0-9]+)"', r"[\1]", t)
    t = t.replace("&.{}", "[]")
    t = re.sub(r"\.\{\s*\}", "{}", t)
    t = re.sub(r"\banytype\b", "auto", t)
    t = t.replace("@is_beam", "@is_nest")
    # prefix optional -> postfix
    t = re.sub(r"(?<![\w.)\]])\?(\*(?:const\s+)?[A-Za-z_][\w.]*)", r"(\1)?", t)
    t = re.sub(r"(?<![\w.)\]])\?(\[\](?:const\s+)?[A-Za-z_][\w.]*)", r"(\1)?", t)
    t = re.sub(r"(?<![\w.)\]])\?([A-Za-z_][\w.]*)", r"\1?", t)
    # rename `err` identifiers (not fields / enum tags)
    t = re.sub(r'(?<![.\w@"])err(?![\w"])', "e", t)
    # CP keyword fields / identifiers
    for kw in CP_KW_FIELDS:
        t = re.sub(r"(?<=\.)%s\b(?!\")" % kw, '@"%s"' % kw, t)
    # `arena` as local -> arena_ ; as field decl/literal key (followed by ':') -> @"arena"
    t = re.sub(r'(?<![\w@".])arena(?=\s*:(?!:))', '@"arena"', t)
    t = re.sub(r'(?<![\w@".])arena(?![\w"])', 'arena_', t)
    for kw in ID_KW:
        if kw == "arena":
            continue
        t = re.sub(r'(?<![\w@".])%s(?![\w"])' % kw, '@"%s"' % kw, t)
    t = re.sub(r"^(\s*)(static|in|as|from|impl|meta|macro)(\s*[,=])", r'\1@"\2"\3', t)
    # single-line struct literal `.{ .a = 1, .b = 2 }` -> `{ a: 1, b: 2 }`
    if re.search(r"\.\{\s*\.\w+\s*=", t):
        t = re.sub(r"\.\{(\s*)\.(\w+)\s*=\s*", r"{\1\2: ", t)
        t = re.sub(r",(\s*)\.(\w+)\s*=(?!>)\s*", r",\1\2: ", t)
    return t


def fix_capture(line):
    """if/while/for with trailing |capture| on one line."""
    m = re.match(r"^(\s*)((?:\}\s*else\s+)?)(if|while|for)\s*\(", line)
    if not m:
        return line
    open_idx = line.index("(", m.end(3))
    close_idx = match_paren(line, open_idx)
    if close_idx < 0:
        return line
    rest = line[close_idx + 1:]
    cm = re.match(r"^\s*\|\s*([^|]+?)\s*\|(\s*.*)$", rest)
    if not cm:
        return line
    cap, tail = cm.group(1), cm.group(2)
    inner = line[open_idx + 1:close_idx].strip()
    kind = m.group(3)
    pre = line[:m.start(3)]
    if inner.startswith("try "):
        inner = inner[4:].strip() + "?"
    if kind in ("if", "while"):
        names = [c.strip() for c in cap.split(",")]
        if len(names) != 1:
            return line
        return "%s%s (%s := %s)%s" % (pre, kind, names[0], inner, tail)
    # for
    names = [c.strip() for c in cap.split(",")]
    srcs = [s.strip() for s in inner.split(",")]
    if len(names) == 1 and len(srcs) == 1:
        n = names[0]
        return "%sfor (%s of %s)%s" % (pre, n, srcs[0], tail)
    return line  # indexed/multi-capture for: manual (use `for (i of 0..n)` + index)


FN_RE = re.compile(r"^(\s*)((?:pub\s+|export\s+|inline\s+|extern\s+)*)fn\s+(@?\"?[\w.]+\"?)\s*\(")


def fix_return_type(line):
    """Insert ':' before return type / drop void on a line that closes a fn signature."""
    # `) RET {` or `) RET;`  (also line starting with ')')
    m = re.match(r"^(.*\))\s+([^\s{;=:][^{;=]*?)\s*(\{|;)\s*$", line)
    if not m:
        return line
    head, ret, end = m.groups()
    ret = ret.strip()
    if ret.startswith(("callconv", "align", "linksection", "addrspace")):
        return line
    if ret == "void":
        return "%s %s" % (head, end)
    if ret == "!void":
        return "%s! %s" % (head, end)
    return "%s: %s %s" % (head, ret, end)


def convert_fn_line(line, state, indent_ctx):
    """Handle a fn signature line. Returns new line."""
    m = FN_RE.match(line)
    if not m:
        return line
    indent = len(m.group(1))
    # receiver param?
    rm = re.search(r"\(\s*(?:comptime\s+)?(this|self|\w+_this)\s*:\s*(\*const\s+|\*)?([\w.]+)\s*(,|\))", line)
    receiver = None
    if rm and rm.group(1) in ("this", "self") and indent > 0:
        receiver = "mut" if (rm.group(2) or "").strip() == "*" else "ro"
        repl = "*_" if receiver == "mut" else ""
        if rm.group(4) == ",":
            line = line[:rm.start()] + "(" + (repl + ", " if repl else "") + line[rm.end():].lstrip()
        else:
            line = line[:rm.start()] + "(" + repl + ")" + line[rm.end():]
    elif rm and rm.group(1) in ("this", "self"):
        # top-level function: keep a plain parameter named `client`
        line = re.sub(r"\((\s*)(this|self)\s*:", r"(\1client:", line, count=1)
        state["rename_top"] = (indent, rm.group(1))
    state["fn_indent"] = indent
    state["receiver"] = receiver
    return line


def convert(src):
    lines = src.split("\n")
    for i in range(len(lines) - 1):
        if re.search(r"\.\{\s*$", lines[i]) and re.match(r"^\s*\.\w+\s*=(?!>)", lines[i + 1]):
            lines[i] = re.sub(r"\.\{\s*$", "{", lines[i])
    root_name = None
    for ln in lines:
        rm0 = re.match(r"^const\s+(\w+)\s*=\s*@This\(\);\s*$", ln)
        if rm0:
            root_name = rm0.group(1)
            break
    imports = []
    out = []
    state = {"fn_indent": None, "receiver": None, "rename_top": None}
    container_stack = []  # (indent, kind) for converted top-level struct/enum
    in_sig = False
    for raw in lines:
        line = raw
        stripped = line.strip()

        # imports
        if re.match(r'^\s*(?:pub\s+)?const\s+std\s*=\s*@import\("std"\);\s*$', line):
            continue
        m = re.match(r'^(?:pub\s+)?const\s+(\w+)\s*=\s*@import\("([^"]+)"\);\s*$', line)
        if m:
            (imports if root_name else out).append('import %s from "%s";' % m.groups())
            continue
        if root_name and re.match(r"^const\s+%s\s*=\s*@This\(\);\s*$" % root_name, line):
            continue

        # top-level container conversion
        m = re.match(r"^(pub\s+)?const\s+(\w+)\s*=\s*(struct|enum)\s*\{\s*$", line)
        if m:
            out.append("%s%s %s {" % (m.group(1) or "", m.group(3), m.group(2)))
            container_stack.append((0, m.group(3)))
            continue
        m = re.match(r"^(pub\s+)?const\s+(\w+)\s*=\s*error\s*\{([^}]*)\}\s*;\s*$", line)
        if m:
            names = ", ".join(x.strip() for x in m.group(3).split(",") if x.strip())
            out.append("%serr %s { %s }" % (m.group(1) or "", m.group(2), names))
            continue
        if container_stack and re.match(r"^\};\s*$", line):
            container_stack.pop()
            out.append("}")
            continue

        # leave multi-line strings alone
        if stripped.startswith("\\\\"):
            out.append(line)
            continue

        # var -> let / `_ = x` -> `* = x`
        line = re.sub(r"^(\s*)var\b", r"\1let", line)
        line = re.sub(r"^(\s*)_ = ", r"\1* = ", line)
        line = re.sub(r"\bthreadlocal\s+(let|const)\b", r"@tls \1", line)
        line = re.sub(r"\bpub\s+@tls\b", "@tls pub", line)
        # struct literal field init at line start: `.x = v,` -> `x: v,` (escape CP keywords)
        line = re.sub(r"^(\s*)\.([A-Za-z_]\w*)\s*=(?!>)\s*",
                      lambda m: "%s%s: " % (m.group(1), '@"%s"' % m.group(2) if m.group(2) in CP_KW_FIELDS + ID_KW else m.group(2)), line)
        line = on_code(line, code_tx)

        # body renames (this/self -> _ or client)
        if state["fn_indent"] is not None and not FN_RE.match(line):
            if state["receiver"] is not None:
                line = on_code(line, lambda t: re.sub(r"\b(this|self)\b", "_", t))
            elif state["rename_top"]:
                nm = state["rename_top"][1]
                line = on_code(line, lambda t: re.sub(r"\b%s\b" % nm, "client", t))

        # `try x;` statement-level
        tm = re.match(r"^(\s*(?:return\s+|(?:const|let)\s+\w+(?:\s*:\s*[^=]+)?\s*=\s*|[\w.\[\]]+\s*=\s*)?)try\s+([^;]+?);\s*$", line)
        if tm and "try " not in tm.group(2) and "catch" not in tm.group(2):
            line = "%s%s?;" % (tm.group(1), tm.group(2))

        # captures
        line = fix_capture(line)

        # fn signatures
        if FN_RE.match(line):
            line = convert_fn_line(line, state, None)
            if container_stack and re.match(r"^    ", line):
                is_static = state["receiver"] is None and not re.search(r"\(\s*\*?_", line)
                line = re.sub(r"^(\s+)pub\s+fn\b", r"\1fn", line) if state["receiver"] is not None or True else line
                if is_static and container_stack[-1][1] in ("struct", "enum"):
                    line = re.sub(r"^(\s+)fn\b", r"\1static fn", line)
            in_sig = line.count("(") > line.count(")")
            state["_sig_open"] = in_sig
            line = fix_return_type(line) if not in_sig else line
        elif in_sig:
            # continuation line(s) of a multi-line signature
            if re.match(r"^\s*\)", line):
                line = fix_return_type(line)
                in_sig = False
            elif state.get("receiver") is None and state.get("rename_top") is None:
                pass
            # receiver on its own line
            rm = re.match(r"^(\s*)(this|self)\s*:\s*(\*const\s+|\*)?[\w.]+,\s*$", line)
            if rm and state["fn_indent"]:
                mut = (rm.group(3) or "").strip() == "*"
                state["receiver"] = "mut" if mut else "ro"
                line = (rm.group(1) + "*_,") if mut else None
                if line is None:
                    continue

        # end of fn body
        if state["fn_indent"] is not None and re.match(r"^%s\}\s*$" % (" " * state["fn_indent"]), line) and not in_sig:
            state["fn_indent"] = None
            state["receiver"] = None
            state["rename_top"] = None

        out.append(line)
    if root_name:
        out = imports + ["@root struct %s {" % root_name] + out + ["}"]
    return "\n".join(out)


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
        cp.write_text(convert(content), encoding="utf-8")
        print(f"[PREP] {source_file} -> {cp} ({len(content.splitlines())} lines)")


if __name__ == "__main__":
    main()
