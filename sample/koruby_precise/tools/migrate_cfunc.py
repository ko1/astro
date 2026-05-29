#!/usr/bin/env python3
"""Mechanically convert legacy VALUE cfuncs to RESULT cfunc_r ABI.

Pattern (input):
    static VALUE foo(CTX *c, VALUE self, int argc, VALUE *argv) {
        ...body...
        DROP_RESULT(korb_raise(...));
        return X;          # or `return Qnil;`
        ...
    }

Pattern (output):
    static RESULT foo(CTX *c, int argc, VALUE *sp) {
        c->sp = sp;
        VALUE self = sp[-argc - 1];
        VALUE *argv = sp - argc;
        ...body...
        return korb_raise(...);  # DROP_RESULT lifted to return; subsequent return X is dropped
        return RESULT_OK(X);
        ...
    }

Usage:
    ./migrate_cfunc.py builtins/range.c FUNC1 FUNC2 ...
    ./migrate_cfunc.py builtins/range.c --all      # all matching cfuncs in the file

Caveats:
- Does NOT convert helpers (non-cfunc functions returning VALUE).
- Does NOT rewrite calls to migrated functions; you must update builtins.c
  registrations from DEF -> DEF_R (and korb_class_add_method_cfunc ->
  korb_class_add_method_cfunc_r) yourself.
- Cannot inline-rewrite a `return korb_funcall(...)` form; flag manually.
- Skips functions that already have RESULT signature.
"""
import re
import sys
import os

HDR_RE = re.compile(
    r'^static VALUE (\w+)\(CTX \*c, VALUE self, int argc, VALUE \*argv\) \{',
    re.MULTILINE,
)


def find_func_body(text, start):
    """Given the position of '{' opening the function, return its end pos
    (position past the matching '}')."""
    depth = 0
    i = start
    in_str = False
    in_chr = False
    in_lc = False  # line comment
    in_bc = False  # block comment
    while i < len(text):
        ch = text[i]
        nx = text[i+1] if i+1 < len(text) else ''
        if in_lc:
            if ch == '\n':
                in_lc = False
            i += 1; continue
        if in_bc:
            if ch == '*' and nx == '/':
                in_bc = False; i += 2; continue
            i += 1; continue
        if in_str:
            if ch == '\\' and nx:
                i += 2; continue
            if ch == '"': in_str = False
            i += 1; continue
        if in_chr:
            if ch == '\\' and nx:
                i += 2; continue
            if ch == "'": in_chr = False
            i += 1; continue
        if ch == '/' and nx == '/':
            in_lc = True; i += 2; continue
        if ch == '/' and nx == '*':
            in_bc = True; i += 2; continue
        if ch == '"':
            in_str = True; i += 1; continue
        if ch == "'":
            in_chr = True; i += 1; continue
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise RuntimeError("Unbalanced braces")


def convert_body(body):
    """Rewrite a single cfunc body. body includes the opening { and closing }."""
    # 1. DROP_RESULT(korb_raise(...)); return X(?);
    #    -> return korb_raise(...);   (drop the subsequent return)
    # Need to balance parens on korb_raise to find the closing `)` of korb_raise.
    out = []
    i = 0
    while i < len(body):
        m = re.match(r'DROP_RESULT\(', body[i:])
        if m:
            # Find matching close paren of DROP_RESULT(...)
            j = i + m.end()
            depth = 1
            while j < len(body) and depth > 0:
                if body[j] == '(': depth += 1
                elif body[j] == ')': depth -= 1
                j += 1
            inner = body[i + m.end(): j - 1]
            # j is past the closing ')'. expect ';'
            k = j
            while k < len(body) and body[k] in ' \t':
                k += 1
            if k < len(body) and body[k] == ';':
                k += 1
            # Skip any subsequent `\s*return ...;` (the legacy dummy return after raise)
            tail = body[k:]
            m2 = re.match(r'(\s+)return [^;]*?;', tail)
            if m2:
                # Lift the indentation before the consumed return
                out.append(' ' * 0)  # placeholder
                # Determine current line's leading whitespace
                # Just replace with `return inner;`
                out.append('return ' + inner.strip() + ';')
                i = k + m2.end()
                continue
            else:
                out.append('return ' + inner.strip() + ';')
                i = k
                continue
        out.append(body[i])
        i += 1
    new_body = ''.join(out)

    # 2. Wrap remaining `return EXPR;` (not `return korb_raise(`, not `return UNWRAP(`,
    #    not `return RESULT_OK(`) with RESULT_OK(...).
    # Use a string-aware scan rather than regex so we don't cross
    # comments / string literals.
    out_parts = []
    i = 0
    n = len(new_body)
    while i < n:
        ch = new_body[i]
        nx = new_body[i+1] if i+1 < n else ''
        # Skip line comments
        if ch == '/' and nx == '/':
            j = new_body.find('\n', i)
            if j < 0: j = n
            out_parts.append(new_body[i:j])
            i = j; continue
        # Skip block comments
        if ch == '/' and nx == '*':
            j = new_body.find('*/', i+2)
            if j < 0: j = n
            else: j += 2
            out_parts.append(new_body[i:j])
            i = j; continue
        # Skip string literals
        if ch == '"':
            j = i + 1
            while j < n and new_body[j] != '"':
                if new_body[j] == '\\' and j+1 < n:
                    j += 2; continue
                j += 1
            j = min(j + 1, n)
            out_parts.append(new_body[i:j])
            i = j; continue
        # Skip char literals
        if ch == "'":
            j = i + 1
            while j < n and new_body[j] != "'":
                if new_body[j] == '\\' and j+1 < n:
                    j += 2; continue
                j += 1
            j = min(j + 1, n)
            out_parts.append(new_body[i:j])
            i = j; continue
        # Match `return ` only if preceded by whitespace or `{` (statement boundary)
        # and we're not in middle of an identifier.
        if (i == 0 or not (new_body[i-1].isalnum() or new_body[i-1] == '_')) \
           and new_body[i:i+7] == 'return ':
            # Find the matching ';' at the same paren/brace depth on the same statement
            j = i + 7
            depth_paren = 0; depth_brace = 0
            while j < n:
                cj = new_body[j]
                if cj == '"' or cj == "'":
                    # Skip the literal
                    qc = cj
                    j += 1
                    while j < n and new_body[j] != qc:
                        if new_body[j] == '\\' and j+1 < n:
                            j += 2; continue
                        j += 1
                    j = min(j + 1, n)
                    continue
                if cj == '/' and j+1 < n and new_body[j+1] == '/':
                    # line comment - end of statement actually but only if no ;
                    break
                if cj == '/' and j+1 < n and new_body[j+1] == '*':
                    end = new_body.find('*/', j+2)
                    j = end + 2 if end >= 0 else n
                    continue
                if cj == '(': depth_paren += 1
                elif cj == ')': depth_paren -= 1
                elif cj == '{': depth_brace += 1
                elif cj == '}': depth_brace -= 1
                elif cj == ';' and depth_paren == 0 and depth_brace == 0:
                    break
                elif cj == '\n' and depth_paren == 0 and depth_brace == 0:
                    # Multi-line return — bail (don't rewrite, output as-is)
                    out_parts.append(new_body[i])
                    i += 1
                    break
                j += 1
            else:
                out_parts.append(new_body[i])
                i += 1
                continue
            if j >= n or new_body[j] != ';':
                # Couldn't find ';' on same statement — emit char-by-char fallback
                out_parts.append(new_body[i])
                i += 1
                continue
            expr = new_body[i+7:j].strip()
            if (expr.startswith('RESULT_OK') or
                expr.startswith('korb_raise') or
                expr.startswith('UNWRAP') or
                expr.startswith('(RESULT)') or
                expr == '' or
                # Bare `return;` (void)
                False):
                out_parts.append(new_body[i:j+1])
            else:
                out_parts.append(f'return RESULT_OK({expr});')
            i = j + 1
            continue
        out_parts.append(ch)
        i += 1
    new_body = ''.join(out_parts)

    # Handle bare `return;` (shouldn't happen in VALUE func, but just in case)
    return new_body


def migrate_file(path, targets):
    with open(path) as f:
        text = f.read()
    changed = 0
    notes = []
    # Iterate in reverse so substitutions don't shift positions of remaining matches
    matches = list(HDR_RE.finditer(text))
    if not matches:
        return text, 0, notes
    # We'll build a list of (start, end, new_text) and apply in reverse
    edits = []
    for m in matches:
        name = m.group(1)
        if targets is not None and name not in targets:
            continue
        # Find '{' position - it's at m.end() - 1
        brace_open = m.end() - 1
        try:
            brace_close = find_func_body(text, brace_open)
        except RuntimeError as e:
            notes.append(f'{name}: skipped ({e})')
            continue
        body = text[brace_open: brace_close]
        # New signature
        new_sig = f'static RESULT {name}(CTX *c, int argc, VALUE *sp)'
        # Insert prologue inside the opening brace
        # Replace body[0] '{' with '{\n    c->sp = sp;\n    VALUE self = sp[-argc - 1];\n    VALUE *argv = sp - argc;\n'
        prologue = ('{\n'
                    '    c->sp = sp;\n'
                    '    VALUE self = sp[-argc - 1];\n'
                    '    VALUE *argv = sp - argc;\n')
        new_body_inner = convert_body(body[1:-1])  # excluding outer braces
        new_block = prologue + new_body_inner + '}'
        new_text = new_sig + ' ' + new_block
        edits.append((m.start(), brace_close, new_text))
        changed += 1
    # Apply in reverse
    for start, end, repl in sorted(edits, key=lambda x: -x[0]):
        text = text[:start] + repl + text[end:]
    return text, changed, notes


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    path = sys.argv[1]
    args = sys.argv[2:]
    targets = None if (args == ['--all']) else set(args) if args else None
    if not args:
        # default: list candidate functions
        with open(path) as f:
            text = f.read()
        for m in HDR_RE.finditer(text):
            print(m.group(1))
        return
    new_text, changed, notes = migrate_file(path, targets)
    if changed == 0:
        print('No matches converted', file=sys.stderr)
        sys.exit(2)
    with open(path, 'w') as f:
        f.write(new_text)
    print(f'Converted {changed} cfunc(s) in {path}')
    for n in notes:
        print('  note:', n)


if __name__ == '__main__':
    main()
