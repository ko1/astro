# are — modern grep on the astrogre regex engine

`are` is a `grep`-style command-line tool on top of the
[`astrogre`](../) Ruby/Onigmo-compatible regex engine.  It plays in
the same space as [ripgrep (`rg`)](https://github.com/BurntSushi/ripgrep),
[the silver searcher (`ag`)](https://github.com/ggreer/the_silver_searcher),
and [`ack`](https://beyondgrep.com/) — keeping the muscle memory of
`grep` flags but switching the defaults to what people actually want
when searching a code tree.

## Quick start

```sh
$ make                          # build
$ ./are 'TODO'                  # search the cwd recursively, hidden+binary skipped
$ ./are -t rust 'fn main'       # restrict to Rust files
$ ./are -T md 'cargo'           # everything BUT Markdown
$ ./are -i -n -A 2 'panic!'     # case-ins, line numbers, 2 trailing context lines
$ ./are --type-list             # show the file-type table
```

## Why it exists

`astrogre` has a perfectly serviceable grep front-end already
(`../astrogre`), but it inherited GNU grep's defaults: PATH required,
no recursion without `-r`, dotfiles included, no file-type filter.
That makes it fine for engine testing and for benchmarking against
GNU grep, but awkward to actually use day-to-day on a code base.

`are` is the production CLI — same engine, modern UX.  The two
binaries coexist; the legacy `astrogre` binary still owns the
library-internal sub-commands (`--self-test`, `--bench`, `--dump`,
`--via-prism`, `--verbose`, …) which `are` deliberately doesn't
expose.

## Defaults that differ from GNU grep

| Behaviour                              | grep        | are          |
| -------------------------------------- | ----------- | ------------ |
| PATH required                          | yes         | defaults `.` |
| Recurse into directories               | `-r`        | always       |
| Skip hidden (`.foo`) entries           | no          | yes          |
| Skip binary files                      | no          | yes          |
| Colour output on a tty                 | distro-dep. | yes          |
| Pattern syntax                         | BRE         | Ruby/Onigmo  |
| Default encoding                       | locale      | UTF-8        |

To get back grep-like behaviour: `--no-recursive`, `--hidden`, `-a`
(text mode), `--color=never`.

## Feature comparison

| Feature                      | grep | ag | ack | rg | **are** |
| ---------------------------- | :--: | :-: | :-: | :-: | :-: |
| Recursive default            |  -   | ✓  | ✓  | ✓  | ✓ |
| `.gitignore` aware           |  -   | ✓  | -  | ✓  | _planned_ |
| Skip binary by default       |  -   | ✓  | ✓  | ✓  | ✓ |
| Skip hidden by default       |  -   | ✓  | ✓  | ✓  | ✓ |
| Multi-thread file walking    |  -   | ✓  | -  | ✓  | _planned_ |
| Type filter (`-t LANG`)      |  -   | ✓  | ✓  | ✓  | ✓ |
| User-defined types           |  -   | ✓  | ✓  | ✓  | ✓ |
| Look-around / named captures |  -   | -  | ✓  | `-P` | ✓ |
| Onigmo MatchCache (ReDoS)    |  -   | -  | -  | -  | ✓ |
| AOT specialisation cache     |  -   | -  | -  | -  | ✓ (`--aot`) |

The "_planned_" items are tracked in [Roadmap](#roadmap) below; the
✓-only column is the part that's already shipping.

## Options

```
Match options:
  -i  case-insensitive            -n  print line numbers
  -c  count matching lines        -v  invert match
  -w  whole word                  -x  match whole line
  -F  fixed string                -q  quiet (exit code only)
  -l  files with matches          -L  files without
  -H  always show filename        -h  never show filename
  -o  only matching parts         -Z  NUL-separated filenames
  -m N  stop after N matches/file
  -A N / -B N / -C N              context lines (after / before / both)
  -e PATTERN (repeatable)         -f FILE  read patterns from FILE

Walking options:
  -t LANG / -T LANG               include / exclude file type
  --type-add NAME:GLOB[:GLOB...]  define a custom type
  --type-list                     list known types and exit
  --include=GLOB / --exclude=GLOB extra basename glob filters
  --hidden                        descend into hidden entries
  -a, --text                      do NOT skip binary files
  --no-recursive                  do NOT descend into directories

Other:
  --color=never|always|auto       (default auto via isatty)
  --engine=astrogre|onigmo        backend selection (default astrogre)
  --aot                           AOT-specialise patterns to code_store/
  -V, --version                   print version
      --help                      print help
```

## Built-in file types

Run `are --type-list` for the full table.  The starter set covers ~50
languages and configuration formats: `c`, `cpp`, `rust`, `go`,
`ruby`, `python`/`py`, `js`, `ts`, `java`, `kotlin`, `swift`,
`haskell`, `ocaml`, `lua`, `perl`, `php`, `sh`, `sql`, `html`,
`css`, `md`, `tex`, `json`, `yaml`, `toml`, `xml`, `make`, `cmake`,
`docker`, `proto`, `asm`, `vim`, `r`, `dart`, `zig`, `nim`,
`crystal`, … (extend it in `types.c` or via `--type-add`).

## Architecture

Most of `are` is a thin shell over the astrogre engine:

```
are/
├── main.c    ← CLI entry, opt parser, recursive walker, mmap fast paths
├── types.c   ← `-t LANG` table + lookup
├── types.h
├── Makefile  ← reuses parent astrogre/ sources directly
└── README.md
```

The Makefile pulls `node.c parse.c match.c selftest.c
backend_astrogre.c` straight from the parent `astrogre/` directory,
along with the codegen products (`node_eval.c` etc.) — there's no
separate static library, just one `cc` invocation per build.

`main.c` is currently a fork of `../main.c` with modernised defaults
and the lib-internal sub-commands stripped.  Consolidating it into a
shared `lib_grep.c` with two thin entry points (one per binary) is
on the roadmap.

## Why use `are` over `rg` or `ag`?

Be honest about it: **for plain literal-search throughput on a big
checkout, `rg` is currently the king** and that won't change in the
next few weeks — its DFA, multi-pattern Teddy prefilter, and parallel
walker have had a decade of optimisation by people who really care.

Where `are` aims to land:

1. **Ruby/Onigmo regex semantics out of the box.**  `rg -P` enables
   PCRE2 (close but not identical to Onigmo); `are` is byte-for-byte
   the same engine you get from `Regexp` in Ruby.  Look-behind,
   named captures, conditional groups, subroutine calls, set
   operations on character classes, MatchCache for ReDoS — all on
   by default, no flag needed.
2. **AOT pattern specialisation.**  `--aot` writes per-pattern
   specialised C to `code_store/` and links it back in.  For a
   pattern you grep with hundreds of times (CI scripts, editor
   integration), the per-invocation match loop becomes a tight,
   pattern-shape-aware basic block.  Nobody else in the grep family
   does this.
3. **Embeddable.**  `astrogre` exposes a stable C ABI and a Ruby C
   extension (`require "astrogre"`), so the same engine that powers
   `are` can be dropped into Logstash-style pipelines, editor
   tooling, or app-level validators without a separate compile.

What we have NOT done yet (and where `rg` will keep winning until we
do):

- `.gitignore` parsing — currently you need `--exclude=` per pattern.
- Parallel file walking — single-threaded today.
- Aho-Corasick / Teddy multi-pattern literal prefilter — the engine
  picks ONE literal anchor and runs `memmem`/`memchr` on it.

## Roadmap

In rough order of expected impact:

1. **`.gitignore` walker** — hierarchical (parent + per-dir),
   negation patterns, `**/`.  ~500 LoC.  Parity with `rg` for
   "doesn't waste time scanning `node_modules/`".
2. **Aho-Corasick multi-pattern anchor** — extract necessary literals
   from each compiled IRE, build an AC automaton, scan once per
   buffer, verify hits with the full engine.  ~1000 LoC.  This is
   the single biggest performance lever vs `rg`.
3. **Parallel file walking** — POSIX threads + work-stealing queue
   on the dirent stream.  ~300 LoC.  CPU-bound on big trees.
4. **Smart-case** (`-S`): if the pattern has no uppercase, behave as
   `-i`; else case-sensitive.  Trivial.
5. **`--files`** — list every file `are` would otherwise search.
   Useful for piping to other tools and for debugging the walker.
6. **JSON output** (`--json`) — for editor integration / piping into
   `jq`.  Mirrors `rg --json`.

When 1–3 land, we should be able to make a serious case against
`rg` on a code-base benchmark.  Until then, `are` is "modern grep
with a Ruby regex engine and a roadmap".

## See also

- Parent engine and library docs: [`../README.md`](../README.md),
  [`../docs/done.md`](../docs/done.md), [`../docs/todo.md`](../docs/todo.md).
- Ripgrep design notes:
  https://blog.burntsushi.net/ripgrep/ — required reading if you
  want to understand where the throughput targets come from.
