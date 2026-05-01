# astrogre — implemented

What's in v1, by category.  Companion to [`todo.md`](./todo.md).

## Engine — language surface

### Atoms
- Literal byte sequence.  Adjacent literals coalesce at parse time.
- `.` (dot).  Four variants: ASCII, ASCII-`/m`, UTF-8, UTF-8-`/m`.
- Character class `[...]`, negated `[^...]`.
- ASCII ranges in classes (`[a-z]`).
- Escaped char-class shortcuts inside `[...]`: `\d \D \w \W \s \S`.
- Numeric escapes `\xHH`, `\0`, `\n \t \r \f \v \a \e`.
- Anchors: `\A` `\z` `\Z` `^` `$` `\b` `\B`.
- Plain-literal escapes `\\ \/ \. \^ \$ \( \) \[ \] \{ \} \| \* \+ \? \-`.

### Quantifiers
- `*` `+` `?`, greedy and lazy (`*?` `+?` `??`).
- `{n}` / `{n,}` / `{n,m}`, greedy and lazy.
- Possessive (`*+` `++` `?+`) parsed; falls through to greedy.

### Groups
- Capturing groups `(...)`.
- Non-capturing `(?:...)`.
- Named capture `(?<name>...)` — captured by left-to-right index.
- Inline-flag groups `(?ixm-ixm:...)` and `(?ixm)`.
- Positive lookahead `(?=...)` and negative `(?!...)`.

### Backreferences
- `\1`–`\9`.

### Flags / encoding
- `/i` (ASCII case-fold).  `/m` (dot matches newline).  `/x` (extended).
  `/n` (ASCII byte mode).  `/u` (UTF-8 default).

### Front-end
- prism integration (`astrogre_parse_via_prism`): walks any Ruby
  source's AST, picks the first `PM_REGULAR_EXPRESSION_NODE`.
- `astrogre_parse_literal`: `/pat/flags` syntax for tests / CLI.
- `astrogre_parse_fixed`: `-F` mode, no regex parser.

## Engine — runtime

- AST in continuation-passing form: every match-node carries a
  `next` operand; chain ends in `node_re_succ`.
- Repetition via shared `node_re_rep_cont` sentinel + per-call
  `rep_frame` on `c->rep_top` (no AST cycles).
- Capture group 0 wrapped around the whole AST so the matcher records
  the overall match span the same way as user-numbered groups.
- `astrogre_search` and `astrogre_search_from` for caller-resumable
  enumeration of hits.

## Engine — performance work that landed

- Adjacent-literal coalescing at the IR level.
- Pre-folding pattern literals to lowercase under `/i`.
- Anchored-`\A` short-circuit in the search loop.
- Single global rep_cont sentinel.
- 4× `uint64_t` inline class bitmap.
- `restrict` on `CTX *` and `NODE *` parameters across `node.def`.

## Drivers / tooling

- **grep CLI**: `./astrogre PATTERN [FILE...]` with the standard
  options `-i -n -c -v -w -F -l -L -H -h -o -r -e --color=auto`.
  Recurses into directories with `-r`, skips dotfiles.  Multi-pattern
  support via repeated `-e PATTERN`.  Filename-only output (`-l`,
  `-L`).  `-o` prints just the matched span(s).
- **`--color`**: red on match, green on line numbers, magenta on
  filenames, matching GNU grep.  `--color=auto` honours `isatty`.
- **`--via-prism`**: parse the pattern argument as Ruby source via
  prism, take the first `/.../`'s body as the search pattern.
- **`--backend=astrogre|onigmo`**: switchable matcher backend at
  runtime.  Onigmo is built locally with `make WITH_ONIGMO=1` (a
  hand-rolled `build_local.mk` skips the autoconf / libtool dance).
- **`--self-test`** (44 cases) and **`--bench`** (in-engine
  microbench) preserved as flags.
- **`bench/grep_bench.sh`**: cross-tool comparison harness; runs
  grep / ripgrep / astrogre / astrogre+onigmo on the same corpus
  and patterns and reports best-of-N seconds per tool.

## Backend abstraction

- `backend.h` declares an ops table (`compile / search / search_from
  / free / aot_compile`) that the grep CLI talks to.
- `backend_astrogre.c` (~80 lines) wraps the in-house engine.
- `backend_onigmo.c` (~110 lines) wraps Onigmo's `onig_new` /
  `onig_search` / `onig_region`.  Both implement `-F` (Onigmo by
  escaping metacharacters at compile time).

## AOT / cached / PG

- **`--aot-compile` (`-C`)**: compile every pattern to
  `code_store/c/SD_<hash>.c`, build `code_store/all.so`, swap each
  node's dispatcher, then run.  Subsequent runs (default mode)
  pick up the cached `all.so` automatically via `astro_cs_load` in
  `OPTIMIZE`.
- **default (cached)**: at pattern allocation time, `OPTIMIZE` calls
  `astro_cs_load` for every node.  Hits use the specialized
  dispatcher; misses fall back to the interpreter.
- **`--pg-compile` (`-P`)**: accepted for CLI parity with abruby;
  currently routes through the same path as `--aot-compile`.  No PG
  profile signal exists yet (HOPT == HORG).
- **`--plain` (`--no-cs`)**: bypass the code store entirely.

Inner SDs are made externally visible via the
`astrogre_export_sd_wrappers` post-process (borrowed from luastro):
each generated SD gets renamed to `SD_<hash>_INL` plus a thin
externally-visible wrapper, so `dlsym` finds every node, not just
the root.  A side array tracks every allocated NODE so the post-build
re-resolve patches the whole chain.

## Search loop folded into the AST

The for-each-start-position search loop is itself a node
(`node_grep_search`) — its EVAL is the loop, and the specializer
treats `body` as a regular NODE * operand, so the SD bakes the
loop AND the inlined regex chain into a single C function.

For `/static/` against a 16 KiB string, the result is ~30 instructions:
loop, `cmpl + cmpw` for the literal, `vmovdqu` for the per-iter
capture-state reset, no indirect calls, no DISPATCH chain.  In-engine
microbench: **22.75 s → 3.15 s on `literal-tail` (7.2× speedup)**.

The grep CLI bench shows essentially no change because each line
(~36 bytes) calls the SD and the per-call overhead (`getline`,
`CTX_struct` zero-init, fwrite of matched lines) dominates.  Folding
the line iteration into the AST as well is the next lever — see
[`todo.md`](./todo.md).
