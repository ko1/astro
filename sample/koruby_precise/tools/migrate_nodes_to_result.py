#!/usr/bin/env python3
"""Mass-convert NODE_DEF bodies in node.def from VALUE-return to RESULT-return.

Transformations applied inside each NODE_DEF body:

1. `return X;` (where X is a VALUE expression)
       → `return RESULT_OK(X);`
2. Already RESULT-returning helpers are NOT wrapped.  Calls into the following
   helpers ARE recognized and left alone:
     korb_raise, korb_raise_type_error, korb_funcall_r, korb_yield_r,
     korb_xxx_r (anything ending _r)
3. `DROP_RESULT(korb_raise(...));\n    return Qnil;`
       → `return korb_raise(...);`

Body detection: NODE_DEF<\s*[a-zA-Z_]+>?\s*\n<fn-sig>\s*\n{\s*BODY\s*}.  We
locate matching braces and operate only inside the outermost `{...}` of each
NODE_DEF.
"""
import re
import sys

NODE_DEF_RE = re.compile(r'^NODE_DEF(\s+@\w+)*\s*\n([\w_]+)\s*\(', re.MULTILINE)

# Calls that already return RESULT — don't wrap return-value with RESULT_OK.
RESULT_HELPERS = {
    'korb_raise', 'korb_raise_type_error',
    'korb_funcall_r', 'korb_yield_r',
    'RESULT_OK', 'RESULT_RAISE_R',
    'EVAL_node_super', 'EVAL_node_super_block',
    'EVAL_node_super_forward', 'EVAL_node_super_forward_block',
}

def starts_with_result_helper(expr):
    """Return True if `expr` (without trailing `;`) is a call to a RESULT-returning helper."""
    s = expr.strip()
    # Match identifier followed by `(`
    m = re.match(r'([a-zA-Z_][a-zA-Z_0-9]*)\s*\(', s)
    if not m:
        return False
    name = m.group(1)
    if name in RESULT_HELPERS:
        return True
    if name.endswith('_r'):
        return True
    return False

def find_node_def_bodies(text):
    """Yield (start_idx, end_idx) for each NODE_DEF body's outermost `{...}`."""
    for m in NODE_DEF_RE.finditer(text):
        # From m.end(), scan forward to find the matching `{` opening the body.
        # The function signature is `name(args) {`; find the `{`.
        i = m.end()
        depth = 1  # we're inside the `(` opened just before m.end().
        # Find the matching `)`.
        while i < len(text) and depth > 0:
            c = text[i]
            if c == '(':
                depth += 1
            elif c == ')':
                depth -= 1
            i += 1
        # Now i points just after the matching `)`.  Skip whitespace.
        while i < len(text) and text[i] in ' \t\n':
            i += 1
        if i >= len(text) or text[i] != '{':
            continue
        body_start = i + 1
        depth = 1
        j = body_start
        while j < len(text) and depth > 0:
            c = text[j]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
            j += 1
        body_end = j - 1
        yield (body_start, body_end)

def strip_comments_and_strings(body):
    """Return a string-mask of the body where comment / string chars are
    replaced with spaces (preserving offsets).  Caller can use this to find
    keywords like `return` without false-positive matches inside comments
    and strings."""
    out = list(body)
    i = 0
    n = len(body)
    while i < n:
        c = body[i]
        if c == '/' and i + 1 < n and body[i+1] == '*':
            # Block comment.
            j = i + 2
            while j + 1 < n and not (body[j] == '*' and body[j+1] == '/'):
                out[j] = ' '
                j += 1
            out[i] = out[i+1] = ' '
            if j + 1 < n:
                out[j] = out[j+1] = ' '
            i = j + 2
            continue
        if c == '/' and i + 1 < n and body[i+1] == '/':
            j = i
            while j < n and body[j] != '\n':
                out[j] = ' '
                j += 1
            i = j
            continue
        if c == '"':
            j = i + 1
            out[i] = ' '
            while j < n and body[j] != '"':
                if body[j] == '\\' and j + 1 < n:
                    out[j] = ' '
                    out[j+1] = ' '
                    j += 2
                    continue
                out[j] = ' '
                j += 1
            if j < n:
                out[j] = ' '
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            out[i] = ' '
            while j < n and body[j] != "'":
                if body[j] == '\\' and j + 1 < n:
                    out[j] = ' '
                    out[j+1] = ' '
                    j += 2
                    continue
                out[j] = ' '
                j += 1
            if j < n:
                out[j] = ' '
            i = j + 1
            continue
        i += 1
    return ''.join(out)

def find_return_statements(body):
    """Yield (start, end, value_expr) for each top-level `return X;` in body.

    Uses a mask that strips comments / strings to prevent matching inside
    them.  Within parens/braces we don't break on `;`.
    """
    mask = strip_comments_and_strings(body)
    i = 0
    while i < len(mask):
        m = re.match(r'\breturn\s+', mask[i:])
        if not m:
            i += 1
            continue
        ret_start = i
        expr_start = i + m.end()
        depth_paren = 0
        depth_brace = 0
        j = expr_start
        while j < len(mask):
            c = mask[j]
            if c == '(':
                depth_paren += 1
            elif c == ')':
                depth_paren -= 1
            elif c == '{':
                depth_brace += 1
            elif c == '}':
                depth_brace -= 1
            elif c == ';' and depth_paren == 0 and depth_brace == 0:
                break
            j += 1
        if j >= len(mask):
            break
        expr_end = j  # points at ';'
        expr = body[expr_start:expr_end].strip()
        yield (ret_start, expr_end + 1, expr)
        i = expr_end + 1

def transform_body(body):
    """Transform a NODE_DEF body: wrap returns with RESULT_OK as needed."""
    # Collect all return statements.
    rets = list(find_return_statements(body))
    # Apply replacements from end to start so offsets stay valid.
    out = list(body)
    for start, end, expr in reversed(rets):
        # Skip if expression is already a call to a RESULT-returning helper.
        if starts_with_result_helper(expr):
            continue
        new_text = f'return RESULT_OK({expr});'
        out[start:end] = list(new_text)
    return ''.join(out)

def collapse_drop_result_raise(text):
    """Transform `DROP_RESULT(korb_raise(...));\n<ws>return Qnil;` →
    `return korb_raise(...);`."""
    pattern = re.compile(
        r'DROP_RESULT\((korb_raise(?:_[a-z_]+)?\([^;]*?\))\);\s*\n\s*return\s+RESULT_OK\(Qnil\);',
        re.DOTALL
    )
    return pattern.sub(r'return \1;', text)

def main():
    src_path = sys.argv[1] if len(sys.argv) > 1 else 'node.def'
    with open(src_path) as f:
        text = f.read()

    # Walk NODE_DEF bodies in reverse order so offsets stay valid.
    bodies = list(find_node_def_bodies(text))
    out = list(text)
    for start, end in reversed(bodies):
        body = text[start:end]
        new_body = transform_body(body)
        out[start:end] = list(new_body)
    text = ''.join(out)
    # Now do the DROP_RESULT collapse.
    text = collapse_drop_result_raise(text)

    with open(src_path, 'w') as f:
        f.write(text)

if __name__ == '__main__':
    main()
