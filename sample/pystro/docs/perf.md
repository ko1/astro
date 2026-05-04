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

ベンチは `bench/*.py`。python3 で約 1 秒かかるサイズに揃えてある。

| ベンチ | python3 | pystro interp | pystro AOT cached | **倍率 (python3/pystro)** |
|---|---:|---:|---:|---:|
| `fib(35)` (再帰) | 1.26 s | 0.69 s | 0.66 s | **1.91× 速い** |
| `while_loop` (10M, augassign) | 0.96 s | 0.18 s | 0.05 s | **19× 速い** |
| `for_range` (15M sum) | 0.96 s | 0.15 s | 0.09 s | **10.7× 速い** |
| `list_bench` (7M append+sum) | 0.96 s | 0.23 s | 0.20 s | **4.8× 速い** |
| `string_bench` (2M split) | 0.61 s | 0.52 s | 0.50 s | **1.22× 速い** |
| `dict_bench` (3M put+get) | 0.85 s | 0.93 s | 0.75 s | **1.13× 速い** |

**全 6 ベンチで python3 を上回った。** 再帰・tight loop で大差、最も僅差なのが
dict (CPython の C 実装が強い)。

## 投入した最適化 (時系列)

### §1 — `gref_cache @ref` (fib 5×)

`node_gref` / `node_gset` に `struct gref_cache *cache @ref` を追加。
`{ uint64_t serial; int idx; }` の inline cache で hot path は配列 index 1 回。

`pystro_gen.rb` で ASTroGen に `@ref` operands の扱い (hash 計算で 0 寄与、
dump スキップ、specialize 時に `&n->u.<kind>.<field>` を emit) を教える。
ascheme と同じパターン。

**Before**: `__strcmp_avx2` が 51% (perf プロファイル)。
**After**: 計測ノイズ以下。

### §2 — `globals_serial` を構造変化のみで bump (while 71×)

初期実装は `py_global_set` が値更新でも bump していた → tight loop で
`i = i + 1` のたびに **全 gref_cache が invalidate**、毎反復 strcmp 再走査。

`py_global_define` を「新 slot 確保 / 未定義→定義への遷移」のみで bump
するよう修正。値更新は bump しない。

`while_loop` bench: 6.36 s → 0.09 s = **71×**。

### §3 — `node_for_global` に内蔵 cache (for_range 23× → 12×)

`for i in range(N):` の i 代入は `py_global_set` 呼出 (cache 無し) で毎回
strcmp 線形走査だった。perf で 77% を食っていた。

`node_for_global` に `struct gref_cache *cache @ref` を追加。`py_iter_init`
直後に `py_global_resolve_or_alloc` で idx を 1 回解決、ループ本体は直接
`c->globals[idx].value = elt` の配列ストア。

`for_range` bench: 1.81 s → 0.08 s = **23×**。

### §4 — メソッド呼出の inline cache (`method_cache @ref`) (list 12×)

`o.m(args)` は毎回 `py_getattr` → 線形 strcmp + `py_make_bound` (heap alloc)
していた。perf で 8% が GC_malloc。

`node_method_*` に `struct method_cache *cache @ref` を追加 (`type_tag` +
`fn` raw pointer)。recv の type tag が一致したら、**bound オブジェクトを
作らずに raw fn を直接呼ぶ** (recv を argv[0] に積む形で)。

list_bench: 2.21 s → 0.19 s = **12×**。

### §5 — `py_apply` を `node.h` に static inline (fib 1.5×)

`py_apply` の closure-with-matching-arity fast path を `node.h` に
`static inline __attribute__((always_inline))` で移動。SD コードからの
PLT hop が消え、SD 内に直接展開される。cold case (builtin / bound /
class / 引数不一致) は `py_apply_slow` にフォールバック。ascheme の
`scm_apply_tail` と同パターン。

fib35: 1.16 s → 1.01 s。

### §6 — leaf func の alloca フレーム (fib 1.5×)

ネストした `def` / `class` を持たない関数 (= leaf) のコールフレームを
`GC_malloc` ではなく **C スタック上に `alloca`**。Boehm の保守的スタック
スキャンが VALUE スロットを生かす。pystro はクロージャでローカルを
キャプチャしないので alloca のライフタイムが call と一致して安全。

parser が pre-scan で `has_nested_def` を立て、`node_def` / `node_lambda`
の `leaf` operand に渡す。`py_make_func` がフラグを `pyobj.func.leaf` に
保存、`py_apply` の inline 版がそれを見て alloca / GC_malloc を切り替え。

fib35: 1.01 s → 0.66 s。

### §7 — dict lookup に identity-equal fast path (dict 1.1×)

`pydict_lookup` の equality check が `py_eq_bool` 経由で関数呼出。整数
キーで明らかに `e->key == key` (VALUE-bit 比較) で十分なケースが多数。

```c
if (e->hash == h) {
    if (e->key == key) return e;       // immediate-equal: fixnum, None, True/False, intern str
    if (immediate(key) || immediate(e->key)) continue;   // can't differ-but-equal
    if (py_eq_bool(c, e->key, key)) return e;
}
```

dict_bench: 0.81 s → 0.75 s。`pydict_lookup` の overhead が 27% → 12% に
減った (perf 比較)。

### §8 — string slice の buffer 共有 (string 1.6×, GC 圧削減)

`s.split()` や `s[i:j:1]` で **新しい char バッファを確保せず、元の
バッファに `(chars, len)` で borrow ポインタを張る**。Boehm の
interior-pointer サポートで親バッファが自動的に生存。

`py_make_str_borrow` は更にサイズ最適化: `offsetof(pyobj, str) +
sizeof(str)` (24 byte 程度) のみ確保し、union の最大メンバ (func / cls)
分の死領域を避ける。Boehm のサイズ別 freelist で小さいバケットに入って
キャッシュ効率も上がる。

string_bench: 0.82 s → 0.50 s。

## 残ボトルネック

### `dict_bench` の `pydict_lookup` (27% → 12% から横ばい)

Open-addressing の二次プロービングが分散させるが、キャッシュミスが
増えるトレードオフ。CPython の dict はもう少し緻密 (PyDict_v8 系)。

### `string_bench` の `py_alloc` (15%)

split で 9 個 + 1 list = 10 alloc/iter は変えにくい。CPython は
sys.intern + 短文字列キャッシュを持つ。pystro では string interning は
未着手。

## perf 採取例

```sh
perf record -g --call-graph dwarf -o /tmp/p.data ./pystro -c bench/fib35.py
perf report -i /tmp/p.data --no-children --stdio | head -40
```
