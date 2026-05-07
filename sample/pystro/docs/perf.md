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

| ベンチ | python3 | pystro interp | pystro AOT cached | **AOT/python3** |
|---|---:|---:|---:|---:|
| `while_loop` (10M, augassign) | 0.93 s | 0.18 s | 0.05 s | **0.05× (18× 速い)** |
| `for_range` (15M sum) | 0.96 s | 0.17 s | 0.12 s | **0.12× (8.0× 速い)** |
| `list_bench` (7M append+sum) | 0.89 s | 0.25 s | 0.21 s | **0.24× (4.2× 速い)** |
| `fib(35)` (再帰) | 1.17 s | 0.71 s | 0.70 s | **0.60× (1.7× 速い)** |
| `recursive` (tak(30,20,10)) | 3.78 s | 2.69 s | 2.73 s | **0.72× (1.4× 速い)** |
| `string_bench` (2M split) | 0.56 s | 0.55 s | 0.54 s | **0.96× (1.04× 速い)** |
| `mandel` (float-heavy) | 0.66 s | 0.64 s | 0.64 s | **0.97× (1.03× 速い)** |
| `nqueens` (recursion + list) | 0.67 s | 0.66 s | 0.65 s | **0.97× (1.03× 速い)** |
| `dict_bench` (3M put+get) | 0.76 s | 1.06 s | 1.07 s | 1.41× (40% 遅い) |

best-of-3。**9 ベンチ中 8 で python3 を上回る** (dict_bench は遅い)。

### R7-R10 → R18 の比較

R7〜R10 で取った旧計測 (R10 perf.md) との差分:

| ベンチ | R10 AOT | R18 AOT | 差 | 主因 |
|---|---:|---:|---:|---|
| `while_loop` | 0.05 s | 0.05 s | 同 | — |
| `for_range` | 0.09 s | 0.12 s | +33% | iter で IndexError catch する setjmp 追加 |
| `list_bench` | 0.19 s | 0.21 s | +11% | append の path で state チェック増 |
| `fib(35)` | 0.62 s | 0.70 s | +13% | call/attr 系の state チェックで分岐増 |
| `recursive` (tak) | 2.49 s | 2.73 s | +10% | 同上 (再帰で per-call overhead 積み上がり) |
| `string_bench` | 0.50 s | 0.54 s | +8% | str slice の path で state チェック増 |
| `mandel` | 0.63 s | 0.64 s | +2% | — |
| `nqueens` | 0.62 s | 0.65 s | +5% | — |
| `dict_bench` | 0.82 s | 1.07 s | +30% | metaclass __call__ ディスパッチに `PYSTRO_BI_KWC` save/restore 挿入 |

R11〜R18 で **CPython 互換性向上** (213/213 internal tests + 28/394
CPython tests fully pass) を入れた代償。 主な overhead 源:

1. **chained call/attr/subscript の state propagation**: `raiser().x`
   が `None.x` ではなく LookupError を伝播する fix で `node_attr_get`
   等に `if (UNLIKELY(c->state != PY_STATE_NORMAL)) return PY_NONE;`
   を追加。 codecs.lookup / argparse / configparser に必要。
2. **`__getitem__` iter の local try frame**: kind=13 で setjmp/longjmp
   で IndexError を捕える (CPython では C コードで cheap だが pystro
   は jmp_buf alloc + setjmp が hot path に)。
3. **`PYSTRO_BI_KWC` save/restore**: nested class call で outer の
   kwargs が leak しないように metaclass __call__ ディスパッチ前後で
   thread-local を save/restore。

これらは互換性のための trade-off。 hot path の追加分岐は基本的に
UNLIKELY hint で predict できる cold path なので、 absolute overhead
は数 % 〜 30% 範囲に収まっている。

### 解釈

**圧倒的に速い (4×〜18×)** — `while_loop` / `for_range` / `list_bench`
- AOT で gref/gset/add/lt が直線的な C 関数呼び出しに畳まれ、 inline
  cache で globals 読み書きが配列 index 1 回。
- `node_for_global` 内蔵 cache、 `method_cache` で `xs.append(i)` の
  bound-method 確保が消える。

**やや速い (1.4×〜1.7×)** — `fib` / `recursive`
- gref_cache + leaf-func alloca。 `py_apply` inline で PLT hop も消えた。

**僅差で速い (1.0×〜1.05×)** — `mandel` / `nqueens` / `string_bench`
- 数値は inline flonum で heap-box 消失。 算術ノードに flonum-flonum
  fast path。 string slice は buffer 共有。

**僅差で負け** — `dict_bench`
- CPython の dict 実装は数十年磨かれた C コード。 専用 layout、 サイズ別
  hash、 `PyObject_Hash` インライン等。 pystro は open-addressing +
  線形 hash + identity-eq fast path のみ。

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
