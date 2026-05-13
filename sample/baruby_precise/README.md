# baruby_precise — baruby + precise mark&sweep GC (MVP)

`sample/baruby/` (libgc conservative) を copy して、 **GC を precise
mark&sweep に置換した試作品**。 [`docs/gc_design.md`](../../docs/gc_design.md)
で議論した「共有 stack `sp[]` に root を spill する Lua-style モデル」 を
ベタ書きで実装し、 conservative 版とベンチ比較できる状態にしてある。

言語仕様 (Array / String / fixnum / 関数定義) は baruby と同一。 GC 周り
だけが違う。

For details:
- [docs/spec.md](docs/spec.md) — language surface (baruby と同じ)
- [docs/runtime.md](docs/runtime.md) — sp[] threading, gc.c の precise
  mark&sweep、 BARUBY_EVAL_ARG macro 等
- [docs/perf.md](docs/perf.md) — **conservative (libgc) vs precise の
  実測ベンチ結果**
- [docs/todo.md](docs/todo.md) — 既知バグ + 残タスク
- [docs/done.md](docs/done.md) — baruby fork 時の話 (precise 化前)

baruby との関係:

| | baruby | baruby_precise |
|---|---|---|
| GC | Boehm libgc (conservative) | 自前 mark&sweep (precise) |
| 共通引数 | `(c, n, fp)` | `(c, n, fp, sp)` |
| Heap alloc | `GC_MALLOC` macro | `baruby_gc_alloc(kind, size, sp_top)` |
| Bench | base line | base line +12〜48% (perf.md) |

## Install

### Prerequisites (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby ruby-bundler git
```

ASTroGen is plain Ruby (3.x).  baruby_precise does **not** need libgc
(self-contained mark&sweep in `gc.c`).

### libprism

prism build を `sample/baruby/prism` から symlink で共有している。
sibling サンプル (baruby) を先にビルドしておけば自動で拾われる。

### Build

```sh
make                  # build ./baruby_precise
make run              # build + ./baruby_precise --plain test.ba.rb
make bench            # build + ruby bench/run.rb
make clean
```

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
./baruby --plain test.ba.rb
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
# ...
# __ELAPSED__ 1.01
# __GC_STATS__ alloc_bytes=560000000 heap_bytes=... gc_count=133
```

数字は自前 mark&sweep の `baruby_gc_stats` から。 `heap_bytes` は
realloc の差分追跡をしていないので unsigned underflow して桁外れの値が
出るが (= 表示だけの問題)、 `alloc_bytes` と `gc_count` は正しい値。

### Modes

| Flag | Mode | Notes |
|---|---|---|
| (none) | Plain + AOT bake | Run interpreted, then bake `code_store/all.so` |
| `-i` / `--plain` | Plain | No AOT load, no bake |
| `-c` | Compile only | Bake `code_store/all.so` |
| `-p` | Profile-guided | PG-bake at exit (動作未検証) |
| `-b` | Benchmark mode | Skip bake |
| `--ccs` | Clear store | Wipe `code_store/` before run |

**動作確認済**: plain mode (test.ba.rb / test_ary.ba.rb / bench), AOT mode
(bench)。 PG mode は precise rooting 経由で未検証。

### Benchmarks

```sh
make bench                              # plain mode (default)
CCACHE_DISABLE=1 ./baruby_precise -c bench/list_alloc.ba.rb   # AOT
```

`docs/perf.md` の §2 に conservative (baruby) との実測比較あり。

## ベンチでの GC 動作 (要点)

| Bench | precise plain | precise AOT | vs baruby (libgc) |
|---|---:|---:|---|
| list_alloc | 1.01 s | 0.43 s | +12% / +13% |
| string_concat | 1.14 s | 0.98 s | +32% / +48% |
| binary_trees | 🐛 0.31 s | 🐛 0.20 s | **計算結果が壊れる**、 要 debug |

詳細は [docs/perf.md](docs/perf.md)、 既知バグは [docs/todo.md](docs/todo.md)
P0 を参照。

## Architecture

```
foo.ba.rb
  └─ Prism (pm_node_t)
       └─ baruby_parse.c transduce  ──► NODE * (ASTroGen format)
       └─ OPTIMIZE(ast) → astro_cs_load → dlsym SD_<hash>
       └─ EVAL(c, ast, fp, sp)  =  (*ast->head.dispatcher)(c, ast, fp, sp)
       └─ build_code_store: astro_cs_compile / build / reload
```

GC: `gc.c` の mark&sweep が `c->env..c->sp` を flat scan、 全 heap object を
linked list で管理。 詳細 [docs/runtime.md](docs/runtime.md) §5。
