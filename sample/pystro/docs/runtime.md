# runtime.md — pystro のランタイム解説

pystro は ASTro 上に乗せた **Python 3 サブセット** の tree-walking
インタプリタ。 動く範囲は [done.md](./done.md)、 未実装は [todo.md](./todo.md)、
ベンチは [perf.md](./perf.md) を参照。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def`   | AST ノード定義 (定数 / 変数 / 算術 / 比較 / 制御 / 呼出 / try / with / def / class / match / yield / etc.) |
| `context.h`  | `VALUE` 表現、 `CTX`、 シングルトン、 option 構造体 |
| `node.h`     | `NodeHead` / 公開 API、 `py_apply` inline fast path |
| `node.c`     | アロケータ + ASTroGen 生成ファイルの `#include` |
| `runtime.c`  | ヒープ / globals / apply / builtins / ヘルパ群 |
| `parser.c`   | recursive-descent parser |
| `lexer.c`    | tokenizer (INDENT/DEDENT, f-string, triple-quoted etc.) |
| `main.c`     | driver / option / REPL |

`runtime/` (sample 共通) も参照:

- `runtime/astro_node.c` — `HASH` / `DUMP` / ハッシュ関数群を `#include` 形式で提供
- `runtime/astro_code_store.{h,c}` — `astro_cs_init` / `compile` / `build` / `load` / `reload` API

## 1. VALUE 表現

下位 3 ビットで dispatch:

```
xxxx_xxxx_xxxx_001 → fixnum (signed 62-bit、 左 1 シフト + 1)
xxxx_xxxx_xxxx_010 → flonum (CRuby 流 3-bit rotate、 ~[1e-77, 1e+77])
xxxx_xxxx_xxxx_000 → ヒープオブジェクト (`struct pyobj *`、 8-byte aligned)
```

- **fixnum**: 62-bit immediate signed int。 オーバーフローは `py_add` の
  slow path で `mpz_t` (GMP bignum) に格上げ。
- **flonum**: CRuby と同じ encoding。 `double` の指数部が中央域に
  入る値は heap-box せず即値に。 数値計算の hot loop で alloc が消える。
- **ヒープ**: `struct pyobj *`。 8-byte aligned で下位 3 bit が 0。

`True` / `False` / `None` / `Ellipsis` / `NotImplemented` は **静的に
確保された singleton `pyobj`** で、 そのアドレスを `VALUE` として配る
(`PY_TRUE` / `PY_FALSE` / `PY_NONE` / etc.)。

ヒープ型 `enum pyobj_type` の `PY_T_*`:

| グループ | 型 |
|---|---|
| Numeric | `INT` (bignum mpz_t), `FLOAT` (boxed double), `COMPLEX` |
| Sequence | `LIST`, `TUPLE`, `STR`, `BYTES`, `BYTEARRAY`, `RANGE` |
| Mapping | `DICT` (open-addressing) |
| Set | `SET`, `FROZENSET` |
| Callable | `FUNC` (Python closure), `BUILTIN` (C func), `CLASS`, `INSTANCE`, `BOUND` (method binding), `STATICMETHOD`, `CLASSMETHOD`, `PROPERTY`, `GENERATOR`, `ITER` |
| その他 | `MODULE`, `FILE`, `SLICE`, `MEMORYVIEW`, `EXCEPTION_GROUP` |

### bignum

`PY_T_INT` は `mpz_t` を 1 個含む。 fixnum と big で API が透過的に
扱えるよう `py_to_mpz()` で fixnum → mpz、 `py_make_int()` で
fits-in-fixnum なら fixnum、 そうでなければ heap-int を作る。

## 2. 実行コンテキスト (`CTX`)

```c
typedef struct CTX_struct {
    struct pyframe *env;            // 現フレーム (top-level では NULL)
    struct pyglobals *globals;      // hash-indexed table、 serial 番号
    VALUE  current_class;           // class body 評価中なら 該当 class
    VALUE  method_class;            // super() の参照点
    int    state;                   // PY_STATE_NORMAL / RETURN / RAISE / BREAK / CONTINUE
    VALUE  state_value;             // return 値 / 例外ペイロード / etc.

    // 例外ハンドラ stack (try/with/exec)
    jmp_buf *try_stack[64];
    int      try_top;
    jmp_buf  err_jmp;
    int      err_jmp_active;

    // 呼び出しスタック (traceback 用)
    const char *call_stack[1024];
    int         call_top;
    int         recursion_limit;

    // 例外関連
    VALUE current_handling_exc;     // except 節内の active exc

    // 例外クラス + 組み込み型クラス (singletons)
    VALUE EXC_BaseException, EXC_Exception, EXC_TypeError, ... ;
    VALUE TYPE_int, TYPE_float, TYPE_str, TYPE_list, ... ;
} CTX;
```

### 制御フロー伝播

`state` は **return / raise / break / continue を上位ノードに伝える
フラグ**。 `node_return` が `state = RETURN; state_value = v` を立て、
上位の `node_seq` / `node_if` / `node_while` / `node_for_*` が
`state != NORMAL` を見て即座に bail。 関数境界まで一直線に巻き戻る。

例外は `state = RAISE` に加えて、 try/with/exec が事前に setjmp していれば
`py_raise_exc` が **longjmp** で直接 try frame まで巻き戻る。 try frame
が無ければ state-based propagation のみ。 ハイブリッド方式。

R18 で chained call/attr/subscript ノード (例えば `raiser().attr`) で
state チェックが漏れていたバグを修正:
`node_attr_get` / `node_subscript_get` / `node_call_n` /
`node_method_0` / `node_method_n` / `node_attr_set` /
`node_subscript_set` / `node_slice` の各箇所で受信側 eval 直後に
`if (UNLIKELY(c->state != PY_STATE_NORMAL)) return PY_NONE;` を入れている。

## 3. フレームと変数解決

### ローカル

`struct pyframe` の flat な `slots[nlocals]` に固定 index でアクセス。
parser が `def` を読む時に suite を pre-scan して `NAME =` の左辺と
仮引数を local として scope に登録 (Python の「関数内で代入された名前は
local」ルール)。

ノード:
- `node_lref(idx)` — local read
- `node_lset(idx, rhs)` — local write

leaf 関数 (ネストした `def`/`class` を持たない) の `pyframe` は
**C スタック上に `alloca`**。 Boehm GC の保守的スタックスキャンが
スロットの VALUE を生かす。 closure capture が無いので alloca の
ライフタイムが call と一致する。

非 leaf は `GC_malloc` でヒープ確保 (closure capture のため)。

### グローバル

`struct pyglobals` は **hash-indexed open-addressing**:
- `entries[]`: `{name, value, defined}` の配列
- `indices[]`: power-of-two サイズの bucket array、 `entries[]` への index
- 重複時は線形プロービング

ノード:
- `node_gref(name, cache @ref)` — global read with inline cache
- `node_gset(name, rhs, cache @ref)` — global write with cache

`gref_cache` は `{ uint64_t serial; int32_t idx }`。 `globals_serial`
が変わっていなければ idx で配列に直接アクセス。 構造変化 (新 slot /
未定義→定義) のみ serial を bump、 値更新では bump しない (R8 で
fix、 perf.md §2)。

### closure

非 leaf 関数の `pyframe.parent` で外側のフレームに繋がる。 `nonlocal`
変数は parser が「外側 scope の slot」に解決して `node_lref_outer` /
`node_lset_outer` を emit。

## 4. 関数呼び出し

`def name(params): body` は `node_def` で `pyobj{type=FUNC}` を作って
global に登録。 `py_make_func` で確保するクロージャ:

```c
struct pyfunc {
    NODE *body;
    struct pyframe *env;
    const char *name;
    int nparams, n_pos_named, n_pos_only, nlocals;
    bool has_varargs, has_kwargs, leaf, is_generator;
    VALUE *defaults;            // [nparams]、 (VALUE)0 = required
    const char **param_names;   // [nparams]
    struct pyglobals *fglobals; // module の globals (cross-module support)
    VALUE defining_class;       // method なら class
    VALUE __doc__;
    VALUE __annotations__;
};
```

呼び出し `f(a, b, kw=v)` は arity に応じて以下の dispatch:

| 状況 | ノード | エントリ |
|---|---|---|
| 0〜3 引数、 kwargs なし | `node_call_K` | `py_apply` (inline) |
| 多数引数 / *args spread | `node_call_n` / `node_call_spread` | `py_apply` |
| kwargs あり | `node_call_kw` | `py_apply_kw` |
| 関数 fast path (inline `node.h`) | — | exact-arity, no varargs/kwargs, leaf |
| その他 (built-in / bound / class / mismatch) | — | `py_apply_slow` |

`py_apply` は `node.h` の `static inline __attribute__((always_inline))`。
SD コードからの PLT hop が消える (perf §5)。 fast path:

1. 新しい `pyframe` を `alloca` (leaf) または `GC_malloc` で確保。
2. `slots[0..nparams) = argv`。
3. `c->env` 退避 → 新フレーム → `EVAL(body)`。
4. `state == RETURN` → `state_value` 取り出して return、
   `state == RAISE` → propagate (PY_NONE 返り)、
   それ以外 → return `None`。
5. `c->env` 復元。

### kwargs 受け渡し

builtin に kwargs を渡すときは thread-local `PYSTRO_BI_KWC` /
`PYSTRO_BI_KWNAMES` / `PYSTRO_BI_KWVALUES` を set/restore して flow させる。
nested class call の outer kwargs leak は R18 で metaclass __call__
ディスパッチ前後の save/restore で fix。

## 5. ノードの種類 (node.def)

| グループ | ノード |
|---|---|
| 定数 | `const_int / const_int64 / const_float / const_str / const_bytes / const_none / const_true / const_false / const_ellipsis / const_int_big` |
| 変数 | `lref / lset / gref / gset / lref_outer / lset_outer` |
| 単項 | `neg / not / bit_inv / pos`(builtin 経由) |
| 算術 | `add / iadd / sub / mul / matmul / truediv / floordiv / mod / pow / bit_and / bit_or / bit_xor / lshift / rshift` (fixnum / flonum fast path inline、 `iadd` は list `+=` の in-place extend) |
| 比較 | `lt / le / gt / ge / eq / ne / is / is_not / in / not_in` (fixnum fast path inline + chained 対応) |
| 論理 | `and / or / not` (short-circuit) |
| 制御 | `if / while / for_local / for_global / seq / nop / return / break / continue / raise / raise_bare` |
| 例外 | `try / with / assert` |
| 呼出 | `call_0 / call_1 / call_2 / call_3 / call_n / call_kw / call_spread` |
| メソッド | `method_0 / method_1 / method_2 / method_n` (inline cache) |
| 属性 | `attr_get / attr_set` (cache) |
| subscript | `subscript_get / subscript_set` (fixnum index で list/tuple/bytes/bytearray の fast path inline) |
| slice | `slice / slice_set` |
| def / class | `def / class_def / class_method_get / class_method_set` |
| match | `match` (PEP 634) + 各種 pattern node |
| yield | `yield / yield_from / await` |
| その他 | `make_list / make_tuple / make_dict / make_set / lambda / super_obj / super_obj_explicit / unpack_assign` |

### inline cache を持つノード

`@ref` で末尾に cache 構造体ポインタを埋め込み、 ASTroGen が hash 計算
スキップ + dump スキップ + specialize 時に `&n->u.<kind>.<field>` を emit。

| キャッシュ | 形 | 効果 |
|---|---|---|
| `gref_cache` | `{serial, idx}` | global lookup の strcmp 排除 |
| `attr_cache` | 4-way (cls, shape_version, eidx) + class-data 用 monomorphic slot (`cls_recv`, `class_value`, `cls_recv_sv`) + instance-receiver class-attr 用 monomorphic slot (`inst_ca_cls`, `inst_ca_val`, `inst_ca_sv`) | instance attr / `Cls.attr` / `instance.classattr` の 3 経路を並列キャッシュ |
| `method_cache` | builtin path: `{type_tag, fn}` / user-class path: 4-way (cls, fn) / module: 4-way (module_ptr, fn) / classmethod: 4-way (cls, wrapped_fn) | bound-method 確保 + MRO walk + strcmp 排除 |
| `binop_cache` | `{cls, fn}` | `node_add/sub/mul` の per-call-site IC、 Vector の operator dunder で命中 |

**`attr_cache` の 3 経路**:

1. *instance attr* (4-way) — `self.x` のように instance dict に
   入っている属性。 `(cls, shape_version)` で eidx を memoize。
   shape_version は `py_class_add_method` で bump (構造変化のみ)。
2. *class-data* (`cls_recv`/`class_value`/`cls_recv_sv`) —
   `Strength.WEAKEST` のように **receiver が class** のときに
   class object 上の属性を memoize (commit `8af7ef6`)。
3. *instance-receiver class-attr* (`inst_ca_*`) —
   `aes.T1` のように **receiver が instance だが** 属性が class
   レベルにあるケース。 instance dict miss → MRO walk + strcmp が
   pyaes で 9% 占有していたのを撤去 (Phase 8)。 stamp は **stable
   data** (list / tuple / dict / str / bytes / int 等、 bound method
   や property は除外) のときのみ。

**4-way polymorphic IC**: `Constraint` subclass の混合 iteration のような
polymorphic call site で monomorphic IC は thrash する (richards で
4.93M/4.93M = 100% miss を計測)。 4 entry 配列で linear scan + LRU 挿入
することで thrash 解消 (commit `2eadb18` / `3e90b55`)。

**dunder slot pre-resolution**: 24 種の特殊メソッド名 (`__init__`、
`__eq__` など) を struct pyclass の専用フィールドに pre-resolve。
`py_class_lookup_method` の hot path は `name == PYSTRO_INTERN_eq` の
ようなポインタ一発比較 + フィールド load で済み、 MRO walk + strcmp は
slow path のみ (commit `bf286e0`)。 詳細は [perf.md](./perf.md) Phase 4。

**`pyclass.fast_new` flag**: user class の `slot_new` が default
`bi_object_new` で built-in base なし、 例外でなければ、 instantiation
で `__new__` dispatch を全 skip して `py_make_instance` 直叩き。
`pyclass_refresh_slots` で事前計算 (Phase 8)。 deltablue / raytrace
の Constraint / Vector instantiation がここで稼ぐ。

**method_cache の 4 種の type_tag**:

| type_tag | 用途 | dispatch |
|---|---|---|
| `PY_T_INSTANCE` (user class) | `inst.method(args)` | `py_apply(fn, [inst, args])` |
| `PY_T_INSTANCE` + `cache->fn` | 組込型 subclass の primary method (`OrderedCollection.append`) | `cache->fn(c, [primary, args])` |
| `PY_T_MODULE` | `math.sqrt(x)` | `py_apply(fn, [args])` (self なし) |
| `PY_T_CLASS` | `Cls.classmethod(args)` | `py_apply(wrapped_fn, [cls, args])` |
| `cache->fn` (builtin tbl) | `[1,2].append(3)` | `cache->fn(c, [recv, args])` |

`py_method_resolve` がこれらを判別して cache を stamp する。 hot path は
`PY_PTR(o)->type == cache->type_tag` の一発で経路を選び、 PIC で
`fn` を引き、 即 dispatch。

## 6. parser

- **lexer がトークン列を全部メモリに乗せる**。 indent 追跡で `INDENT` /
  `DEDENT` を生成、 `#` コメント無視、 `\n` で `NEWLINE` (paren_depth > 0
  のときは抑制)。 f-string は brace_depth 追跡で nested quote をサポート
  (PEP 701)。 三重クォート、 `b"..."`、 `r"..."`、 raw f-string `rf"..."`。
- 続いて recursive-descent parser がトークンを舐めて AST を作る。
  優先順位は手書きの分割関数 (`parse_or → parse_and → parse_not →
  parse_compare → parse_arith → parse_term → parse_factor → parse_power
  → parse_postfix → parse_atom`)。
- `def` / `class` を読むときは body の token range を `find_suite_end` で
  特定して **lvalue を pre-scan** し scope に登録、 その後改めて parser を回す。
- match-case の pattern は専用 `parse_pattern_*` 群。
- exec/eval/compile からの parse 失敗は parse_error が `parse_error_jmp`
  jmp_buf に longjmp、 runtime 側で `SyntaxError` を raise する。

### parser 側の最適化

- **method call fusion**: `obj.name(args)` は `node_attr_get` + `node_call_N`
  ではなく **`node_method_N`** に直接 fuse する。 `parse_dot_trailer` で
  T_DOT の次が T_LPAREN を見たら argc に応じて `node_method_0/1/2/n` を
  ALLOC する。
- **LHS の同 fusion**: `self.x().y = z` のような assignable target でも、
  `parse_assignable_target` の build loop で `TR_DOT + TR_CALL` を
  `node_method_N` に fuse。 これがないと LHS だけ
  `node_call_0(node_attr_get(self, "x"))` 経由になり、 method PIC が
  効かず毎回 bound method alloc + `py_apply_slow` 落ちしていた。
  deltablue で 4× の決定打 (Phase 8)。
- **augmented assign の特化**: `lhs += rhs` は通常 `lset(name, add(lref(name), rhs))`
  に desugar するが、 `+=` だけは `add` ではなく **`iadd`** を使う。
  `iadd` は list+list のとき in-place extend (CPython の `__iadd__` 相当)、
  それ以外は `add` と同じ semantics。

## 7. Code Store / AOT / JIT

`OPTIMIZE` が `astro_cs_load` を呼ぶ標準パターン (ascheme と同じ)。
`-c` で起動すると AST 構築直後に `astro_cs_compile + build + reload`
で SD を焼き、 その run からそのまま使う。 `--aot-compile` は焼いて
exit。

bench 対象 (`make bench`) では:

1. `rm -rf code_store` (cold)
2. `CCACHE_DISABLE=1 ./pystro -c bench.py` (bake)
3. `for i in 1..3; do time ./pystro bench.py; done` で best-of-3

`CCACHE_DISABLE=1` が要るのは sandbox 環境で ccache がキャッシュ
ディレクトリに書けないため。

PG / JIT は v0 ではまだ着手していない (todo.md 参照)。

## 8. 例外モデル

- `raise X` → `node_raise` が `state = RAISE; state_value = X` を
  set、 上位の try/with/exec frame まで state ベースで propagate。
- `py_raise_exc` (C 内部) は `state` set + 直近の try frame に longjmp。
  no try frame なら `err_jmp` (top-level) に jump、 そこも無ければ exit。
- try/except は `py_run_try` が setjmp/longjmp で本体走らせ、 raise
  なら handler を逐次マッチ。 finally は本体終了後 (raise/return/normal
  全部の path で) 必ず run。
- with は `py_run_with` で `__enter__/__exit__` 呼出を managed。
  `__exit__` が True を返したら例外を suppress。
- PEP 654 except* (ExceptionGroup split) も対応。 `py_eg_split` が
  type 述語で matched/unmatched に分割。

## 9. メモリ管理

- Boehm GC (libgc.so.1)。 conservative scan。
- alloca フレーム (leaf func) は call boundary でのみ生存、 GC scan が
  スタックの VALUE を生かす。
- `pyobj` は `GC_malloc` (atomic / non-atomic は最適化少しあり、 大半は
  generic)。
- 配列 backing (list の items, dict の entries 等) は `GC_realloc`
  で grow。 interior-pointer サポートで部分参照 (str slice の borrow 等)
  が親バッファを生かす。
- sigil-less: refcount は無い、 cycle も Boehm が回収。

## 10. CPython 互換性 ハイライト (R18)

- `_abc_registry` ベースの ABCMeta virtual subclass
  (`isinstance(5, numbers.Integral)` が動く)
- `bi_import` cached re-attach (`from a.b import c` で a.b が後から
  parent a に貼り付く)
- module の `__file__` / `__dict__`
- `exec(bytes)` / `eval(bytes)` / `compile(bytes)` 受理
- `in` の generic iter protocol fallback
- 単項 `+` で non-numeric は TypeError
- exec/eval/compile の parse failure を SyntaxError として raise
- `__class_getitem__` の @classmethod を unwrap
- TypeError 表記が CPython 互換
  (`'X' object is not iterable`,
   `'<' not supported between instances of 'X' and 'Y'`,
   `argument of type 'X' is not a container or iterable`)
- `__getitem__` iter protocol が IndexError/StopIteration を
  local try frame で catch
- `types.MethodType(fn, inst)` constructible (metaclass で内蔵
  bound-method type も認識)
- nested class call の `PYSTRO_BI_KWC` leak fix
  (RawConfigParser(defaults={}) が動く)
- chained call/attr/subscript で raise が伝播
  (codecs.lookup(...).incrementalencoder の例外が消えない)
