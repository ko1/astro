# astrogre performance notes

This is the v1 sample.  ASTro's specialization / code-store machinery
is wired up via the abruby-style modes
(`--aot-compile` / cached / `--pg-compile` / `--plain`); the
search loop itself is now an AST node (`node_grep_search`) so
specialization fuses the for-each-start-position loop and the
inlined regex chain into one SD function.  This doc records what
*did* and *did not* help on the way to the current numbers.

## Where AOT specialization actually wins

The literal-led patterns from the original microbench got eaten by
the memchr / memmem prefilter (next section) — those numbers are
no longer interesting because the prefilter handles them outside
the bake's reach.  The AOT win is now visible on the *opposite*
shape: long node chain, no fixed-byte first prefix (so prefilter
doesn't fire), enough per-position work that dispatch overhead is
a meaningful share.

`bench/aot_bench.sh` runs the in-engine `--bench-file` path (whole
118 MB corpus loaded once, full-sweep count of all matches per
iter) and compares against grep / ripgrep / Onigmo via their
respective `-c` modes.  All times in milliseconds, best-of-3:

```
                                              interp     aot      aot/I  +onigmo  aot/O   grep  ripgrep
/(QQQ|RRR)+\d+/                                2341    1053     2.22×    696    1.51×    79      24
/(QQQX|RRRX|SSSX)+/                            3052     998     3.06×    720    1.39×    28      27
/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/             854     377     2.26×    777    0.49× ★ 597     215
/[a-z][0-9][a-z][0-9][a-z]/                     909     404     2.25×    808    0.50× ★   5     225
/(\d+\.\d+\.\d+\.\d+)/                         1557    1101     1.42×    755    1.46×     5      50
/[A-Z]{50,}/                                   1285    1182     1.09×   1111    1.06×  1535     187
/\b(if|else|for|while|return)\b/               1655     404     4.10×   1078    0.37× ★   2     119
/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/            20413   15583     1.31×  10141    1.54×     3     221
```
★ = astrogre + AOT beats Onigmo.

The 2-3× AOT wins line up with the AOT specialisation thesis:

- **Chain has to be long.**  alt (`(QQQ|RRR)`) → rep → class /
  literal → ... — five or more nodes per match attempt means five+
  indirect calls bake removes.
- **Prefilter has to NOT fire.**  These patterns start with a class
  or alt, so memchr/memmem can't pre-skip; the chain runs at
  every position.
- **Search has to walk the whole input.**  All these patterns
  scan the entire 118 MB (failing patterns sweep with no exit;
  matching patterns count all hits).

`/[A-Z]{50,}/` stays at 1.09× even though it sweeps the whole
input — the chain is just rep_cont ↔ class, two nodes, so dispatch
was already cheap.

### vs Onigmo

The interesting cells are the three where astrogre + AOT beats
Onigmo:

- `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`: 377 ms vs 777 ms (**2.06×
  faster than Onigmo**) — 9 class checks per attempt, ASTro inlines
  all into one function while Onigmo's bytecode VM dispatches each.
- `/[a-z][0-9][a-z][0-9][a-z]/`: 404 ms vs 808 ms (**2.00× faster**)
  — same shape, 5 class checks.
- `/\b(if|else|for|while|return)\b/`: 404 ms vs 1078 ms (**2.67×
  faster**) — `\b` + alt-5; ASTro inlines the alt branches as
  conditional compares, Onigmo's VM walks them one at a time.

Onigmo wins on patterns where it has structural shortcuts our
chain doesn't:

- `/(QQQ|RRR)+\d+/`, `(QQQX|RRRX|SSSX)+`: probably an alternation-
  with-shared-prefix optimisation we don't do.
- `/(\d+\.\d+\.\d+\.\d+)/`: greedy-dot-with-anchored-ending heuristic.

### vs grep / ripgrep

ugrep / ripgrep stay an order of magnitude ahead on patterns where
their literal-prefix prefilter or lazy DFA fires
(`/[a-z][0-9][a-z][0-9][a-z]/` ← grep 5 ms because the leading
`[a-z]` collapses to a memchr-class scan).

But for patterns where prefilter can't apply, ASTro+AOT can be
**faster than grep**:

- `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`: astrogre+AOT 377 ms vs
  grep 597 ms — grep doesn't have a useful prefilter for this shape
  and falls back to PCRE-class matching per position; ASTro's
  inlined chain is just faster.
- `/[A-Z]{50,}/`: astrogre+AOT 1182 ms vs grep 1535 ms — ditto.

ripgrep stays consistently fast across all (lazy DFA + literal-
prefix prefilter), the engineering cost is much higher.

### Upper bound

The earlier microbench's `literal-tail /match/` 7.22× still
applies as the *upper bound* the bake can deliver (no prefilter
*and* no memcmp dominating per-iter), but for realistic regex
patterns 2-3× is the practical ceiling for AOT alone — and the
bake is enough to **leapfrog Onigmo** on chain-heavy patterns
even without dedicated alt / class scan optimisations.

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

## Prefilter nodes — closing the Onigmo gap

`node_grep_search_memchr` / `node_grep_search_memmem` (added after
the original fusion experiment) skip directly to candidate
positions when the pattern starts with a known fixed byte / multi-
byte literal.  Same architectural shape as `node_grep_search`: an
EVAL that does the algorithmic skip + a body operand that the
specialiser inlines for verification.

Bench, post-prefilter (118 MB corpus, line by line, best-of-5 s):

```
                          grep   ripgrep   +onigmo  interp  aot-cached
literal    /static/      0.002    0.037     0.240   0.285    0.287
rare                     0.039    0.022     0.200   0.269    0.267
anchored   /^static/     0.003    0.042     0.249   0.284    0.281
case-i     /VALUE/i      0.002    0.053     0.285   0.746    0.712
alt-3                    0.003    0.053     1.183   2.199    2.133
class-rep  /[0-9]{4,}/   0.002    0.059     0.749   1.384    1.392
ident-call               0.002    0.196     3.626   4.238    4.294
count -c   /static/      0.002    0.030     0.244   0.293    0.286
```

Compared to the prior interp (no prefilter), the prefilter-eligible
patterns drop **3-4×**:

```
                         prior interp  →  prefilter interp
literal /static/             0.934            0.285  (3.27×)
literal-rare                 1.004            0.269  (3.73×)
anchored /^static/           0.720            0.284  (2.54×)
count -c /static/            0.957            0.293  (3.27×)
```

For the patterns the prefilter handles, astrogre is now **within
20 %** of Onigmo (vs 2-4× behind without it).  The remaining gap is
process startup + per-line `getline` + CTX-init.  Patterns the
current prefilter doesn't trigger on (case-i, alt, class-led) are
unchanged — those need their own analysis-and-emit, all listed in
[`todo.md`](./todo.md).

aot-cached vs interp is now essentially noise.  That's expected —
the prefilter has eaten the dispatch chain bake was meant to remove.

## Why the grep CLI bench doesn't show the fusion gain

For comparison, the prior bench (before prefilter, after fusion):

```
                          grep   ripgrep   +onigmo  interp  aot-cached
literal    /static/      0.002    0.036     0.238   0.934    0.974
ident-call               0.002    0.192     3.500   3.990    3.938
```

aot-cached was essentially indistinguishable from interp because
each input line is ~36 bytes — the fused loop ran only ~36
iterations per call, short enough that the **per-call overhead**
(`CTX_struct` zero-init, getline buffering, fwrite of matched
lines) dominated the wall clock.  The in-engine microbench
amortizes that across 16 K-byte strings; grep doesn't.

The natural next step is to fold the **line iteration** into the
AST too — a `node_grep_lines(body, str, len)` whose EVAL walks
newlines and prints matched lines, all inside one SD function.
See [`todo.md`](./todo.md).

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
