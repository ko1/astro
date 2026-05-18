# baruby_precise — precise GC testbed (14 backends)

`sample/baruby/` (libgc conservative) を fork して、 **precise rooting +
複数の自前 GC を切替えてベンチ比較できる testbed**。 共有 `sp[]` で
root を spill する Lua/Rust 系モデル ([`docs/gc_design.md`](../../docs/gc_design.md))
を実装し、 14 種類の GC algorithm を **build-time switch** (`make GC=<name>`)
で選べる。

言語仕様 (Array / String / fixnum / 関数定義) は baruby と同一。 GC 周りと
それを支える rooting / WB / sp threading だけが違う。

## 14 GC backends

`make GC=<name>` で切替え。 default は `copy`。 詳細は [docs/runtime.md](docs/runtime.md) §5.10。

| # | Name | Strategy | Gen | Moves? |
|---|---|---|---|---|
| 1 | `none` | libc malloc + leak | — | no |
| 2 | `mark` | slab page + mark&sweep | — | no |
| 3 | `mark_gen` | slab page + mark&sweep, 2-gen | yes | no |
| 4 | `mark_gen_inc` | mark_gen + 増分マーキング (SATB) | yes | no |
| 5 | `copy` | Cheney semi-space (default) | — | yes |
| 6 | `copy_gen` | bump nursery + semispace tenured | yes | yes |
| 7 | `copy_gen_inc` | copy_gen + 増分マーキング (SATB) | yes | yes |
| 8 | `mark_compact` | single region + Lisp-2 slide compact | — | yes |
| 9 | `mark_compact_gen` | bump nursery + bump tenured + slide compact | yes | yes |
| 10 | `bump` | bump alloc only, leak (alloc floor baseline) | — | no |
| 11 | `mark_bump_gen` | bump nursery + bump tenured, no compact | yes | no |
| 12 | `immix` | block (32 KiB) + line (128 B) mark-region, no evac | — | no |
| 13 | `immix_gen` | bump nursery + Immix tenured (mark-region, no evac) | yes | nursery→tenured copy |
| 14 | `mark_bitmap` | sticky mark&sweep + per-page bitmap (8 B header) | yes | no |

For details:
- [docs/spec.md](docs/spec.md) — 言語仕様 (baruby と同じ)
- [docs/gc_runtime.md](docs/gc_runtime.md) — **GC を知らない人向け入門 + 14 backend 早見表 + 設計空間**
- [docs/runtime.md](docs/runtime.md) — sp[] threading、 14 backend カタログ (技術詳細)
- [docs/perf.md](docs/perf.md) — **全 14 backend × 14 bench 実測 + libgc 比較**
- [docs/todo.md](docs/todo.md) — 既知の制約 + 残タスク
- [docs/done.md](docs/done.md) — 変更履歴

## baruby との関係

| | baruby | baruby_precise |
|---|---|---|
| GC | Boehm libgc (conservative) | 11 種類の自前 GC を build-time switch |
| 共通引数 | `(c, n, fp)` | `(c, n, fp, sp)` |
| Heap alloc | `GC_MALLOC` macro | `baruby_gc_alloc(kind, size, sp_top)` |
| Write barrier | 無し (libgc 不要) | gen 系 backend で `baruby_gc_wb` |
| 最速 backend vs libgc | base line | **全 11 bench で勝つ、 geomean ~ -22%** |

[docs/perf.md](docs/perf.md) §3 に libgc との 11 bench fair 比較あり
(`life.ba.rb` は baruby 側の独立バグで除外)。 string_concat / binary_trees
で -40〜-46%、 mutator 支配 bench (nqueens / list_sort) でも -7〜-9%。

## Install

### Prerequisites (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby ruby-bundler git
```

baruby_precise は **libgc を必要としない** (全 11 backend が自前 GC)。

### libprism

prism build を `sample/baruby/prism` から symlink で共有している。
sibling サンプル (baruby) を先にビルドしておけば自動で拾われる。

### Build

```sh
make                          # build ./baruby_precise (default GC=copy)
make GC=mark_gen_inc          # build with mark_gen_inc backend
make run                      # build + ./baruby_precise --plain test.ba.rb
make bench                    # build + ruby bench/run.rb
make clean
```

利用可能な GC: `none mark mark_gen mark_gen_inc copy copy_gen copy_gen_inc mark_compact mark_compact_gen bump mark_bump_gen immix immix_gen mark_bitmap`

AOT mode (`-c`): `CCACHE_DISABLE=1 ./baruby_precise -c bench/list_alloc.ba.rb`
で SD specialize → code_store/all.so 構築 → 再 dlopen。 CCACHE_DISABLE は
sandbox 環境での ccache 書込み問題回避用。

## Usage

### Run a script

```ruby
# test.ba.rb
def fib(n)
  if n < 2
    1
  else
    fib(n-2) + fib(n-1)
  end
end

p fib(20)
```

```sh
./baruby_precise --plain test.ba.rb
# 10946
# Result: 10946, node_cnt:22
# __ELAPSED__ 0.000123
```

Array + String:

```ruby
a = [1, 2, 3]
a.push(4)
p a              # [1, 2, 3, 4]
p a.size         # 4
p a[-1]          # 4

s = "hello"
p s + " " + "world"   # "hello world"
p s.size              # 5
p s[0]                # "h"
```

### GC stats

```sh
BARUBY_GC_STATS=1 ./baruby_precise --plain bench/list_alloc.ba.rb
# __ELAPSED__ 0.94
# __GC_STATS__ backend=copy alloc_bytes=560000000 heap_bytes=... \
#              gc_count=1 minor=0 major=0 gc_seconds=0.0001 gc_pct=0.0 \
#              max_pause_ms=0.10
```

出力:
- `backend`: build-time に選んだ GC algorithm
- `alloc_bytes` / `heap_bytes`: 累計 alloc / live size
- `gc_count` / `minor` / `major`: collection 数
- `gc_seconds` / `gc_pct`: 累計 GC 時間とミューテータに対する比率
- `max_pause_ms`: 1 回 collect の最大 wall time (latency upper-bound)

### Stress mode

```sh
BARUBY_GC_STRESS=1 ./baruby_precise --plain bench/binary_trees.ba.rb
```

- 毎 alloc で GC を発火 (「mark 漏れ」 がその場で発覚)
- moving GC では旧 from-space を恒久 retire (`mprotect(PROT_NONE)` +
  `madvise(DONTNEED)`)、 stale pointer の deref が即 SIGSEGV
- 開発中の事実上の必須モード

### Modes

| Flag | Mode | Notes |
|---|---|---|
| (none) | Plain + AOT bake | Run interpreted, then bake `code_store/all.so` |
| `-i` / `--plain` | Plain | No AOT load, no bake |
| `-c` | Compile only | Bake `code_store/all.so` |
| `-p` | Profile-guided | PG-bake at exit |
| `-b` | Benchmark mode | Skip bake |
| `--ccs` | Clear store | Wipe `code_store/` before run |

**動作確認済**: plain mode (全 11 backend で test 3 種 + 12 bench)、
stress mode (test 3 種)、 AOT mode (一部、 [docs/todo.md](docs/todo.md) 参照)。

## Benchmarks

```sh
make bench                              # plain mode (default), 全 12 bench
BARUBY_BIN=./baruby_precise ruby bench/run.rb -n 3   # 3-run 中央値
```

[docs/perf.md](docs/perf.md) §2 に 11 backend × 12 bench の table 一覧、
§3 に libgc 比較あり。

## Architecture

```
foo.ba.rb
  └─ Prism (pm_node_t)
       └─ baruby_parse.c transduce  ──► NODE * (ASTroGen format)
       └─ OPTIMIZE(ast) → astro_cs_load → dlsym SD_<hash>
       └─ EVAL(c, ast, fp, sp)  =  (*ast->head.dispatcher)(c, ast, fp, sp)
       └─ build_code_store: astro_cs_compile / build / reload
```

GC: `gc_<name>.c` (build 時に 1 つだけリンク) が `c->env..c->sp` を
flat scan して root を識別。 各 backend の詳細は
[docs/runtime.md](docs/runtime.md) §5.10。
