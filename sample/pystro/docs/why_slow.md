# why_slow.md — CPython より遅い理由の解析

2026-05-07 時点の計測ベース分析。 全数字は best-of-5 / `perf stat` /
`perf record -F 4000` で取得した実測値。

## 結論: なぜ遅いのか

| ベンチ | py3 instr | pystro instr | inst比 | py3 IPC | pystro IPC | total slowdown |
|---|---:|---:|---:|---:|---:|---:|
| **richards** | 12.4 B | **5.2 B** | **0.42×** | 3.21 | 2.67 | **0.50× (FASTER)** |
| deltablue | 2.0 B | 4.4 B | 2.20× | 2.96 | 1.60 | 4.07× |
| raytrace | 10.1 B | 19.3 B | 1.90× | 2.94 | 2.29 | 2.44× |
| crypto_pyaes | 7.0 B | 22.9 B | **3.27×** | 3.35 | 2.10 | 5.22× |

時間差は **ほぼ命令数差で説明できる**。 IPC 低下は副次要因 (1.2〜1.8×)
で、 致命的ではない。

richards で勝てるのは命令数が **CPython の半分以下**。 method dispatch
heavy なワークロードで pystro の IC が CPython のbytecode interpreter
+ frame setup よりも少ない命令で同じ仕事をする。

逆に deltablue / raytrace / pyaes では pystro が **2〜3× 多くの命令を
実行**している。 各ベンチで何が原因かを以下で個別に分析。

## ベンチ別 分析

### crypto_pyaes (5.2× 遅い、 22.9B vs 7.0B inst)

最も遅い。 命令数 3.27× 多。 hot symbols (perf record):

| 関数 | 占有 |
|---|---:|
| `GC_malloc_kind` (Boehm) | 7.3% |
| `__memmove_avx_unaligned_erms` | 6.3% |
| `__strcmp_avx2` | 5.4% |
| libgc 内部 | 4.5% |
| `__memset_avx2_unaligned_erms` | 4.2% |
| `__gmpz_init_set_si` (GMP) | 3.0% |
| `py_getattr` | 2.9% |
| `py_try_binop_dunder` | 2.0% |
| `py_class_lookup_method_slow` | 1.8% |
| `GC_free` | 1.5% |
| `py_bit_xor` | 1.5% |
| `py_bit_and` | 1.4% |

**カテゴリ別:**
- GC (Boehm 周り): **15.8%** (`GC_malloc_kind` + 内部 `074ce/7304` + `GC_free`)
- bytes copy/zero: **10.5%** (memmove + memset)
- 名前解決 (strcmp + lookup_method): 7.2%
- bit ops: 2.85%
- GMP: 3.0%

**根本原因:**

1. **bytes 型の毎回 alloc**: `aes_block ^ key_byte` のような XOR で
   pystro は毎回 `py_bit_xor` → 結果用に新 `bytes` オブジェクト alloc。
   CPython は refcount + small-object arena でほぼ free。 加えて
   CPython 3.12 は `BINARY_OP_INT` specialised bytecode で
   8-bit 整数の XOR をインライン展開する。
2. **GMP の overhead**: 全 int 演算が `mpz_t` 経由になる場合がある。
   pyaes は 8-bit 値を XOR するので fixnum で済むはずが、 大きい
   round key 等で bignum 化することがある (3% は `gmpz_init_set_si`)。
3. **Boehm GC**: refcount より 1 alloc あたり ~5× 遅い (per-alloc
   bookkeeping、 size class search など)。
4. **bytes slice の毎回 alloc**: `key[16:32]` のような slice が新
   bytes object を作る。 CPython も同じだが、 pystro の str slice の
   ような buffer 共有最適化を bytes は持たない。

**改善余地** (試算 — 計測した hotspot から):
- GC を Bartlett mostly-copying に切替: -10% (Boehm 15.8% の 2/3 が
  alloc bookkeeping、 半減見込み)
- bytes 専用 XOR/AND inline path (`py_bit_xor` の builtin shortcut):
  -5%
- GMP を delay (fixnum で完結する path を増やす): -3%

合計で 18% 改善見込み → 2.80 s → 2.30 s 程度。 まだ python3 の 4×
遅いが、 ある程度は埋まる。

### raytrace (2.3× 遅い、 19.3B vs 10.1B inst)

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 16.3% |
| libgc 内部 | 9.1% (074ce + 7304) |
| `py_class_lookup_method_slow` | 5.3% |
| `strcmp@plt` | 4.4% |
| `GC_malloc_kind` | 3.6% |
| `py_getattr` | 3.2% |
| `py_apply_slow` | 2.9% |
| `py_setattr` | 2.7% |
| `py_class_lookup_method.part.0` | 1.3% |
| `memmove` | 1.1% |
| `py_try_binop_dunder` | 1.1% |

**カテゴリ別:**
- 名前解決 (strcmp 系 + lookup_method): **27.3%**
- GC: **12.7%** (libgc + GC_malloc)
- attr_set / py_setattr: 2.7%
- py_apply_slow: 2.9%

**根本原因:**

1. **新 Vector の毎回 alloc**: `Vector.__add__` / `scale` / `cross` /
   `dot` 等が **新 Vector** を返す → 1 op = 1 instance alloc + 3 attr_set。
   raytrace の per-pixel が ~10 Vector 演算 → 100x100 image × 3 loops =
   300K pixels × 10 × N alloc。
2. **`py_class_lookup_method_slow` が 5% 残**: operator dunder
   (`__add__/__sub__/__mul__`) は `node_add/sub/mul` の binop_cache で
   IC 化済み (commit `d849a47`) だが、 minor dunders (`__neg__`,
   `__truediv__` の reflected `__rtruediv__` 等) や、 attr access の
   slow path が残る。
3. **strcmp 16.3%**: 一部は前述の minor dunder lookup、 一部は
   instance creation 経由の `py_setattr` の descriptor 確認 (これは
   commit `3e7f1b4` で 22% → 16% まで削減した)。

**改善余地:**
- Vector の小オブジェクト pool 化 (alloc を再利用): -5〜10%
- 残った operator dunder slot 化 (`__truediv__` 等): -2%
- bytes 専用 fast path 同様、 attr_set 専用 fast path を node_def に
  inline (slow path 撤去): -3%

合計で 10〜15% 改善見込み → 2.06 s → 1.7-1.85 s。

### deltablue (3.6× 遅い、 4.4B vs 2.0B inst)

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 7.2% |
| `lm_pop` (list pop) | **6.7%** |
| `py_class_lookup_method_slow` | 4.5% |
| `py_apply_slow` | 4.3% |
| libgc 内部 | 5.1% |
| GC_malloc | 2.4% |
| `py_getattr` | 2.2% |
| baked SD (`SD_*`) | ~10% |

**カテゴリ別:**
- 名前解決: ~13.5% (strcmp + lookup_method + getattr)
- GC: **7.5%**
- list ops (lm_pop): **6.7%** ← 特筆
- py_apply_slow: 4.3%

**根本原因:**

1. **`OrderedCollection.pop(0)` が O(N)**: deltablue の `add_propagate`
   は work queue として OrderedCollection を使う。 pop(0) で先頭を
   取り出すたびに残り要素を element-by-element shift。 6.7% 占有。
   CPython も `list.pop(0)` は O(N) だが **memmove で bulk shift** する
   ので per-element の py_VALUE check が無く高速。
   pystro は `for (i=0; i<n-1; i++) items[i] = items[i+1]` の loop。
2. **strcmp + lookup_method 13.5%**: deltablue は class method
   (`Strength.weakest_of`) と class data (`Strength.WEAKEST`) 両方を
   ホット path で使う。 commit `8af7ef6` で class data attr は
   monomorphic IC 化したが、 class method 呼び出しは別 path
   (node_method 系) で polymorphic 4-way IC は効いている。 残る strcmp
   は (a) class method の cache miss path、 (b) `Strength.weakest_of`
   の戻り値経由で更に dunder 検索される間接パス、 (c) `__init__`
   chain などの cold path 。
3. **`py_apply_slow` 4.3%**: Constraint subclass の constructor が
   多 (chain_test で n=5000 回 EqualityConstraint 作成)。 user-class
   instance method IC は効くが、 instantiation 自体は py_apply_slow に
   `__new__` / `__init__` lookup を経由する。

**改善余地:**
- `lm_pop` を memmove 化: -5% (6.7 → ~1.5)
- class instantiation の専用 path (object.__new__ 即 inline):
  -2〜3%
- 残った strcmp 経路の特定 + IC 化: -3%

合計で 10〜15% 改善見込み → 0.65 s → 0.55-0.60 s。 python3
0.18 s には依然届かないが、 deltablue は **algorithmic に
work queue を pop(0) で回す** ので O(N²) の影響をほぼ受けて
いる (CPython も同じ大変だが per-element コストが軽い)。

### richards (FASTER than python3、 0.51s vs 1.05s)

参考。 命令数 5.2B vs 12.4B = **0.42×**。 IPC 2.67 vs 3.21 = 0.83×。
合算 0.50× = 2× faster。

なぜ勝てるか:
- 全 method 呼び出しが 4-way PIC で IC 命中 (richards は Task
  hierarchy で 6 subclass 混合だが PIC 4-way で全部入る)
- 全 attr_get/set も 4-way PIC で命中 (Constraint subclass の
  `task.state` 等)
- python3 の bytecode interpreter が `LOAD_FAST` / `STORE_ATTR` /
  `CALL` を毎回 dispatch するのに対し、 pystro の SD-baked code は
  C 関数呼び出しの直線コード

richards は **method dispatch + attribute access のみで勝負が決まる**
古典 benchmark。 pystro の IC + AST direct dispatch 設計が完璧に
ハマる。

## 共通の構造的劣位

CPython 3.12 にあって pystro に無い機能 (特に macro で効くもの):

1. **specialized bytecode (PEP 659)**
   `LOAD_ATTR_INSTANCE_VALUE` / `BINARY_OP_INT` / `CALL_FUNCTION_EX`
   等の per-bytecode-instruction adaptive specialization。 pystro の
   IC は AST node 単位で粗いが、 CPython は 8 bytes per opcode で
   細かく特殊化。
2. **refcount + 小オブジェクト arena**
   per-alloc bookkeeping が Boehm の 1/5。 pyaes / raytrace のように
   alloc が hot な benchmark で 5〜10% 差が出る。
3. **builtin 型の C-implemented dunder**
   `int.__add__` / `bytes.__xor__` 等が ALL の path で C-inline。
   pystro は `py_bit_xor` 等の wrapper を経由する。
4. **string-key dict layout**
   CPython の dict は str-key 専用 path で hash が事前計算済み +
   strcmp 不要。 pystro は generic open-addressing 1 種のみ。
5. **C-level type slots (`tp_add` / `tp_init` 等)**
   pystro の dunder slot (commit `bf286e0`) は C struct に inline 解決
   値を持つだけで、 dispatch そのものは Python レベル。 CPython は
   slot を直接 C 関数として呼ぶので関数呼出 overhead が無い。

## 攻め所優先順 (data-driven)

`perf stat` の命令数差 + perf record の hot 関数 を組み合わせて
ROI 高い順:

1. **bytes 操作の inline path** (pyaes -8〜12%)
   pyaes の `^/&/<<` で毎回 `py_bit_xor` 経由 + 結果 alloc。 fixnum
   での bit op に専用 fast path を追加。
2. **list.pop(0) の memmove 化** (deltablue -5%)
   現状 element-by-element shift。 1 行で memmove に。
3. **Vector instance pool / 小 object arena** (raytrace -5〜10%)
   毎 op 新 Vector がボトルネック。
4. **GC を Bartlett に** (全体 -5〜10%)
   ASTro 計画 (cf. `idea.md`) で進行中。

JIT 化は別議論。 上の (1)〜(4) で macro slowdown が 5× → 3-4× 程度に
縮む見込み (CPython にはまだ届かないが、 dispatch overhead が支配的
だった部分は撤去できている)。
