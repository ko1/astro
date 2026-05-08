# perf.md — pystro 性能計測

## まとめ

2026-05-08 時点 (best-of-5、 `make bench` + `bench/macro/`):

- **macro 4 本中 4 で python3 を上回る** (1.06×〜2.18× faster)
- **micro 10 本中 9 で python3 を上回る** (1.13×〜16× faster)
- 残る負け: `dict_bench` のみ (1.27× slow) — generic open-addressing
  dict が CPython の str-key 専用 layout に届かない

最も大きな飛躍は本 session の **deltablue 0.57 s → 0.14 s** (4×)。
LHS の `self.output().value = ...` を parser 段階で `method_N` に
fuse することで、 全 IC 最適化が hot loop に効くようになった。

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

## 現状ベンチ (2026-05-08)

### macro (`bench/macro/*.py`) — pyperformance 由来

実アプリ寄り。 user-class method dispatch、 多態、 operator
overload、 bignum、 bytes 操作が支配的。

| ベンチ | python3 | pystro interp | pystro AOT | **AOT/python3** |
|---|---:|---:|---:|---:|
| `richards`     (OS sched sim, ~400 行)        | 1.07 s | 0.85 s | **0.49 s** | **0.46× (2.18× FASTER)** |
| `crypto_pyaes` (pure-Py AES-CTR, ~900 行)     | 0.54 s | 0.95 s | **0.37 s** | **0.69× (1.46× FASTER)** |
| `deltablue`    (constraint solver, ~600 行)   | 0.17 s | 0.43 s | **0.14 s** | **0.82× (1.21× FASTER)** |
| `raytrace`     (簡易 raytracer, ~400 行)      | 0.89 s | 1.79 s | **0.84 s** | **0.94× (1.06× FASTER)** |

全 4 本で勝ち。 richards は 2.18× faster — method dispatch heavy
な OS scheduler simulation で AST direct dispatch + 4-way PIC が
CPython の bytecode interpreter + frame setup を上回る。

### micro (`bench/*.py`)

`bench/*.py` は python3 で約 1 秒かかる規模。

| ベンチ | python3 | pystro interp | pystro AOT | **AOT/python3** |
|---|---:|---:|---:|---:|
| `while_loop` (10M, augassign)  | 0.97 s | 0.20 s | **0.06 s** | **0.06× (16.2× 速い)** |
| `for_range` (15M sum, C range) | 0.96 s | 0.13 s | **0.07 s** | **0.07× (13.7× 速い)** |
| `for_range_pyrange` (Py iter)  | 2.28 s | 0.89 s | **0.41 s** | **0.18× (5.6× 速い)** |
| `list_bench` (7M append+sum)   | 0.95 s | 0.23 s | **0.19 s** | **0.20× (5.0× 速い)** |
| `recursive` (tak(30,20,10))    | 4.19 s | 2.76 s | **1.56 s** | **0.37× (2.7× 速い)** |
| `fib(35)` (再帰)               | 1.14 s | 0.70 s | **0.40 s** | **0.35× (2.9× 速い)** |
| `mandel` (float-heavy)         | 0.72 s | 0.65 s | **0.27 s** | **0.38× (2.7× 速い)** |
| `nqueens` (recursion + list)   | 0.72 s | 0.63 s | **0.36 s** | **0.50× (2.0× 速い)** |
| `string_bench` (2M split)      | 0.61 s | 0.58 s | **0.54 s** | **0.89× (1.13× 速い)** |
| `dict_bench` (3M put+get)      | 0.74 s | 1.02 s | 0.94 s | 1.27× (CPython の方が速い) |

## 投入した最適化 (時系列、 直近順)

### Phase 8: macro 全勝 (今 session、 2026-05-08)

`docs/why_slow.md` (旧版、 現 `vs_cpython.md`) で残っていた 3 つの
ボトルネック (deltablue 1.21× slow / raytrace 1.04× slow / pyaes
2.27× slow) を順に潰した。 最後の parser fix が deltablue の 4× を
生んだ。

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
| raytrace     | 1.27 s | **0.84 s** | -34%、 1.51× faster |
| crypto_pyaes | 1.77 s | **0.37 s** | **-79%、 4.78× faster** |

### Phase 7: module method IC + attr_set new-instance fast path (前 session)

| commit | 改善 | 内容 |
|---|---|---|
| `16d05a3` | raytrace -15% | `node_method_*` の fast path に `t == PY_T_MODULE` branch 追加。 `math.sqrt(x)` 等の module method を IC 化。 cache stamp 時に (module_ptr, resolved) を保存、 hot path は self prepend なしで py_apply |
| `c93297a` | raytrace -28% | attr_set fast path で **新 instance の attrs alloc を inline** 化。 過去に同 (cls, sv) で stamp 済なら descriptor / __setattr__ 確認は不要 → pydict_new + insert を直接実行 |
| `8c8348e` | macro 全般 -25〜-48% | `py_alloc` を per-type sizing に。 instance alloc を 312B → 32B (deltablue -48%, raytrace -27%, pyaes -25%) |
| `1944b94` | raytrace 直撃 | `py_eq` / `py_add` / `py_sub` 等の literal `"__op__"` を `PYSTRO_INTERN_*` 化 |

### Phase 6: bytes/bit ops + algorithmic (前々 session)

| commit | 改善 | 内容 |
|---|---|---|
| `5cc5ebd` | deltablue -7.7% | `lm_pop` を memmove 化 (element-by-element shift loop → bulk copy) |
| `5e8cc1b` | pyaes -35% | bit op (`& \| ^ << >>`) に fixnum fast path。 8-bit 値 XOR/AND が毎回 mpz_t 経由していたのを撤去 |

### Phase 5: 計測ベース細粒度 IC

`PYSTRO_DBG_NAMES` で slow lookup される名前を頻度順で集計し、
頻出箇所に IC 追加:

| commit | 改善 | 内容 |
|---|---|---|
| `d849a47` | raytrace -3% | `node_add/sub/mul` に per-call-site `binop_cache` |
| `3e7f1b4` | raytrace -19.6% | attr_set の cache stamp を冪等化 (descriptor 確認 skip) |
| `8af7ef6` | deltablue -10% | attr_cache に class-data attr の monomorphic IC (`cls_recv`, `class_value`, `cls_recv_sv`) 追加 |

### Phase 4: macro 最適化 (IC 系の積み重ね)

実アプリ benchmark の `__strcmp_avx2` 27-29% + `py_class_lookup_method`
13-15% = 合計 40-45% を順次撤去:

| commit | 改善 | 内容 |
|---|---|---|
| `ae87e35` | richards -29% | user-class instance method の monomorphic IC を method_cache に追加 |
| `2eadb18` | richards -10% | 4-way polymorphic IC に拡張 |
| `bf286e0` | strcmp 大幅減 | 24 種の dunder 名前を pre-intern + struct pyclass に slot 配置 |
| `ba3897e` | deltablue -67% | `attrs_id` を class 共有の `shape_version` に置換 |
| `3e90b55` | richards -78% | attr_cache も polymorphic 化 (4-way) |

### Phase 3: Python iterator の高速化

- `node_attr_set` に attr_cache 追加
- `py_iter_next_inline` (kind 0/2 を SD inline)
- `struct py_iter` に `next_m` cache (kind=5 の `__next__` lookup を init で 1 回)
- `py_iter_next_user` に `no_stack_protector`

### Phase 2: 関数 body の SD 化

`py_apply` が `EVAL(c, f->func.body)` を runtime ポインタ経由で
dispatch していた → 関数 body は AOT で SD 化されない。
`code_repo` (koruby パターン) で全 body を集めて compile / load:
mandel -59%、 recursive -43%、 fib -32%、 nqueens -29%。

### Phase 1: micro 最適化

| 段階 | 効果 | 概要 |
|---|---:|---|
| §1 `gref_cache @ref` | fib 5× | `node_gref/gset` に inline cache |
| §2 `globals_serial` 構造変化のみで bump | while 71× | tight loop で全 cache invalidate を撤去 |
| §3 `node_for_global` 内蔵 cache | for_range 23× | ループ前に idx 解決 |
| §4 method_cache (builtin 用) | list 12× | `xs.append(i)` で bound 確保 + strcmp 撤去 |
| §5 `py_apply` を node.h に static inline | fib 1.15× | SD から PLT hop 排除 |
| §6 leaf func の alloca フレーム | fib 1.5× | GC_malloc → C スタック alloca |
| §7 dict identity-equal fast path | dict 1.1× | immediate キーで `py_eq_bool` skip |
| §8 string slice の buffer 共有 | string 1.6× | borrow ポインタ + Boehm interior-pointer |
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
が主体。 これ以上削るには JIT。

### richards (0.49 s)

| 関数 | 占有 |
|---|---:|
| baked SD (`SD_*`) | 合計 60%+ |
| `__strcmp_avx2` | 7-8% |
| `py_getattr` | 5% |
| `GC_malloc_kind` | 5% |
| `py_class_lookup_method_slow` | 4-5% |

baked SD が時間の主導。 残り 30% は dispatch overhead だが、 これ以上は
JIT 化や AST node fusion の世界。

### raytrace (0.84 s)

Vector arithmetic で `__add__/__sub__/__mul__` が `binop_cache` で IC 命中、
attr_set new-instance fast path で descriptor 確認も撤去済み。
fast_new で Vector instantiation の `__new__` dispatch も skip。
strcmp 残存は `__truediv__` などの reflected dunder。

### crypto_pyaes (0.37 s)

| 関数 | 占有 |
|---|---:|
| `__memmove_avx_unaligned_erms` | 12.3% |
| `__memset_avx2_unaligned_erms` | 11.8% |
| `GC_malloc_kind` | 6.4% |
| `__strcmp_avx2` | 5.8% + 2.0% (plt) |
| `py_class_lookup_method_slow` | 4.0% |
| `py_getattr` | 4.2% |

bytes/bytearray の memmove + memset が 24%、 GC alloc 6%。 これらは
libc + Boehm 層で SD からは触れにくい。 既に python3 を 1.46× 上回って
いるのでここから先は別議論。

## 帯域 (perf stat)

deltablue で python3 と pystro AOT の比較:

|   | python3 | pystro AOT |
|---|---:|---:|
| 経過時間 | 0.17 s | 0.14 s |
| 命令数 | ~2.0 B | ~1.6 B |
| IPC | ~3.0 | ~2.7 |

命令数で逆転、 IPC は CPython がやや上 (bytecode dispatch の予測しやすさ)。

## 残ボトルネック

### dict_bench (1.27× slow)

CPython の dict 実装は数十年磨かれてきた:
- dunder lookup の C インライン
- サイズ別 layout (~7 種類)
- **str-key 専用 layout** で hash 事前計算 + strcmp 不要

pystro はジェネリック open-addressing 1 種のみ。 1× を逆転するには
str-key 専用パスが必要。 micro 専用最適化として ROI は中。

### Boehm GC のオーバーヘッド (pyaes 残存 24%)

bytes copy/zero と GC alloc は libc + Boehm で実装されているので
SD 層からは触れない。 Bartlett mostly-copying GC への切り替えは
ASTro 全体計画 (`docs/idea.md`) で検討中。 pystro 単独では撤去不能。

### operator dunder の minor 系

`__truediv__`、 `__rmod__`、 `__matmul__` 等が pyclass の slot 化
されておらず MRO walk + strcmp。 主要 dunder (24 種) は slot 化
済みだが、 残り ~20 種は struct 拡大の副作用を避けて延期。
別 struct に切り出して pyclass→slot table へポインタ 1 個だけ
持たせる pattern が必要。

## これから

優先度順:

1. **dict_bench を 1× に** — str-key 専用 dict layout。
2. **operator dunder 残り** — slot table を別 struct 化して延期分を吸収。
3. **JIT / type inference** — IPC を上げる方向。 ASTro 全体方針と要相談。

詳細な攻め所と現状残存ボトルネックの分析は `docs/vs_cpython.md` を参照。

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
```
