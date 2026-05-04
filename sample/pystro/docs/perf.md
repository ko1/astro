# perf.md — pystro 性能計測

## ベースライン

ハードウェア・ソフトウェア
- gcc 13 -O2 / SD は `-O3 -fPIC -fno-plt -march=native`
- Boehm GC + GMP
- 比較対象: `python3` (CPython 3.12.3)

実行モード
- `interp`: `--no-compile`、SD なし
- `AOT cached`: `code_store/all.so` 生成済みでの run (best-of-3)

## CPython 比 (~1 秒スケール)

`bench/*.py` は python3 で約 1 秒かかる規模に揃えてある。
`make bench` で同じ 3 列が出る。

| ベンチ | python3 | pystro interp | pystro AOT cached | **pystro/python3** |
|---|---:|---:|---:|---:|
| `while_loop` (10M, augassign) | 0.95 s | 0.18 s | 0.05 s | **0.05× (18× 速い)** |
| `for_range` (15M sum) | 1.01 s | 0.15 s | 0.09 s | **0.09× (11× 速い)** |
| `list_bench` (7M append+sum) | 0.88 s | 0.22 s | 0.19 s | **0.21× (4.7× 速い)** |
| `fib(35)` (再帰) | 1.18 s | 0.62 s | 0.62 s | **0.53× (1.9× 速い)** |
| `recursive` (tak(30,20,10)) | 4.06 s | 2.59 s | 2.49 s | **0.61× (1.6× 速い)** |
| `string_bench` (2M split) | 0.60 s | 0.52 s | 0.50 s | **0.83× (1.2× 速い)** |
| `mandel` (float-heavy) | 0.70 s | 0.62 s | 0.63 s | **0.90× (1.1× 速い)** |
| `nqueens` (recursion + list) | 0.69 s | 0.62 s | 0.62 s | **0.90× (1.1× 速い)** |
| `dict_bench` (3M put+get) | 0.77 s | 0.87 s | 0.82 s | 1.06× (僅差で負け) |

best-of-3。**9 ベンチ中 8 で python3 を上回る**。

### 解釈

**圧倒的に速い (5×〜18×)**
- **tight while loop** (`i += 1` × 10M): 18×。CPython の bytecode 解釈
  オーバーヘッドが目立つ。pystro は AOT で gref/gset/add/lt が直線的な
  C 関数呼び出しに畳まれ、inline cache で globals 読み書きが配列 index
  1 回に。
- **for-in-range**: 11×。`node_for_global` の内蔵 cache でループ
  ターゲット代入の strcmp が消えた。`py_iter_next` の overhead はまだ
  あるが、CPython の `FOR_ITER` + PyLong allocator も似たコストを払う。
- **list append + iterate**: 4.7×。method_cache が `xs.append(i)` の
  bound-method 確保を消したのと、list の C 表現がほぼ最適。

**やや速い (1.1×〜1.9×)**
- **fib / tak (再帰)**: gref_cache + leaf-func alloca が効く。py_apply
  inline で PLT hop も消えた。CPython と互角またはやや上。
- **mandel / nqueens**: 数値は inline flonum (CRuby 流 3-bit rotate)
  で heap-box が消えた。算術ノードが flonum-flonum fast path を直接
  inline。

**やや遅い (僅差)**
- **dict_bench**: 1.06× 遅い。CPython の dict 実装は数十年磨かれた C
  コード — 専用 layout、サイズ別 hash、PyObject_Hash インタイル等。
  pystro は open-addressing + 線形 hash + identity-eq fast path のみ。

## 投入した最適化 (時系列)

### §1 — `gref_cache @ref` (fib 5×)

`node_gref` / `node_gset` に `struct gref_cache *cache @ref` を追加。
`{ uint64_t serial; int idx; }` の inline cache で hot path は配列 index 1 回。

`pystro_gen.rb` で ASTroGen に `@ref` operands の扱い (hash 計算で 0、
dump スキップ、specialize 時に `&n->u.<kind>.<field>` を emit) を教える。

**Before**: `__strcmp_avx2` が 51% (perf プロファイル)。
**After**: 計測ノイズ以下。

### §2 — `globals_serial` を構造変化のみで bump (while 71×)

`py_global_set` が値更新でも bump していた → tight loop で `i = i + 1`
のたびに **全 gref_cache が invalidate**、毎反復 strcmp 再走査。
構造変化 (新 slot / 未定義→定義) のみで bump するよう修正。
`while_loop` bench: 6.36 s → 0.09 s = **71×**。

### §3 — `node_for_global` に内蔵 cache (for_range 23×)

`for i in range(N):` の i 代入が `py_global_set` 経由で毎回 strcmp 線形
走査だった。perf で 77% を食っていた。`node_for_global` に
`struct gref_cache *cache @ref` を追加、ループ前に idx 1 回解決、
ループ本体は直接 `c->globals[idx].value = elt`。
`for_range`: 1.81 s → 0.08 s = **23×**。

### §4 — メソッド呼出の inline cache (`method_cache @ref`) (list 12×)

`o.m(args)` が毎回 `py_getattr` → 線形 strcmp + `py_make_bound`
(heap alloc) していた。`node_method_*` に `struct method_cache *cache @ref`
を追加 (`type_tag` + `fn` raw pointer)。recv の type tag が一致したら
**bound オブジェクトを作らずに raw fn を直接呼ぶ**。
`list_bench`: 2.21 s → 0.19 s = **12×**。

### §5 — `py_apply` を `node.h` に static inline (fib 1.15×)

`py_apply` の closure-with-matching-arity fast path を `node.h` に
`static inline __attribute__((always_inline))` で移動。SD コードからの
PLT hop が消え、SD 内に直接展開される。cold case (builtin / bound /
class / 引数不一致 / varargs) は `py_apply_slow` にフォールバック。

### §6 — leaf func の alloca フレーム (fib 1.5×)

ネストした `def` / `class` を持たない関数 (= leaf) のコールフレームを
`GC_malloc` ではなく **C スタック上に `alloca`**。Boehm の保守的スタック
スキャンが VALUE スロットを生かす。クロージャでローカルをキャプチャ
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

`s.split()` や `s[i:j:1]` で **新しい char バッファを確保せず、元の
バッファに `(chars, len)` で borrow ポインタを張る**。Boehm の
interior-pointer サポートで親バッファが自動的に生存。

`py_make_str_borrow` は更にサイズ最適化: `offsetof(pyobj, str) +
sizeof(str)` (24 byte 程度) のみ確保、union の最大メンバ分の死領域を
避ける。Boehm のサイズ別 freelist で小さいバケットに入って cache 効率
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
`node_eq` 直下に inline fixnum 比較を入れ、py_eq 内も `if
(PY_IS_FIXNUM(a) && PY_IS_FIXNUM(b)) return PY_FALSE;` で GMP 経路を
回避。同じ修正を `py_cmp` にも。
`nqueens`: 1.05 s → 0.57 s。

## 残ボトルネック

### dict_bench の `pydict_lookup`

CPython は数十年磨かれた dict 実装で **dunder lookup の C インライン**、
**サイズ別 layout** (~7 種類)、**専用 string-keyed layout** などを持つ。
pystro はジェネリック open-addressing 1 種のみ。
1.1× を逆転するなら str-key 専用パスが必要。

### mandel の `py_make_float` 残留

inline flonum でほぼ消えたが、`py_apply` 経由の関数呼び出し境界で
double が boxed/unboxed されるケースが残る。`py_apply` の PLT hop は
inline 化で消えたが、関数の VALUE 受け渡しは依然 union 経由。

### 関数 inline cache (call site → resolved closure)

`gref_cache` は値が cache されるが、その値が closure object の場合、
`py_apply` の closure fast path に入るまでに 1〜2 個の type 判定が
ある。`node_call_*` に専用 cache を入れて closure body へ直接ジャンプ
できるようにすれば fib をもう少し速くできる。

## perf 採取例

```sh
perf record -g --call-graph dwarf -o /tmp/p.data ./pystro -c bench/fib35.py
perf report -i /tmp/p.data --no-children --stdio | head -40
```

`gref_cache` 投入前後の同コマンドで `__strcmp_avx2` が 51% → 計測
ノイズ以下、`gset` serial-bump fix の前後で `py_global_index` の
サンプル数が桁違いに減るのが目視できる。
