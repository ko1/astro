# perf.md — pystro 性能計測

## ベースライン

ハードウェア・ソフトウェア
- gcc 13 -O2 / SD は `-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`
- Boehm GC + GMP
- 比較対象: `python3` (CPython 3.12.3)

実行モード
- `interp`: `--no-compile`、SD なし
- `AOT cached`: `code_store/all.so` 生成済みでの run (best-of-3)

## CPython 比 (~1 秒スケール、2026-05-07 R18 時点)

`bench/*.py` は python3 で約 1 秒かかる規模に揃えてある。
`make bench` で同じ 3 列が出る (CCACHE_DISABLE=1 で AOT bake)。

### micro (`bench/*.py`)

| ベンチ | python3 | pystro interp | pystro AOT cached | **AOT/python3** |
|---|---:|---:|---:|---:|
| `while_loop` (10M, augassign) | 0.92 s | 0.18 s | **0.05 s** | **0.06× (19× 速い)** |
| `for_range` (15M sum, C range) | 0.96 s | 0.14 s | **0.07 s** | **0.07× (14× 速い)** |
| `for_range_pyrange` (Py iter) | 2.12 s | 0.79 s | **0.35 s** | **0.17× (6.1× 速い)** |
| `list_bench` (7M append+sum) | 0.88 s | 0.22 s | **0.18 s** | **0.20× (5× 速い)** |
| `mandel` (float-heavy) | 0.65 s | 0.65 s | **0.25 s** | **0.39× (2.6× 速い)** |
| `recursive` (tak(30,20,10)) | 3.89 s | 2.69 s | **1.41 s** | **0.36× (2.8× 速い)** |
| `fib(35)` (再帰) | 1.15 s | 0.69 s | **0.39 s** | **0.34× (3.0× 速い)** |
| `nqueens` (recursion + list) | 0.67 s | 0.64 s | **0.40 s** | **0.60× (1.7× 速い)** |
| `string_bench` (2M split) | 0.58 s | 0.57 s | **0.54 s** | **0.93× (1.07× 速い)** |
| `dict_bench` (3M put+get) | 0.75 s | 1.06 s | 0.99 s | 1.32× (32% 遅い) |

best-of-3。**10 micro 中 9 で python3 を上回る** (dict_bench のみ遅い)。

### macro (`bench/macro/*.py`) — pyperformance 由来の実アプリ寄り

| ベンチ | python3 | pystro interp | pystro AOT cached | **AOT/python3** |
|---|---:|---:|---:|---:|
| `richards` (OS sched sim, ~400 行) | 1.09 s | 4.78 s | **2.31 s** | 2.12× (2.1× 遅い) |
| `deltablue` (constraint solver, ~600 行) | 0.16 s | 1.20 s | **0.74 s** | 4.63× (4.6× 遅い) |
| `raytrace` (簡易 raytracer, ~400 行) | 0.88 s | 4.05 s | **2.70 s** | 3.07× (3.1× 遅い) |
| `crypto_pyaes` (pure-Py AES-CTR) | 0.53 s | 4.07 s | 3.02 s | 5.7× (5.7× 遅い) |

`ae87e35` 〜 `ba3897e` で 4 段階の最適化を投入:
- user-class method の monomorphic IC (`ae87e35`) → richards -29%
- 4-way polymorphic IC (`2eadb18`) → richards -10% (polymorphic 解消)
- pre-resolved dunder slot + intern (`bf286e0`) → richards -10%, raytrace -10%
- attr_cache を attrs_id → class shape_version (`ba3897e`) → 大改善

最初の baseline (6.83 / 2.27 / 6.19 / 2.58) → 現在 (2.31 / 0.74 / 2.70 / 3.02):
**richards 3.0× / deltablue 3.1× / raytrace 2.3× faster** (best-of-3)。
crypto_pyaes は 17% 遅い (operator dunder `__add__/__or__/__xor__` が
slot 未対応で 24-way scan 全 miss → slow path に落ちる; 後述)。

#### 計測結果 — どこで時間を食ってるか

`perf record -F 4000 ./pystro bench/macro/<name>.py` の上位サンプル。

**deltablue (after `ae87e35` user-class IC、 1.82s、 7.06G samples)**:

| 関数 | 占有 (after) | (before) |
|---|---:|---:|
| `__strcmp_avx2` (libc) | 27.6% | 27.2% |
| `py_class_lookup_method` | 13.6% | 12.8% |
| `py_getattr` | 6.66% | 4.95% |
| `strcmp@plt` | 5.90% | 4.69% |
| `GC_malloc_kind` | 2.56% | **4.39%** |
| `py_apply_slow` | 削除 | 4.80% |

**richards (before IC、 7.34s、 26.8G samples)**:

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 28.8% |
| `py_class_lookup_method` | 15.1% |
| `strcmp@plt` | 6.47% |
| `GC_malloc_kind` | 5.03% |
| `py_hash` | 4.55% |
| `py_getattr` | 4.32% |

**根本原因 (発覚時)**: pystro の `method_cache` は **builtin type
専用**で、 user class のインスタンスメソッドには inline cache が
無かった。 `py_method_resolve` (runtime.c:3833) のコメント:

> // Instance / class method (no inline-cache-able fast path).

つまり `self.recalculate()` のような user-class method 呼び出し
は SD-baked code でも毎回 `py_getattr` → `py_class_lookup_method`
→ MRO walk + strcmp に落ちていた。

**修正 `ae87e35`**: `struct method_cache` を拡張 (`cls_ptr` 追加)、
user-class instance method を per-call-site で stamp。 fast path
で `cls_ptr == PY_PTR(o)->inst.cls` チェックして hit すれば
`py_apply` を直接呼ぶ (bound 確保 + MRO walk + strcmp 全回避)。

ベンチ:
- richards: 6.83 → 4.83 s (**-29%**)
- deltablue: 2.27 → 1.82 s (**-20%**)
- raytrace: 6.19 → 5.46 s (**-12%**)
- crypto_pyaes: 2.58 → 2.67 s (+3.5%、 noise の範囲)
- micro 全般: +5-9% 微 regression (cls_ptr load + branch のオーバーヘッド)

profile 上 `__strcmp_avx2` の占有率は変わってないが (27% → 28%)、
absolute サンプル数は減ってる (deltablue で 8.7G → 7.06G、 strcmp
の絶対値も 19% 減)。 また `GC_malloc_kind` が 4.39% → 2.56% に
半減した — 毎 call の `py_make_bound` 確保が消えた効果。

`py_class_lookup_method` がまだ 13.6% を占めるのは、 method_cache
が無い経路 (`__init__` / `__new__` / `__hash__` / 演算子 dunder
など special method lookup) からも呼ばれるから (runtime.c に
20 箇所超 call site あり)。 次の最適化は:

- method 名を intern (string 値 → 専用 ID) して strcmp → ポインタ
  比較に。 これで **全** lookup 経路が高速化。
- それで足りなければ per-class method dict (現状 array+strcmp)。

**crypto_pyaes** (2.74s, 10.1G samples) は profile が違う:

| 関数 | 占有 |
|---|---:|
| `__strcmp_avx2` | 11.0% |
| `GC_malloc_kind` | 7.85% |
| `__memmove_avx_unaligned_erms` | 4.91% |
| `__memset_avx2_unaligned_erms` | 4.90% |
| `py_class_lookup_method` | 4.89% |
| `__gmpz_init_set_si` (GMP) | 2.90% |
| baked SD | 数% |

bytes 操作 (memmove + memset = 9.8%)、 GC alloc (7.9%)、 GMP の
fixnum→bignum 変換 (2.9%) など多様な要因。 strcmp は 11% で
deltablue/richards より少ない (polymorphism 少なく monomorphic
パターン多)。

#### 帯域 (perf stat / deltablue)

|   | python3 | pystro AOT |
|---|---:|---:|
| 経過時間 | 0.18 s | 2.42 s |
| 命令数 | 2.0 B | **18.6 B (9.3× 多い)** |
| IPC | 2.95 | 1.92 |
| LLC miss / refs | n/a | **20.7%** |
| L1 dcache miss | n/a | 3.38% |
| branch miss | n/a | 0.36% |

命令数が 9.3× 多いのが最大要因。 IPC 低下 (1.92) と LLC miss 20%
は副次的だが、 AVX2 strcmp の per-call setup や Boehm GC が小さい
インスタンスをばら撒くことで cache locality が悪化していると
考えられる (この帯域分析は推測 — 命令数差だけで時間差の大半を
説明できる)。

#### これからの優先項目 (上の計測結果に基づく)

1. ~~user class instance method の inline cache~~ → **`ae87e35` で実装済**。
2. `py_class_lookup_method` 自体の高速化 — method 名を intern して
   strcmp 撤去。 special method lookup (operator dunder / __init__ /
   __hash__ など) も込みで効く。
3. instance allocation pattern の改善 (Boehm 頻度 2-8%、 small obj
   arena か frame reuse)。

### R10 → R18 の比較

R7〜R10 で取った旧計測 (R10 perf.md) との差分:

| ベンチ | R10 AOT | R18 AOT (修正後) | 差 | コメント |
|---|---:|---:|---:|---|
| `while_loop` | 0.05 s | 0.05 s | 同 | — |
| `for_range` | 0.09 s | 0.12 s | +33% | iter で IndexError catch する setjmp 追加 |
| `list_bench` | 0.19 s | 0.22 s | +16% | append の path で state チェック増 |
| `fib(35)` | 0.62 s | **0.42 s** | -32% | per-body SD (R18 後半 fix) で improved |
| `recursive` (tak) | 2.49 s | **1.41 s** | -43% | 同上 |
| `string_bench` | 0.50 s | 0.53 s | +6% | str slice の path で state チェック増 |
| `mandel` | 0.63 s | **0.26 s** | -59% | per-body SD で float fast path が SD 化 |
| `nqueens` | 0.62 s | **0.44 s** | -29% | 同上 |
| `dict_bench` | 0.82 s | 0.99 s | +21% | metaclass __call__ の `PYSTRO_BI_KWC` save/restore overhead |

call-heavy bench は R18 で大幅高速化。 きっかけは **AOT が関数 body
にも効くようにした fix** (5a2b83b):

#### 関数 body の SD 化 (mandel 2.6×, recursive/fib 2-3×)

R10 〜 R18 前半まで、 `astro_cs_compile(top_body, NULL)` は top-level
プログラム本体しか SD 化していなかった (`nm code_store/all.so | grep
-c " T "` が 1 個)。 関数 body は `py_make_func` に渡されるが、
`py_apply` が `EVAL(c, f->func.body)` を runtime ポインタ経由で
dispatch するので ASTroGen の specialization が見えない。 結果として
fib / recursive / mandel / nqueens は AOT でも tree-walking interp と
同じパスを通っていた。

修正 (R18 後半):

1. `node.c` に `struct code_repo` を追加 (koruby と同パターン)。
2. `py_make_func` を hook して各 def 時に body を code_repo に登録 +
   `astro_cs_load(body, NULL)` で dispatcher 即時 swap。
3. main.c の `-c` flow を「先に interp で 1 回走らせて py_make_func
   が populate」→「code_repo 全部 + main を astro_cs_compile」→
   「build / reload / load 各 body」→「exit」 に。

`nm code_store/all.so | grep -c " T "` が 1 → 関数の数 (fib なら 2、
mandel なら ~3) に。 mandel が 2.6× 速くなったのは関数 body が
inline flonum 演算ノードと共に SD 化されたため。

#### Python iterator の高速化 (16bf5a3 + a7326de + 43a7e83 + c3d7170 + cfbc026)

`class PyRange` のような pure-Python iterator (`__iter__` / `__next__`)
で AOT が python3 と同等止まりだったのを 4.6× → **6.6× 速い** まで
持ってきた:

1. **`node_attr_set` に attr_cache を追加**: `self.i = x` の毎反復に
   `__setattr__` / data descriptor / `__slots__` チェックを strcmp で
   走査していた (perf で 17% 占有) → fast path で
   `entries[eidx].value = vv` 直接書込に。 cache stamp 時にだけ
   semantic 検証 (override 系があれば cache 無効化)。
2. **attr_get/set fast path から strlen+memcmp 除去**: cache の attrs_id
   が一致 ⇒ backing storage 同一 ⇒ eidx は同じスロット。 key 名検証
   は冗長で削れる (DICT_DELETED_KEY だけ確認)。
3. **`py_iter_next_inline` を node.h に追加**: kind 0/2 (list/tuple/
   range) は user code を呼ばないので SD-baked for-loop に inline 安全。
   kind 5 (user iter) は `py_apply` の alloca が tight loop で stack
   蓄積するので out-of-line のまま。
4. **struct py_iter に `next_m` キャッシュ**: kind=5 の `__next__`
   lookup を init で 1 回だけに。
5. **`py_iter_next_user` に `no_stack_protector`**: alloca が
   `-fstack-protector-strong` を triggers し canary 読み書き check が
   毎 call ~5 cycle 入っていた。 untrusted index で stack array に
   書く処理は無いので canary 不要 → 0.40s → 0.34s (15% 改善)。

副作用: ASTroGen-generated node_eval.c で `extern VALUE` 宣言なしの
関数呼出が int 戻り値扱いで高位 32 bit 切り捨てる compiler バグを
発見、 `py_class_lookup_method_pub` を使うように修正。

### 解釈

**圧倒的に速い (5×〜16×)** — `while_loop` / `for_range` / `list_bench`
- AOT で gref/gset/add/lt が直線的な C 関数呼び出しに畳まれ、 inline
  cache で globals 読み書きが配列 index 1 回。
- `node_for_global` 内蔵 cache、 `method_cache` で `xs.append(i)` の
  bound-method 確保が消える。
- `py_iter_next_inline` が list/tuple/range の case を SD に inline。

**よく速い (1.6×〜5.6×)** — Python iter / `fib` / `recursive` /
`mandel` / `nqueens`
- 関数 body が SD 化された (R18 fix)。 gref_cache + leaf-func alloca、
  `py_apply` inline で PLT hop 排除、 inline flonum で heap-box 消失。
- `for x in PyRange(N)` のような Python iterator が **6.6× 速い**。
  attr_get / attr_set 両方 inline cache + fast path から strlen+memcmp
  除去 + `__next__` キャッシュ + hot 関数の canary 除去。
- mandel は float fast path がループ内で完全 SD 化されるので 2.7×。

**僅差で速い (1.04×)** — `string_bench`
- string slice は buffer 共有。 split/join 等の str op がほぼ memcpy。

**僅差で負け** — `dict_bench`
- CPython の dict 実装は数十年磨かれた C コード。 専用 layout、 サイズ別
  hash、 `PyObject_Hash` インライン等。 pystro は open-addressing +
  線形 hash + identity-eq fast path のみ。 加えて R18 で metaclass
  __call__ ディスパッチに `PYSTRO_BI_KWC` save/restore を入れたぶん
  class 呼び出しが少し重くなった。

**大きく負け (4-14×)** — macro (richards / deltablue / raytrace / pyaes)
- 詳細は上の「## 計測結果 — どこで時間を食ってるか」項参照。
- 一行で言うと: pystro は **user class instance method の inline cache
  が無い** ので、 `self.foo()` の解決が毎回 `py_class_lookup_method`
  の MRO+strcmp 線形走査に落ちる。 deltablue / richards でこの経路
  が 40-45% を占める。

## 投入した最適化 (時系列)

### §1 — `gref_cache @ref` (fib 5×)

`node_gref` / `node_gset` に `struct gref_cache *cache @ref` を追加。
`{ uint64_t serial; int idx; }` の inline cache で hot path は配列 index 1 回。

`pystro_gen.rb` で ASTroGen に `@ref` operands の扱い (hash 計算で 0、
dump スキップ、 specialize 時に `&n->u.<kind>.<field>` を emit) を教える。

**Before**: `__strcmp_avx2` が 51% (perf プロファイル)。
**After**: 計測ノイズ以下。

### §2 — `globals_serial` を構造変化のみで bump (while 71×)

`py_global_set` が値更新でも bump していた → tight loop で `i = i + 1`
のたびに **全 gref_cache が invalidate**、 毎反復 strcmp 再走査。
構造変化 (新 slot / 未定義→定義) のみで bump するよう修正。
`while_loop` bench: 6.36 s → 0.09 s = **71×**。

### §3 — `node_for_global` に内蔵 cache (for_range 23×)

`for i in range(N):` の i 代入が `py_global_set` 経由で毎回 strcmp 線形
走査だった。 perf で 77% を食っていた。 `node_for_global` に
`struct gref_cache *cache @ref` を追加、 ループ前に idx 1 回解決、
ループ本体は直接 `c->globals[idx].value = elt`。
`for_range`: 1.81 s → 0.08 s = **23×**。

### §4 — メソッド呼出の inline cache (`method_cache @ref`) (list 12×)

`o.m(args)` が毎回 `py_getattr` → 線形 strcmp + `py_make_bound`
(heap alloc) していた。 `node_method_*` に `struct method_cache *cache @ref`
を追加 (`type_tag` + `fn` raw pointer)。 recv の type tag が一致したら
**bound オブジェクトを作らずに raw fn を直接呼ぶ**。
`list_bench`: 2.21 s → 0.19 s = **12×**。

### §5 — `py_apply` を `node.h` に static inline (fib 1.15×)

`py_apply` の closure-with-matching-arity fast path を `node.h` に
`static inline __attribute__((always_inline))` で移動。 SD コードからの
PLT hop が消え、 SD 内に直接展開される。 cold case (builtin / bound /
class / 引数不一致 / varargs) は `py_apply_slow` にフォールバック。

### §6 — leaf func の alloca フレーム (fib 1.5×)

ネストした `def` / `class` を持たない関数 (= leaf) のコールフレームを
`GC_malloc` ではなく **C スタック上に `alloca`**。 Boehm の保守的スタック
スキャンが VALUE スロットを生かす。 クロージャでローカルをキャプチャ
しないので alloca のライフタイムが call と一致して安全。

### §7 — dict identity-equal fast path (dict 1.1×)

`pydict_lookup` の equality check が `py_eq_bool` 経由で関数呼出。
fixnum / None / True/False のような immediate キーは VALUE 比較だけで
等価判定可能。

```c
if (e->hash == h) {
    if (e->key == key) return e;       // immediate-equal
    if (immediate(key) || immediate(e->key)) continue;
    if (py_eq_bool(c, e->key, key)) return e;
}
```

`dict_bench` の `pydict_lookup` overhead: 27% → 12% に低下。

### §8 — string slice の buffer 共有 (string 1.6×)

`s.split()` や `s[i:j:1]` で **新しい char バッファを確保せず、 元の
バッファに `(chars, len)` で borrow ポインタを張る**。 Boehm の
interior-pointer サポートで親バッファが自動的に生存。

`py_make_str_borrow` は更にサイズ最適化: `offsetof(pyobj, str) +
sizeof(str)` (24 byte 程度) のみ確保、 union の最大メンバ分の死領域を
避ける。 Boehm のサイズ別 freelist で小さいバケットに入って cache 効率
も上がる。
`string_bench`: 0.82 s → 0.50 s。

### §9 — inline flonum + 算術ノード fast path (mandel 2.6×, nqueens 1.8×)

CRuby 流の 3-bit rotate flonum encoding (`scm_try_flonum`) を導入し、
`node_add/sub/mul/truediv/lt/le/gt/ge` に fixnum と並ぶ
flonum-flonum の inline fast path を追加。

```c
if (LIKELY(PY_IS_FLONUM(av) & PY_IS_FLONUM(bv)))
    return py_make_float(py_flonum_to_double(av) + py_flonum_to_double(bv));
```

double 値が encoding 範囲 (~[1e-77, 1e+77]) に収まるなら heap alloc 0。

### §10 — node_eq / py_eq の fixnum fast path (nqueens 1.8×)

`py_eq` は不等な fixnum どうしで GMP `mpz_init` を呼んでいた。
`node_eq` 直下に inline fixnum 比較を入れ、 py_eq 内も `if
(PY_IS_FIXNUM(a) && PY_IS_FIXNUM(b)) return PY_FALSE;` で GMP 経路を
回避。 同じ修正を `py_cmp` にも。
`nqueens`: 1.05 s → 0.57 s。

## 残ボトルネック

### dict_bench の `pydict_lookup`

CPython は数十年磨かれた dict 実装で **dunder lookup の C インライン**、
**サイズ別 layout** (~7 種類)、 **専用 string-keyed layout** などを持つ。
pystro はジェネリック open-addressing 1 種のみ。 R18 で
`PYSTRO_BI_KWC` save/restore を入れたぶん metaclass __call__ ディスパッチ
が重くなった (これは class 呼び出しが多い test では効く)。
1× を逆転するなら str-key 専用パスが必要。

### nqueens / mandel の AOT が更に速くできる余地

per-body SD で 2.6× / 1.6× まで来たが、 まだ python3 比 0.38× /
0.63× で改善余地がある。 inline flonum encoding の判定や `py_apply`
内のフレーム alloc が hot path に残っている。

### chained-raise propagation の overhead

R18 で `raiser().attr` の例外伝播を直したぶん、 hot な `node_attr_get` /
`node_subscript_get` / `node_method_*` に state check が増えた。
UNLIKELY hint で cold path に分岐するが、 fib/tak のような call+attr
密な benchmark で 5〜15% の overhead。 削るなら state check を SD-time
の dataflow analysis で省略できる箇所だけ inline 化する手があるが、
v0 では trade-off を受け入れる。

### mandel の `py_make_float` 残留

inline flonum でほぼ消えたが、 `py_apply` 経由の関数呼び出し境界で
double が boxed/unboxed されるケースが残る。 `py_apply` の PLT hop は
inline 化で消えたが、 関数の VALUE 受け渡しは依然 union 経由。

### 関数 inline cache (call site → resolved closure)

`gref_cache` は値が cache されるが、 その値が closure object の場合、
`py_apply` の closure fast path に入るまでに 1〜2 個の type 判定が
ある。 `node_call_*` に専用 cache を入れて closure body へ直接ジャンプ
できるようにすれば fib をもう少し速くできる。

## perf 採取例

```sh
perf record -g --call-graph dwarf -o /tmp/p.data ./pystro -c bench/fib35.py
perf report -i /tmp/p.data --no-children --stdio | head -40
```

`gref_cache` 投入前後の同コマンドで `__strcmp_avx2` が 51% → 計測
ノイズ以下、 `gset` serial-bump fix の前後で `py_global_index` の
サンプル数が桁違いに減るのが目視できる。
