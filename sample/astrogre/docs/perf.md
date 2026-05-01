# astrogre performance notes

This is the v1 sample.  ASTro's specialization / code-store machinery
is wired up via the abruby-style modes
(`--aot-compile` / cached / `--pg-compile` / `--plain`); the
search loop itself is now an AST node (`node_grep_search`) so
specialization fuses the for-each-start-position loop and the
inlined regex chain into one SD function.  This doc records what
*did* and *did not* help on the way to the current numbers.

## Where AOT specialization actually wins

After folding the search loop into the AST, the in-engine microbench
(`./astrogre --bench` — single 16 KiB string, repeated calls) shows
the fusion clearly:

```
                                         interp     aot-cached   speedup
literal-tail   /match/                   22.75 s       3.15 s     7.22×
literal-i      /MATCH/i                  15.65 s      15.22 s     1.03×
class-digit    /\d+/   (matches early)    0.009 s      0.009 s    1.00×
class-word     /\w+/   (matches late)    11.60 s      10.43 s     1.11×
alt-3          /cat|dog|match/           15.67 s      12.86 s     1.22×
rep-greedy     /a.*z/                    11.23 s      10.74 s     1.05×
group-alt      /(a|b|c)+m/  (no match)   21.53 s      21.99 s     0.98×
anchored       /\Amatch/                  0.24 s       0.25 s     0.96×
dot-star       /.*match/                 11.04 s       9.16 s     1.20×
```

`literal-tail` is the case where the fusion really lands: scanning
16 KiB for a 5-byte literal in a tight loop, with the inlined
chain reduced to a `cmpl + cmpw + ja` per position and the
capture-state reset hoisted to a single `vmovdqu`.  Other patterns
move 5-25 % depending on how much per-position work the regex chain
imposes — the more there is, the smaller the share that dispatch
overhead represented in the first place.

The disassembly of the fused SD for `/static/`:

```
SD_<hash>_INL:
    endbr64
    mov    8(%rdi),%rcx          ; c->str_len
    mov    0x10(%rdi),%rax       ; c->pos
    lea    1(%rcx),%rdx
    cmp    %rdx,%rax
    jae    66f                   ; pos >= len + 1 → exit
    ...
    vpxor  %xmm0,%xmm0,%xmm0     ; zero ymm0 for capture-state reset
.loop:
    lea    -6(%rax),%rdx
    mov    %rdx,0x10(%rdi)        ; c->pos = s
    vmovdqu %ymm0,(%rsi)         ; reset all 32 valid[] flags via SIMD
    ...
    cmp    %rax,%rcx
    jb     fail
    add    (%rdi),%rdx           ; str + pos
    cmpl   $0x74617473,(%rdx)    ; "stat"
    je     check_ic
    inc    %rax
    cmp    %rax,%r8
    jne    .loop
    ret_zero
check_ic:
    cmpw   $0x6369,4(%rdx)        ; "ic"
    jne    fail
    set ends[0], valid[0]; ret 1
```

That's the entire grep search — start-position loop + inlined regex
match — in ~30 instructions.  No indirect call, no DISPATCH chain,
not even a function call out of the SD.

## Why the grep CLI doesn't show the fusion gain

`bench/grep_bench.sh` runs each tool against a 118 MB corpus, line by
line:

```
                          grep   ripgrep   +onigmo  interp  aot-cached
literal    /static/      0.002    0.036     0.238   0.934    0.974
rare                     0.036    0.020     0.215   1.004    1.024
anchored   /^static/     0.003    0.041     0.358   0.720    0.672
case-i     /VALUE/i      0.002    0.050     0.278   0.773    0.784
alt-3                    0.003    0.055     1.199   2.122    2.348
class-rep  /[0-9]{4,}/   0.006    0.103     0.861   1.381    1.330
ident-call               0.002    0.192     3.500   3.990    3.938
count -c   /static/      0.002    0.029     0.241   0.957    0.933
```

aot-cached is essentially indistinguishable from interp here, and the
reason has nothing to do with the SD itself: each line is ~36 bytes
on average, so the fused loop runs only ~36 iterations per call —
short enough that the **per-call overhead** (`CTX_struct` zero-init,
file I/O, getline buffering, `fwrite` of matched lines) dominates the
wall clock.  The microbench amortizes that across 16 K-byte strings;
grep doesn't.

The natural next step is to fold the **line iteration** into the AST
too — a `node_grep_lines(body, str, len)` whose EVAL walks newlines
and prints matched lines, all inside one SD function.  See
[`todo.md`](./todo.md).

For an apples-to-apples comparison with Onigmo (also a backtracking
engine without a literal-prefix prefilter), astrogre+onigmo is
currently 2-4 × ahead of astrogre.  The remaining gap is the literal-
prefix prefilter — Onigmo runs Boyer-Moore over `static` even though
the wrapping pattern is "regex"-shaped.

## What helped

### Folding the search loop into the AST
`node_grep_search` is the wrapper that the parser puts at the top of
every compiled pattern.  Its EVAL is the for-each-start-position
loop; when the specializer recurses into its `body` operand and
inlines the `body_dispatcher` direct function pointer, gcc fuses the
loop and the regex chain into a single function with no indirect
calls.  That's where the 7.22× literal-tail speedup comes from.

### `_INL` rename + extern wrapper post-process
Borrowed wholesale from luastro's `luastro_export_sd_wrappers`.
Without it, `astro_cs_load` only sees the root SD; inner nodes' SDs
stay hidden behind `static inline` and the runtime chain bounces
through host-side `DISPATCH_*` for every per-node touch.

### Side-array re-resolve after build
`node_allocate` tracks every NODE in `astrogre_all_nodes`;
`astrogre_pattern_aot_compile` walks the array post-build and calls
`astro_cs_load` on each node so this very run picks up the fresh SDs
(otherwise only the *next* invocation benefits).

### Coalesce adjacent literals at parse time
Turns `\AHello, World/` from 13 lit nodes into 1 — fewer dispatches
and fewer SD entries to compile.

### Pre-fold case at parse time
`/Hello/i` becomes `node_re_lit_ci("hello", 5, ...)` — pattern is
lowercased once, matcher does an asymmetric fold of the input byte
during the byte-by-byte compare.

### Anchored-start short-circuit
`anchored_bos` is an operand on `node_grep_search` so the SD's
position loop knows statically when to stop after one iteration.

### Single rep_cont sentinel
Exactly one `node_re_rep_cont` allocation, shared by every rep node.
Keeps the AST acyclic so Merkle hashes survive and code-store
sharing applies.

### Backend abstraction
`backend.h` decouples the grep CLI from the matcher.  Adding the
Onigmo backend was ~150 lines of wrapper + a 60-line `build_local.mk`
that compiles Onigmo without the autoconf / libtool dance.

### `restrict` on `c` and `n`, 256-bit bitmap inline as `uint64_t × 4`
Standard ASTro hygiene.

## What did not help / was abandoned

### Per-rep "static" continuation node
Earlier draft allocated a `rep_cont` *per repeat node*.  Made the
AST cyclic, broke Merkle hashing.

### "Body returns one success per call" repetition model
Failed `(a|ab)*c` on `abc` — the inner alt has to be retryable
across iteration backtracks.

### UTF-8 multi-byte char-class
Tried storing class as a 256-bit ASCII bitmap *plus* a sorted list of
`(lo, hi)` codepoint ranges.  Binary search dominated dispatch.

### Storing `bytes` for class as `const char *`
Saves 24 bytes per node struct; matcher then has to load the pointer.
Inline won by ~12 % on `class-word`.

### Possessive quantifiers
Parsed but degraded to plain greedy.

### PG (profile-guided) bake
`--pg-compile` is accepted as a CLI flag for parity with abruby but
currently behaves as `--aot-compile` because we have no profile
signal.  A real signal for regex (hot-alternative reordering, hot
iteration counts, capture elision when never read) is on the runway.

## Things on the runway, not started

* **Line iteration in the AST.**  The biggest remaining lever for
  the grep CLI.  Add a `node_grep_lines(body, str, len, callback)`
  whose EVAL does newline scanning + per-line search dispatch + the
  print/count side-effect, all in one SD function.  Should drop the
  per-call overhead that currently masks the search-loop fusion at
  the grep level.
* **Literal-prefix prefilter.**  For unanchored patterns with a
  fixed-byte prefix, use Boyer-Moore-style scan to find candidate
  positions and verify with the AST.  Single biggest miss vs
  ripgrep.  Independent of the AST.
* **First-byte bitmap.**  Even simpler than full BMH: at compile
  time, build a 256-bit bitmap of allowed first bytes; skip ahead
  using a vectorised scan.
* **Real PG signal.**  Hot-alternative reordering: count branch
  hits at each `node_re_alt`, bake the hot one as the first branch.
  Capture elision: if no backreference uses a given group during
  the profile run, drop its save/restore.
* **JIT.**  The standard ASTro JIT path applies once we want to
  generate fresh SDs without an offline build step.

These are listed in [`todo.md`](./todo.md).
