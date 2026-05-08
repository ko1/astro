# perf.md — pystro 性能計測

## まとめ (2026-05-08)

5 条件 (CPython 3.12 / 3.14 / 3.14+JIT / pystro interp / pystro AOT) で
best-of-5。 詳細表は [現状ベンチ](#現状ベンチ-2026-05-08) を参照。

- **vs CPython 3.12.3** (Ubuntu 24.04 stock): macro 4/4 + micro 9/10 で勝ち
- **vs CPython 3.14.4 (no-JIT)**: macro 3/4 + micro 9/10 で勝ち
  (`raytrace` が 3.14 の 25% speedup で逆転される)
- **vs CPython 3.14.4 +JIT**: macro 3/4 + micro 9/10 で勝ち
  (JIT は本ベンチ規模 ~1s では大半が中立 or 微悪化、
  richards のみで JIT が +7%)

JIT との比較は [vs_cpython.md](./vs_cpython.md) で詳しく分析。

## ベースライン情報

ハードウェア・ソフトウェア
- gcc 13 -O2 / SD は `-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`
- Boehm-Demers-Weiser GC + GMP

比較対象 (5 条件):

| 略号 | 実体 | 備考 |
|---|---|---|
| `py3.12` | CPython 3.12.3 (Ubuntu 24.04 stock `python3`) | 純 bytecode interpreter、 PEP 659 specialization あり |
| `py3.14` | CPython 3.14.4 (uv install) PYTHON_JIT=0 | Tier 2 uops + 諸最適化、 JIT は無効 |
| `py3.14+JIT` | CPython 3.14.4 PYTHON_JIT=1 | copy-and-patch JIT (PEP 744) 有効 |
| `pystro-i` | `./pystro --no-compile` | pure tree-walking interpreter (SD なし) |
| `pystro-AOT` | `./pystro` + code_store baked | SD compiled、 IC + AST direct dispatch |

実行モード:
- `make bench` で micro が、 macro は `bench/macro/` 直接実行 (要
  `CCACHE_DISABLE=1` for AOT bake、 pyaes は `PYTHONPATH=bench/macro`)

## 現状ベンチ (2026-05-08)

### macro (`bench/macro/*.py`) — pyperformance 由来

実アプリ寄り。 user-class method dispatch、 多態、 operator
overload、 bignum、 bytes 操作が支配的。

| ベンチ | py3.12 | py3.14 | py3.14+JIT | pystro-i | pystro-AOT | best |
|---|---:|---:|---:|---:|---:|---|
| `richards`     | 1.07 s | 0.90 s | 0.84 s | 0.87 s | **0.48 s** | pystro |
| `deltablue`    | 0.17 s | 0.15 s | 0.16 s | 0.18 s | **0.14 s** | pystro |
| `raytrace`     | 0.91 s | **0.68 s** | 0.72 s | 1.02 s | 0.86 s | py3.14 |
| `crypto_pyaes` | 0.56 s | 0.49 s | 0.54 s | 0.47 s | **0.40 s** | pystro |

pystro AOT を CPython 各バージョンと比較した時の比 (1× 未満が pystro 勝):

| ベンチ | vs py3.12 | vs py3.14 | vs py3.14+JIT |
|---|---:|---:|---:|
| `richards`     | **0.45× (2.23× faster)** | **0.53× (1.88× faster)** | **0.57× (1.75× faster)** |
| `deltablue`    | **0.82× (1.21× faster)** | **0.93× (1.07× faster)** | **0.88× (1.14× faster)** |
| `raytrace`     | **0.95× (1.06× faster)** | 1.26× SLOWER | 1.19× SLOWER |
| `crypto_pyaes` | **0.71× (1.40× faster)** | **0.82× (1.23× faster)** | **0.74× (1.35× faster)** |

**3.14 の素の改善が大きい**。 JIT 無しでも 3.12 → 3.14 で
richards 16% / raytrace 25% / pyaes 13% 速くなっている (Tier 2 uops
interpreter + 諸最適化)。 raytrace では 3.14 が pystro AOT を
逆転した。

**JIT は本規模では win とは限らない**。 macro 4 本中 JIT が改善した
のは richards のみ (0.90 → 0.84、 +7%)。 deltablue / raytrace / pyaes
では JIT 有り方が逆に遅い:
- deltablue 0.15 → 0.16 (+7%)
- raytrace 0.68 → 0.72 (+6%)
- pyaes 0.49 → 0.54 (+10%)

ベンチ規模 ~1s では JIT warmup コストを回収しきれない、 もしくは
JIT-compiled trace の hot loop が短すぎて bytecode interpreter
(Tier 2 含む) より勝てないと推測される。 詳細分析は
[vs_cpython.md](./vs_cpython.md)。

### micro (`bench/*.py`)

`bench/*.py` は python3 で約 1 秒かかる規模。

| ベンチ | py3.12 | py3.14 | py3.14+JIT | pystro-i | pystro-AOT | best |
|---|---:|---:|---:|---:|---:|---|
| `while_loop`         | 0.88 s | 0.85 s | 0.90 s | 0.18 s | **0.05 s** | pystro |
| `for_range`          | 0.96 s | 0.89 s | 0.99 s | 0.14 s | **0.08 s** | pystro |
| `for_range_pyrange`  | 2.33 s | 1.81 s | 1.87 s | 0.82 s | **0.37 s** | pystro |
| `list_bench`         | 0.89 s | 0.86 s | 1.02 s | 0.21 s | **0.19 s** | pystro |
| `recursive` (tak)    | 3.79 s | 2.70 s | 2.63 s | 2.50 s | **1.39 s** | pystro |
| `fib(35)`            | 1.14 s | 0.77 s | 0.80 s | 0.68 s | **0.41 s** | pystro |
| `mandel`             | 0.66 s | 0.60 s | 0.65 s | 0.59 s | **0.25 s** | pystro |
| `nqueens`            | 0.66 s | 0.61 s | 0.64 s | 0.56 s | **0.33 s** | pystro |
| `string_bench`       | 0.56 s | 0.68 s | 0.67 s | 0.51 s | **0.49 s** | pystro |
| `dict_bench`         | **0.75 s** | 0.80 s | 0.84 s | 1.05 s | 0.98 s | py3.12 |

**JIT は micro でも negative**:

| ベンチ | py3.14 | py3.14+JIT | JIT 効果 |
|---|---:|---:|---:|
| `while_loop` | 0.85 | 0.90 | -6% (悪化) |
| `for_range` | 0.89 | 0.99 | -11% (悪化) |
| `for_range_pyrange` | 1.81 | 1.87 | -3% |
| `list_bench` | 0.86 | 1.02 | -19% (悪化) |
| `fib(35)` | 0.77 | 0.80 | -4% |
| `mandel` | 0.60 | 0.65 | -8% |
| `nqueens` | 0.61 | 0.64 | -5% |
| `recursive` (tak) | 2.70 | 2.63 | **+3%** (改善) |
| `string_bench` | 0.68 | 0.67 | +1% |
| `dict_bench` | 0.80 | 0.84 | -5% |

10 本中 JIT が improvement なのは `recursive` (tak、 deeply recursive)
のみ。 これは tail-recursive なホット trace が安定しているから JIT
最適化が効くと思われる。 他はベンチ規模 ~1s で warmup コスト負け。

dict_bench で唯一 pystro が負け (0.98 vs 0.75 = 1.31× slow vs 3.12、
1.23× slow vs 3.14)。 CPython の str-key 専用 dict layout が強い。

### pystro AOT 比較サマリ

|   | vs py3.12 | vs py3.14 | vs py3.14+JIT |
|---|---:|---:|---:|
| macro 勝率 | 4/4 | **3/4** | 3/4 |
| micro 勝率 | 9/10 | 9/10 | 9/10 |

3.14 の 25% improvement で raytrace は逆転されたが、 micro は全 10 本
で 3.14 ≥ pystro の差は dict_bench を除いてゼロ。 さらに pystro 側は
SD compile 時の AOT 効果が AST direct dispatch + IC で乗算的に効いて
いるので、 hot dispatch loop 重視のワークロードでは依然 pystro 優勢。

## 投入した最適化 (時系列、 直近順)

### Phase 8: macro 全勝 (2026-05-08)

`docs/why_slow.md` (旧版、 現 `vs_cpython.md`) で残っていた 3 つの
ボトルネック (deltablue 1.21× slow / raytrace 1.04× slow / pyaes
2.27× slow vs 3.12) を順に潰した。 最後の parser fix が deltablue の
4× を生んだ。

| 改善対象 | 変化 | 内容 |
|---|---|---|
| pyaes -22% | `node_subscript_get` に fixnum index fast path。 list/tuple/bytes/bytearray の `arr[i]` で `py_list_get` の full dispatch (3% of total) を回避 |
| pyaes -33% | `node_floordiv` / `node_mod` に fixnum fast path。 `i % 4` / `i // 4` 等が GMP `mpz_fdiv_r` 経由していたのを直接 C の `%` / `/` (Python 流の floor 補正付き) に。 GMP 1.7% + 1.6% を撤去 |
| pyaes -33% | **`node_iadd` 新設** + `T_PLUS_EQ` を ALLOC_node_iadd に切替。 `lst += [...]` を in-place extend に。 pyaes の `_remaining_counter += [...]` が O(N²) だったのを O(N) に。 memmove 12% → 2.5% |
| pyaes -44% | `attr_cache` に instance-receiver class-attr 用 monomorphic slot (`inst_ca_cls/val/sv`)。 `aes.T1`/`self.S` 等の class-level table 参照が MRO walk + strcmp していたのを 1 回の pointer compare で memoize。 py_class_lookup_method_slow 9% を撤去 |
| raytrace -8% | `pyclass.fast_new` flag。 user class の `slot_new` が default `bi_object_new` で built-in base なし、 例外でなければ、 instantiation で `__new__` dispatch 全スキップ → `py_make_instance` 直叩き。 `pyclass_refresh_slots` で事前計算 |
| deltablue -7% | `py_method_resolve` に **classmethod IC**。 `Cls.classmethod()` の hot path で `py_class_lookup_method` + bound-method alloc を毎回していたのを cache stamp で撤去。 `node_method_N` に `t == PY_T_CLASS` branch 追加して cls 引数 prepend |
| **deltablue -33%** | **parser LHS `dot+call` fuse**。 これが本 session の最大の飛躍。 詳細は次節 |

#### LHS の dot+call fuse (deltablue 0.21 → 0.14 s)

deltablue の constraint propagation hot loop:

```python
self.input().value = self.output().value
```

RHS 側 `self.output().value` (= 値として読む側) は `parse_dot_trailer`
が `node_attr_get(node_method_0(self, "output"), "value")` に正しく落とす。
LHS 側 `self.input().value = ...` (= 代入先) は `parse_assignable_target`
という別経路を通っていて、 中間の `self.input()` が
`node_call_0(node_attr_get(self, "input"))` に展開されていた。

`node_call_0(attr_get)` 経由は:
- `attr_get` で毎回 bound method を新規 alloc (`py_make_bound`)
- `py_apply` の inline fast path は `PY_T_FUNC` のみ命中、 bound は
  fast path 落ち → `py_apply_slow` 経由
- method PIC が無い (`node_call_0` には cache が無い)

修正は `parse_assignable_target` の build loop で `TR_DOT + TR_CALL`
を 1 つの `method_N` ノードに fuse:

```c
if (trs[i].kind == TR_DOT
        && i + 1 < ntr - 1 && trs[i + 1].kind == TR_CALL) {
    // 引数を parse して node_method_N へ
    ...
    i++;     // TR_CALL を消費済み
}
```

これで `self.input()` も `self.output()` も RHS と同形の `node_method_0`
になり、 4-way PIC + `py_apply` の closure fast path が両側で効く。
末尾 trailer (`f() = 1` のような禁止形) は意図通り残るので check は壊さない。

ベンチ (best-of-5):

|   | Phase 7 後 | Phase 8 後 | 改善 |
|---|---:|---:|---:|
| richards     | 0.48 s | **0.49 s** | ≈ 同等 |
| deltablue    | 0.57 s | **0.14 s** | **-75%、 4.07× faster** |
| raytrace     | 1.27 s | **0.86 s** | -32%、 1.48× faster |
| crypto_pyaes | 1.77 s | **0.40 s** | **-77%、 4.43× faster** |

### Phase 7: module method IC + attr_set new-instance fast path

| commit | 改善 | 内容 |
|---|---|---|
| `16d05a3` | raytrace -15% | `node_method_*` の fast path に `t == PY_T_MODULE` branch 追加。 `math.sqrt(x)` 等の module method を IC 化 |
| `c93297a` | raytrace -28% | attr_set fast path で **新 instance の attrs alloc を inline** 化 |
| `8c8348e` | macro 全般 -25〜-48% | `py_alloc` を per-type sizing に。 instance alloc を 312B → 32B |
| `1944b94` | raytrace 直撃 | `py_eq` / `py_add` / `py_sub` 等の literal `"__op__"` を `PYSTRO_INTERN_*` 化 |

### Phase 6: bytes/bit ops + algorithmic

| commit | 改善 | 内容 |
|---|---|---|
| `5cc5ebd` | deltablue -7.7% | `lm_pop` を memmove 化 |
| `5e8cc1b` | pyaes -35% | bit op (`& \| ^ << >>`) に fixnum fast path |

### Phase 5: 計測ベース細粒度 IC

| commit | 改善 | 内容 |
|---|---|---|
| `d849a47` | raytrace -3% | `node_add/sub/mul` に per-call-site `binop_cache` |
| `3e7f1b4` | raytrace -19.6% | attr_set の cache stamp を冪等化 |
| `8af7ef6` | deltablue -10% | attr_cache に class-data attr の monomorphic IC |

### Phase 4: macro 最適化 (IC 系の積み重ね)

| commit | 改善 | 内容 |
|---|---|---|
| `ae87e35` | richards -29% | user-class instance method の monomorphic IC |
| `2eadb18` | richards -10% | 4-way polymorphic IC に拡張 |
| `bf286e0` | strcmp 大幅減 | 24 種の dunder 名前を pre-intern + slot 配置 |
| `ba3897e` | deltablue -67% | `attrs_id` を class 共有の `shape_version` に置換 |
| `3e90b55` | richards -78% | attr_cache も polymorphic 化 (4-way) |

### Phase 3: Python iterator の高速化

- `node_attr_set` に attr_cache 追加
- `py_iter_next_inline` (kind 0/2 を SD inline)
- `struct py_iter` に `next_m` cache (kind=5 の `__next__` lookup を init で 1 回)
- `py_iter_next_user` に `no_stack_protector`

### Phase 2: 関数 body の SD 化

mandel -59%、 recursive -43%、 fib -32%、 nqueens -29%。

### Phase 1: micro 最適化

| 段階 | 効果 | 概要 |
|---|---:|---|
| §1 `gref_cache @ref` | fib 5× | `node_gref/gset` に inline cache |
| §2 `globals_serial` 構造変化のみ bump | while 71× | tight loop で全 cache invalidate を撤去 |
| §3 `node_for_global` 内蔵 cache | for_range 23× | ループ前に idx 解決 |
| §4 method_cache (builtin 用) | list 12× | bound 確保 + strcmp 撤去 |
| §5 `py_apply` を node.h に static inline | fib 1.15× | SD から PLT hop 排除 |
| §6 leaf func の alloca フレーム | fib 1.5× | GC_malloc → C スタック alloca |
| §7 dict identity-equal fast path | dict 1.1× | immediate キーで `py_eq_bool` skip |
| §8 string slice の buffer 共有 | string 1.6× | borrow ポインタ |
| §9 inline flonum + 算術 fast path | mandel 2.6× | 3-bit rotate encoding |
| §10 fixnum compare fast path | nqueens 1.8× | py_eq / py_cmp の GMP 経由を回避 |

## 現在の profile (Phase 8 完了後)

### deltablue (0.14 s)

| 関数 | 占有 |
|---|---:|
| `py_apply_slow` | 7.0% |
| `__strcmp_avx2` | 6.4% |
| baked SD (`SD_*`) | 合計 ~25% |
| `__memmove_avx_unaligned_erms` | 3.9% |
| `GC_malloc_kind` | 3.4% |
| `py_class_lookup_method_slow` | 3.4% |

`py_apply_slow` 7% の中身は bound method dispatch / 数少ない PIC miss
が主体。

### richards (0.48 s)

baked SD が時間の主導 (60%+)。 残り 30% は dispatch overhead だが、
これ以上は JIT 化や AST node fusion の世界。

### raytrace (0.86 s) — 3.14 に逆転された

Vector arithmetic で `__add__/__sub__/__mul__` は per-call-site
`binop_cache` で IC 命中。 attr_set new-instance fast path で descriptor
確認も撤去済み。 fast_new で Vector instantiation の `__new__` dispatch
も skip。 残存:
- `__truediv__`、 `__neg__` 等の minor dunder の linear MRO walk + strcmp
- Boehm GC の per-Vector alloc (per pixel ~10 Vector)

3.14 が 0.91 → 0.68 (25% speedup) で勝った理由は推測:
- Tier 2 uops で Vector の `__add__` 等を JIT 経由ではなく interpreter
  内で specialize して fold できる
- 浮動小数の boxed/unboxed の reuse pattern が効いた

詳細は [vs_cpython.md](./vs_cpython.md)。

### crypto_pyaes (0.40 s)

| 関数 | 占有 |
|---|---:|
| `__memmove_avx_unaligned_erms` | 12.3% |
| `__memset_avx2_unaligned_erms` | 11.8% |
| `GC_malloc_kind` | 6.4% |
| `__strcmp_avx2` | 5.8% + 2.0% (plt) |
| `py_class_lookup_method_slow` | 4.0% |

bytes/bytearray の memmove + memset が 24%、 GC alloc 6%。 これらは
libc + Boehm 層で SD からは触れにくいが、 既に 3.14 を 1.23× 上回る。

## 残ボトルネック

### dict_bench (1.31× slow vs py3.12)

CPython の dict 実装は数十年磨かれてきた:
- dunder lookup の C インライン
- サイズ別 layout (~7 種類)
- **str-key 専用 layout** で hash 事前計算 + strcmp 不要

pystro はジェネリック open-addressing 1 種のみ。 1× を逆転するには
str-key 専用パスが必要。

### raytrace の 3.14 逆転 (1.27× slow vs py3.14)

operator dunder の minor 系 (`__truediv__`、 `__neg__`) が pyclass の
slot 化されていない。 主要 dunder (24 種) は slot 化済みだが、 残り
~20 種は struct 拡大の副作用を避けて延期。 別 struct に切り出して
pyclass→slot table へポインタ 1 個だけ持たせる pattern が必要。

### Boehm GC のオーバーヘッド (pyaes 残存 24%)

bytes copy/zero と GC alloc は libc + Boehm で実装されているので
SD 層からは触れない。 Bartlett mostly-copying GC への切り替えは
ASTro 全体計画 (`docs/idea.md`) で検討中。

## これから

優先度順:

1. **operator dunder 残り** — slot table を別 struct 化して延期分を吸収。
   raytrace で py3.14 を抜き返す想定。
2. **dict_bench を 1× に** — str-key 専用 dict layout。
3. **JIT / type inference** — IPC を上げる方向。 ASTro 全体方針と要相談。

詳細な比較分析と現状残存ボトルネックは [`vs_cpython.md`](./vs_cpython.md)。

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
perf record -F 4000 --call-graph=fp -o /tmp/dw.data ./pystro bench/macro/richards.py
perf report -i /tmp/dw.data --no-children --stdio -G | grep -B1 -A20 "py_apply_slow" | head

# CPython 3.14 比較
PY314=$(uv python find 3.14)
PYTHON_JIT=1 $PY314 bench/macro/richards.py
PYTHON_JIT=0 $PY314 bench/macro/richards.py
```
