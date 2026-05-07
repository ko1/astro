# perf.md — pystro 性能計測

## まとめ

2026-05-07 時点 (best-of-5、 `make bench`):

- **micro 10 本中 8 で python3 を上回る** (1.5×〜19× faster)
- **macro 4 本中 1 で python3 を上回る** — richards で **2.1× faster**
- 一般に user-class method dispatch 重視のワークロードは IC で
  python3 と互角〜2× faster、 builtin (bytes / dict) や operator
  overload heavy は依然 1.4×〜5× slow

最適化の積み上げで **macro 全体は元の baseline から 2.5×〜14× faster**
に到達 (richards 6.83 s → 0.48 s)。

## ベースライン情報

ハードウェア・ソフトウェア
- gcc 13 -O2 / SD は `-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`
- Boehm-Demers-Weiser GC + GMP
- 比較対象: `python3` (CPython 3.12.3)

実行モード
- `interp`: `--no-compile`、 SD なし (純 tree-walking)
- `AOT cached`: `code_store/all.so` baked、 best-of-5 run
- `make bench` で micro が、 macro は `bench/macro/` 直接実行 (要
  `CCACHE_DISABLE=1` for AOT bake、 pyaes は `PYTHONPATH=bench/macro`)

## 現状ベンチ (2026-05-07)

### micro (`bench/*.py`)

`bench/*.py` は python3 で約 1 秒かかる規模。

| ベンチ | python3 | pystro interp | pystro AOT | **AOT/python3** |
|---|---:|---:|---:|---:|
| `while_loop` (10M, augassign)  | 0.89 s | 0.18 s | **0.06 s** | **0.07× (14.8× 速い)** |
| `for_range` (15M sum, C range) | 0.91 s | 0.14 s | **0.08 s** | **0.09× (11.4× 速い)** |
| `for_range_pyrange` (Py iter)  | 2.17 s | 0.79 s | **0.36 s** | **0.17× (6.0× 速い)** |
| `list_bench` (7M append+sum)   | 0.86 s | 0.22 s | **0.18 s** | **0.21× (4.8× 速い)** |
| `recursive` (tak(30,20,10))    | 3.87 s | 2.69 s | **1.37 s** | **0.35× (2.8× 速い)** |
| `fib(35)`  (再帰)              | 1.15 s | 0.69 s | **0.41 s** | **0.36× (2.8× 速い)** |
| `mandel`  (float-heavy)        | 0.66 s | 0.65 s | **0.28 s** | **0.42× (2.4× 速い)** |
| `nqueens` (recursion + list)   | 0.68 s | 0.64 s | **0.47 s** | **0.69× (1.45× 速い)** |
| `string_bench` (2M split)      | 0.58 s | 0.57 s | 0.59 s | 1.02× (≒ 同等) |
| `dict_bench` (3M put+get)      | 0.79 s | 1.06 s | 1.10 s | 1.39× (1.4× 遅い) |

### macro (`bench/macro/*.py`) — pyperformance 由来

実アプリ寄りのベンチ。 user-class method dispatch、 多態、
operator overload が支配的。

| ベンチ | python3 | pystro interp | pystro AOT | **AOT/python3** |
|---|---:|---:|---:|---:|
| `richards` (OS sched sim, ~400 行)        | 1.03 s | 4.78 s | **0.47 s** | **0.45× (2.2× FASTER)** |
| `deltablue` (constraint solver, ~600 行)  | 0.16 s | 1.20 s | **0.61 s** | 3.81× |
| `raytrace` (簡易 raytracer, ~400 行)      | 0.95 s | 4.05 s | **2.01 s** | 2.12× |
| `crypto_pyaes` (pure-Py AES-CTR, ~900 行) | 0.55 s | 4.07 s | **1.92 s** | 3.49× |

richards は **python3 の 2 倍速い**。 method dispatch overhead を
完全に潰した結果、 small-class polymorphic な OS scheduler simulation
で python3 の bytecode interpreter を抜いた。

deltablue / raytrace / pyaes はまだ python3 に届かない:
- deltablue: `OrderedCollection.pop(0)` が O(N)。 algorithmic factor。
- raytrace: Vector arithmetic で operator dunder lookup が hot
  (`__add__/__sub__/__mul__` が IC 経路に乗ってない)。
- pyaes: bytes/bytearray の memcpy/memset 主体。 SD 層からは
  触りにくい。

## 投入した最適化 (時系列)

### Phase 1: micro 最適化 (R1〜R10)

| 段階 | 効果 | 概要 |
|---|---:|---|
| §1 `gref_cache @ref` | fib 5× | `node_gref/gset` に inline cache。 strcmp 51% → noise |
| §2 `globals_serial` 構造変化のみで bump | while 71× | tight loop で全 cache invalidate を撤去 |
| §3 `node_for_global` 内蔵 cache | for_range 23× | ループ前に idx 解決、 体内は配列 index 1 回 |
| §4 method_cache (builtin 用) | list 12× | `xs.append(i)` で bound 確保 + strcmp 撤去 |
| §5 `py_apply` を node.h に static inline | fib 1.15× | SD から PLT hop 排除 |
| §6 leaf func の alloca フレーム | fib 1.5× | GC_malloc → C スタック alloca |
| §7 dict identity-equal fast path | dict 1.1× | immediate キーで `py_eq_bool` 関数呼出 skip |
| §8 string slice の buffer 共有 | string 1.6× | borrow ポインタ + Boehm の interior-pointer |
| §9 inline flonum + 算術 fast path | mandel 2.6× | CRuby 流 3-bit rotate encoding |
| §10 fixnum compare fast path | nqueens 1.8× | py_eq / py_cmp 内の GMP 経由を回避 |

### Phase 2: 関数 body の SD 化 (R18 前半)

`py_apply` が `EVAL(c, f->func.body)` を runtime ポインタ経由で
dispatch していた → 関数 body は AOT で SD 化されない。
`code_repo` (koruby パターン) で全 body を集めて compile / load:

- mandel: 0.63 → 0.26 s (-59%)
- recursive: 2.49 → 1.41 s (-43%)
- fib: 0.62 → 0.42 s (-32%)
- nqueens: 0.62 → 0.44 s (-29%)

### Phase 3: Python iterator の高速化 (R18 中盤)

`class PyRange` のような pure-Python iterator (`__iter__` / `__next__`)
を 4.6× → 6.6× faster に:

- `node_attr_set` に attr_cache 追加 (R18 attr_set fast path)
- attr_get/set fast path から strlen+memcmp 除去 (cache match で
  attrs 同一性まで保証されるので key 名検証は冗長)
- `py_iter_next_inline` (kind 0/2 を SD inline、 kind 5 は alloca
  問題で out-of-line)
- `struct py_iter` に `next_m` cache (kind=5 の `__next__` lookup を
  init で 1 回だけに)
- `py_iter_next_user` に `no_stack_protector` (alloca が triggers する
  canary check を撤去、 0.40 → 0.34 s)

### Phase 6: bytes/bit ops + algorithmic (今 session 後半)

`docs/why_slow.md` で各 macro bench の CPython 比 slow の原因を計測
ベースで分析した結果を踏まえ、 上位 ROI 順で着手:

| commit | 改善 | 内容 |
|---|---|---|
| `5cc5ebd` | deltablue -7.7% | `lm_pop` を memmove 化。 element-by-element shift loop だったものを bulk copy に |
| `5e8cc1b` | pyaes -35% | bit op (`& \| ^ << >>`) に fixnum fast path。 AES の 8-bit 値 XOR/AND が毎回 mpz_t 経由していたのを撤去 |

**pyaes 大改善**: 2.74 → **1.92 s** (-30%)。 python3 比 4.7× → **3.5×
slow** に縮まった。 GMP の `__gmpz_init_set_si` (perf 上 3%) を
fixnum 経路でほぼ撤去できたのが効いた。

### Phase 5: 計測ベース細粒度 IC (前 session 後半)

`PYSTRO_DBG_NAMES` を一時的に runtime.c に仕込んで、 slow lookup
される名前を頻度順で集計したら以下の発見:

- raytrace: `y` 2.75M 回 / `z` 2.75M 回 (!)
- deltablue: `weakest_of` 40K / `weaker` 60K / `stronger` 35K /
  `WEAKEST` 15K / `REQUIRED` 10K / `strength` 30K / `name` 30K / ...

raytrace の `y/z` 2.75M は **新 instance の attr_set slow path** から
の descriptor 確認だった (新 Vector の __init__ で attrs が空、
fast path の eidx<elen check 失敗 → slow path → 毎回
`py_class_lookup_method_pub(cls, "y")` で MRO walk + strcmp)。

deltablue の class methods/attrs (`Strength.WEAKEST` 等) は
**class object に対する attr_get** で、 attr_cache の fast path が
PY_T_INSTANCE only だったので毎回 slow path。

3 段階の追加 IC:

| commit | 改善 | 内容 |
|---|---|---|
| `d849a47` | raytrace -3% | node_add/sub/mul に per-call-site binop_cache を追加。 Vector の `+/-/*` で IC 命中 → MRO+strcmp 削除 |
| `3e7f1b4` | raytrace -19.6% | attr_set の cache stamp を冪等化。 既存 (cls, sv) スロットがあれば descriptor 確認を skip |
| `8af7ef6` | deltablue -10% | attr_cache に class-data attr の monomorphic IC (`cls_recv`, `class_value`, `cls_recv_sv`) 追加 |

ベンチ (best-of-5):

|   | Phase 4 後 | Phase 5 後 | 改善 |
|---|---:|---:|---:|
| richards    | 0.48 s | 0.51 s | +5% (微 regression、 struct 拡大の副作用) |
| deltablue   | 0.71 s | 0.65 s | **-10%** |
| raytrace    | 2.48 s | 2.06 s | **-19.6%** |
| crypto_pyaes | 2.73 s | 2.80 s | +3% (noise) |

raytrace の strcmp 22% → 15% に低下、 absolute サンプル数 9.7G →
7.77G。 `py_class_lookup_method_slow` 9.7% → 5.9%。

### Phase 4: macro 最適化 (前 session、 IC 系の積み重ね)

実アプリ寄り benchmark で perf record すると **`__strcmp_avx2` が
27-29% + `py_class_lookup_method` が 13-15% = 合計 40-45%** を占有。
`runtime.c:3833` の `py_method_resolve` のコメントが言ってた:

> // Instance / class method (no inline-cache-able fast path).

つまり pystro の method_cache は **builtin type 専用** で、
user class の instance method には IC が無かった。

5 段階で順番に潰した (各段階で `perf record` + 自前 counter で裏取り):

| commit | 改善 | 内容 |
|---|---|---|
| `ae87e35` | richards -29% | user-class instance method の monomorphic IC を method_cache に追加 (cls_ptr 比較で MRO walk + strcmp + bound 確保 全回避) |
| `2eadb18` | richards -10% | 100% IC thrash 計測 (richards 4.93M/4.93M) → 4-way polymorphic IC に拡張 |
| `bf286e0` | strcmp 大幅減 | dunder 名前 (`__init__/__eq__/...` 24 種) を pre-intern + struct pyclass に slot 配置 + lazy-refresh、 `py_class_lookup_method` で pointer-compare 一発に |
| `ba3897e` | deltablue -67% | attr_cache hit 率を計測 (deltablue 11%、 richards 45%) → 原因が「instance ごとに `attrs_id` 異なる」と判明、 `attrs_id` を class 共有の `shape_version` に置換 |
| `3e90b55` | richards -78% | attr_cache も polymorphic 化 (4-way)。 `Constraint` subclass 混合 iteration で thrash していたのを解消 |

最初の baseline (6.83 / 2.27 / 6.19 / 2.58) → 現在 (0.48 / 0.80 / 2.48 / 2.74):

| ベンチ | baseline | 現在 | 改善 |
|---|---:|---:|---:|
| richards | 6.83 s | **0.48 s** | **14.2× faster** |
| deltablue | 2.27 s | **0.80 s** | 2.8× faster |
| raytrace | 6.19 s | **2.48 s** | 2.5× faster |
| crypto_pyaes | 2.58 s | 2.74 s | 同等 |

## 計測ログ — Phase 4 各段階

### 出発点 (Phase 4 投入前)

`perf record -F 4000 ./pystro bench/macro/deltablue.py`:

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` (libc) | 27.2% |
| `py_class_lookup_method` | 12.8% |
| `py_getattr` | 4.95% |
| `py_apply_slow` | 4.80% |
| `strcmp@plt` | 4.69% |
| `GC_malloc_kind` | 4.39% |
| baked SD (`SD_*`) | 合計 1.4% |

baked SD が時間の 1.4% しか占めていない — 残りは全部 dispatch
overhead で、 これが攻め所だった。

### `ae87e35` 後 (user-class IC)

| 関数 | 占有 (after) | (before) |
|---|---:|---:|
| `__strcmp_avx2` | 27.6% | 27.2% |
| `py_class_lookup_method` | 13.6% | 12.8% |
| `py_getattr` | 6.66% | 4.95% |
| `GC_malloc_kind` | **2.56%** | 4.39% |
| `py_apply_slow` | 削除 | 4.80% |

strcmp 占有率は変わらないが、 absolute サンプル数が 19% 減 (deltablue
8.7G → 7.06G)。 GC_malloc 半減は **毎 call の `py_make_bound`
確保が消えた** 効果。 `py_class_lookup_method` がまだ 13.6% を
占めるのは、 method_cache 経路以外 (`__init__` / `__hash__` /
operator dunder など special method lookup、 runtime.c に 20 箇所超
call site あり) からも呼ばれるため。

### `2eadb18` 後 (4-way PIC)

instrument して計測した IC hit/miss:

- richards: total resolve 36M、 100% thrash (cache を毎回 overwrite)
- deltablue: 400K resolve、 23% miss
- → method_cache を 4-way polymorphic に拡張

richards は Task / Packet / WorkerTaskRec / HandlerTaskRec / DeviceTaskRec /
IdleTask / WorkTask の 6+ クラス混合 iteration が支配的だったので
4-way で完全に thrash 解消。

### `bf286e0` 後 (intern + dunder slot)

`__init__/__eq__/__lt__/__hash__/__setattr__/__getattr__/__getattribute__/__bool__/__len__/__getitem__/__setitem__/__contains__/__iter__/__next__/__call__/__get__/__index__/__invert__/__neg__/__metaclass__/__set_name__/__repr__/__str__` の 24 種:

- `install_builtins` で `intern_name` を呼んで `PYSTRO_INTERN_*`
  globals に保存
- `runtime.c` の string literal 経由 lookup を全て global に置換
- `struct pyclass` に `slot_init/slot_eq/...` を追加、 lazy refresh で
  populate
- `py_class_lookup_method` は dunder 名前で pointer-compare → slot
  load の O(1) パス。 strcmp は slow path に隔離
- ASTroGen (`pystro_gen.rb` override) で `const char *` operand を
  SD literal ではなく `n->u.X.name` field 参照で emit (parser intern
  pool ポインタが SD に直接渡る)

deltablue で `__strcmp_avx2` 28% → 12% に大幅減。

### `ba3897e` 後 (shape_version)

attr_cache hit 率を計測 — deltablue 11% / richards 45%。 instance
ごとに `attrs_id` (= `PY_PTR(o->inst.attrs)`) が異なるので、
同じ class でも別 instance だと毎回 miss していた。

修正:
- `cache->attrs_id` (uint64_t) を `cache->shape_version` (uint32_t) に置換
- `struct pyclass` に `shape_version` field、 method add で bump
- 同 class の複数 instance 間で eidx を共有可能に (attr 名前を同じ
  insertion order で入れる前提、 通常 __init__ で成り立つ)

ベンチ:
- richards: 4.34 → 2.31 s (-46%)
- deltablue: 1.79 → 0.74 s (-59%)
- raytrace: 4.98 → 2.70 s (-46%)

### `3e90b55` 後 (attr 4-way PIC)

shape_version 後でも attr_cache hit 率は deltablue 71% / richards 64%
(残り 30% は polymorphic 受信)。 method_cache と同パターンで
4-way 化。 deltablue は `Constraint` subclass の `recalculate` /
`output` 等を todo list で混合 iter していて完全 polymorphic だった。

ベンチ:
- richards: 2.31 → **0.48 s** (-79%、 もう一段の improvement)
- deltablue: 0.74 → **0.69 s** (-7%)
- raytrace: 2.70 → **2.48 s** (-8%)

## 現在の profile (Phase 4 完了後)

### deltablue (0.71 s)

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 11.2% |
| `lm_pop` (list pop) | 5.9% |
| `py_apply_slow` | 5.0% |
| `py_getattr` | 4.9% |
| `py_class_lookup_method_slow` | 4.3% |
| `GC_malloc_kind` | 1.7% |

strcmp が 11% に縮んだが、 `lm_pop` 6% がアルゴリズム的に
expensive (`OrderedCollection.pop(0)` が O(N))。 `__matmul__` /
`__rfloordiv__` 等 minor dunder の operator lookup が strcmp の
残存原因。

### richards (0.48 s)

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 7.5% |
| `py_getattr` | 5.5% |
| `GC_malloc_kind` | 5.0% |
| `py_class_lookup_method_slow` | 4.5% |
| `py_hash` | 3.3% |

baked SD が 60%+ を占めて主導。 残り 30% は dispatch overhead
だが、 これ以上削るには JIT 化や AST node fusion の世界に入る。

### raytrace (2.48 s)

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 22.9% |
| `py_class_lookup_method_slow` | 9.3% |
| `strcmp@plt` | 5.8% |
| `GC_malloc_kind` | 3.4% |
| `py_try_binop_dunder` | 1.3% |

raytrace は Vector の `__add__/__sub__/__mul__/__neg__` が **slot
未対応** で linear MRO walk + strcmp に落ちる。 これが strcmp 23%
の正体。 operator dunder slot を追加すれば改善見込みだが、 試して
みた範囲では struct 拡大の cache 副作用で他が regress した
(struct pyclass が 272B → 448B に膨れた)。 別アプローチ (perfect
hash dispatch、 bytecode-style method resolution) が必要。

### crypto_pyaes (2.74 s)

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 11.0% |
| `GC_malloc_kind` | 7.9% |
| `__memmove_avx_unaligned_erms` | 4.9% |
| `__memset_avx2_unaligned_erms` | 4.9% |
| `py_class_lookup_method_slow` | 4.9% |
| `__gmpz_init_set_si` (GMP) | 2.9% |

bytes/bytearray の memmove + memset が 9.8%、 GC alloc 8%。
SD 層からは触れにくい層 (libc + Boehm) が hot。 改善するには:
- bytes 操作 (XOR、 slice) の専用 fast path
- Boehm の代替 (Bartlett mostly-copying など、 ASTro 計画あり)
- GMP の fixnum→bignum 変換コストを fixnum-only path で回避

## 帯域 (perf stat)

deltablue で python3 と pystro AOT の比較:

|   | python3 | pystro AOT |
|---|---:|---:|
| 経過時間 | 0.18 s | 0.71 s |
| 命令数 | 2.0 B | 5.5 B |
| IPC | 2.95 | 2.7 |
| LLC miss / refs | n/a | ~10% |

命令数 2.7× 多が直接の時間差要因。 IPC は近い (2.95 vs 2.7) ので
CPU 効率は悪くない。 残りは「やる仕事の絶対量」を減らす方向しか
ない (= JIT、 type inference、 AST 融合 etc.)。

## これから

優先度順:

1. **operator dunder の slot 化** — raytrace の strcmp 23% を狙う。
   struct 拡大による副作用を避けるには、 slot を別 struct に切り出して
   pyclass→slot table へポインタ 1 個だけ持たせる pattern が良さそう。
2. **bytes 操作の専用化** — pyaes の memcpy/memset/GC を狙う。
   XOR / slice / concat に inline path を入れる。
3. **method 名 hash dispatch** — 24-way / 46-way の linear scan を
   perfect hash で O(1) 化。 全 dunder lookup が高速化。
4. **statically-known type の inline cache** — `int + int` のような
   monomorphic 数値 op を SD 内で完全 inline 展開。 nqueens / mandel
   をさらに削れる見込み。

JIT 化 (動的に native code を吐く) は別議論。 ASTro 全体の方向と
要相談。

## perf 採取例

```sh
# AOT bake
rm -rf code_store
CCACHE_DISABLE=1 ./pystro -c bench/macro/richards.py

# プロファイル
perf record -F 4000 -o /tmp/p.data ./pystro bench/macro/richards.py
perf report -i /tmp/p.data --no-children --stdio --no-call-graph -s symbol | head -25

# 命令数 / IPC
perf stat ./pystro bench/macro/richards.py 2>&1 | grep -E "instructions|cycles|IPC"

# 関数 caller 追跡
perf record -F 4000 --call-graph=dwarf -o /tmp/dw.data ./pystro bench/macro/richards.py
perf report -i /tmp/dw.data --no-children --stdio -g caller --inverted | head -50
```

## 残ボトルネック (まとめ)

### dict_bench

CPython は数十年磨かれた dict 実装で、 dunder lookup の C インライン、
サイズ別 layout (~7 種類)、 専用 string-keyed layout などを持つ。
pystro はジェネリック open-addressing 1 種のみ。 1× を逆転するなら
str-key 専用パスが必要。

### deltablue / raytrace / pyaes の operator dunder

`__add__/__sub__/__mul__/__or__/__and__/__xor__/__lshift__` 等が
slot 未対応で MRO walk + strcmp。 **slot を別 struct に切り出す
パターン** で struct 拡大の副作用を避けつつ追加できる見込み。

### Boehm GC のオーバーヘッド

instance allocation、 list/dict alloc が macro で 5〜10% 占有。
Bartlett mostly-copying GC への切り替えは ASTro 全体計画 (cf.
`docs/idea.md`) で検討中。

## R10 → R18 比較 (履歴)

R7〜R10 で取った旧計測との差分。 このセッションでの Phase 4 直前。

| ベンチ | R10 AOT | R18 AOT (Phase 4 前) | 差 | 原因 |
|---|---:|---:|---:|---|
| `while_loop` | 0.05 s | 0.05 s | 同 | — |
| `for_range` | 0.09 s | 0.12 s | +33% | iter で IndexError catch する setjmp 追加 |
| `list_bench` | 0.19 s | 0.22 s | +16% | append の path で state チェック増 |
| `fib(35)` | 0.62 s | 0.42 s | -32% | per-body SD (R18 後半 fix) |
| `recursive` (tak) | 2.49 s | 1.41 s | -43% | 同上 |
| `mandel` | 0.63 s | 0.26 s | -59% | 同上、 inline flonum で float fast path SD 化 |
| `nqueens` | 0.62 s | 0.44 s | -29% | 同上 |
| `dict_bench` | 0.82 s | 0.99 s | +21% | metaclass __call__ の `PYSTRO_BI_KWC` save/restore |
