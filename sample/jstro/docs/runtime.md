# runtime.md — jstro ランタイム解説

jstro が JavaScript プログラムをどのように実行するかを、値表現・関数呼び出し・
オブジェクトアクセス・例外処理の各レイヤに分けて説明する。

## 値表現 (`JsValue`)

CRuby から借りた tagged 64-bit 表現。下位 3 bit でディスパッチ:

| 下位 bit パターン | 意味 |
|------------------|------|
| `xxx_x000` (≠0, 8-aligned) | ヒープ・ポインタ (GC 管理オブジェクト) |
| `xxx_xxx1`              | SMI (signed 63-bit, `(v >> 1)` で復元) |
| `xxx_xx10`              | inline flonum (CRuby 風 shift 符号化) |
| `0x00`                  | `undefined` |
| `0x04`                  | `null` |
| `0x14`                  | `false` |
| `0x24`                  | `true` |
| `0x34`                  | "the hole" (sparse array / TDZ / deleted slot センチネル) |

`undefined == 0` を選んだのは calloc-zeroed メモリを undefined として読める
ようにするため。

### 数値のレンジ

- SMI: signed 63-bit。整数演算が overflow しない範囲ではヒープ割り当てなし。
- inline flonum: IEEE 754 double のうち magnitude が約 2⁻²⁵⁵..2²⁵⁶ に
  収まるものを 60 bit で再エンコード。out-of-range (denormal, ±0, ±Inf,
  NaN) は `JsHeapDouble` でヒープボックス化。
- `JV_AS_DBL` / `jv_from_double` は `__attribute__((always_inline))` で
  ホットパスに展開される。

### ヒープオブジェクト型 (`GCHead.type`)

```c
JS_TSTRING    = 1
JS_TFLOAT     = 2     // 範囲外 double のヒープボックス
JS_TBOX       = 3     // クロージャでキャプチャされたローカルのセル
JS_TOBJECT    = 16    // 通常オブジェクト
JS_TARRAY     = 17
JS_TFUNCTION  = 18    // JsFunction (closure)
JS_TCFUNCTION = 19    // JsCFunction (host C function)
JS_TERROR     = 20    // Error / TypeError / ...
JS_TMAP       = 32
JS_TSET       = 33
JS_TMAPITER   = 34
JS_TACCESSOR  = 35    // {get, set} を slot 値として埋め込む
JS_TSYMBOL    = 36
JS_TBIGINT    = 37    // 予約だけ。実装はしていない (123n リテラルは Number になる)
JS_TREGEX     = 38
JS_TPROXY     = 39
JS_TGENERATOR = 40    // (構文受理のみ; 真の suspend は未実装)
JS_TPROMISE   = 41    // (sync resolve)
```

`JS_TOBJECT 以上はオブジェクト的`という分類だが、レイアウトはバラバラ
なので、`jv_heap_type(v) == JS_TOBJECT` を厳密にチェックするか、
専用の prototype を介してメソッドをディスパッチする。

## hidden class (`JsShape`)

V8 同様の hidden class モデル:

```c
struct JsShape {
    GCHead    gc;
    uint32_t  nslots;
    uint32_t  capa;
    JsString **names;          // [nslots]
    uint32_t  ntrans, tcap;
    struct JsShapeTrans { JsString *name; JsShape *to; } *trans;
    JsShape  *parent;
};
```

各オブジェクトは `JsShape *` を持ち、`o->slots[i]` にプロパティ値を格納。
`obj.x = 1; obj.y = 2;` のような順序でプロパティを足していく場合、同じ順序
でセットされた別オブジェクトとは shape が共有される (`trans` テーブルに
キャッシュされた transition を辿るので、線形スキャンを回避)。

### IC (inline cache)

各 AST ノードの `JsPropIC` フィールド:

```c
struct JsPropIC {
    uintptr_t shape;       // post-state shape
    uint32_t  slot;
    uintptr_t pre_shape;   // member_set 用 — 遷移直前の shape
};
```

#### read (member_get)

最速パス: `o->shape == ic->shape && o->shape->names[ic->slot] == name`
(2 つの compare + 1 load) なら `o->slots[ic->slot]` を返す。
**ただし**:
- スロットが `JV_HOLE` (delete された) の場合は slow path に落とす (proto chain 探索のため)
- スロットが `JS_TACCESSOR` (getter/setter) の場合も slow path

ミス時は `js_shape_find_slot` で線形スキャン → IC を更新。

#### write (member_set)

read と同様の "shape 完全一致" パスに加えて、**transition pattern** を
高速化する `pre_shape` フィールドを持つ。

理由: `function Body(x,y) { this.x = x; this.y = y; }` のような
コンストラクタを 5 回呼ぶと、各呼び出しで同じ AST ノード (`this.x = x`)
は EMPTY_SHAPE → SHAPE_X の **遷移** を起こす。post-state を IC に
覚えても、次の呼び出しの開始 shape は EMPTY_SHAPE で IC ミスになる。

そこで **(pre_shape → shape, slot)** を覚えれば、O 回目以降は

1. `o->shape == ic->shape` ? → 既存スロット書き換え (定常時)
2. `o->shape == ic->pre_shape` ? → `o->shape = ic->shape; o->slots[slot] = v` (constructor 時)
3. otherwise → 線形スキャン

の順で判別。これが nbody bench で `js_shape_find_slot` を 9% から ほぼ 0% に
削減した最大の最適化。

### accessor (getter/setter)

slot 値が `JS_TACCESSOR` 型の小オブジェクト (`{get: fn, set: fn}`) のとき、
- 読み出し: getter を receiver=元のインスタンスとして呼ぶ
- 書き込み: setter を呼ぶ
- どちらの half もない (data descriptor) ように見えれば普通に読み書き

`js_object_get_with_receiver(c, o, key, receiver)` は proto chain を辿る
ときに receiver を保持する内部ヘルパーで、`obj.foo` の `foo` が proto に
ある getter であっても `this === obj` で起動する。

## frame と関数呼び出し

すべての関数呼び出しは **alloca で確保された flat 配列** をフレームとして
使う:

```c
JsValue *frame = (JsValue *)alloca(sizeof(JsValue) * fn->nlocals);
```

各 AST ノードの local 参照は `frame[slot]` で行う (slot は parser がスコープ
を辿って静的に決定)。グローバル参照は `c->globals` (これも `JsObject` で
hidden class の対象) を読む。

### スコープ解決 (parser 側)

パーサ側で各識別子参照を 4 種類のいずれかに分類:

- `RES_LOCAL`  — 同関数内のローカル。`frame[slot]` 直アクセス。
- `RES_BOXED`  — 同関数内の **キャプチャされた** ローカル。`frame[slot]`
                には `JsBox *` が入っており `box->value` 経由で読む。
- `RES_UPVAL`  — 親関数のローカル/upval。クロージャの `upvals[idx]` ポインタ
                経由 (`*c->cur_upvals[idx]`)。
- `RES_GLOBAL` — グローバル。`c->globals` で shape lookup。

### let / const と TDZ

`let` / `const` は宣言種別 (`decl == 1 / 2`) を `RefBinding` で保持。
`emit_load` / `emit_store` は decl が let/const のとき、TDZ チェック付きの
`node_let_get` / `node_let_set` を発行する。`node_let_set` は currently-stored
slot 値が `JV_HOLE` ならば `ReferenceError("Cannot access ... before initialization")`
を投げる。

ブロック先頭で **テキスト pre-scan** (`hoist_letconst`) を走らせて
let/const の名前を予約し、`scope_add_hole` で hole-init slot 一覧に
追加する。`node_block` 評価時にすべての hole_slots を `JV_HOLE` で初期化
してから body を評価。

### 関数宣言ホイスト

PARSE_FILE / parse_function_expr / parse_block の入口で `hoist_scan` を
呼び、`function NAME` 形式を検出して target スコープに事前宣言する。
こうすることで `f` が `g` を参照、`g` が `f` を参照する相互再帰のような
パターンが書けるようになる。

### per-iteration `let` binding (`for-let`)

`for (let i = 0; i < n; i++) { ... }` のような形式で内部のクロージャが
`i` をキャプチャすると、各反復で **新しいバインディング** を見るのが
spec の挙動。jstro は `node_for_let` 専用ノードを発行し、各反復で:

1. slot の現在値を snapshot 化
2. fresh `JsBox` をアロケート、snapshot を `box->value` にセット
3. `frame[slot] = box pointer` に置く
4. body 評価 (内部のクロージャがこの box をキャプチャ)
5. body 終了後、`frame[slot] = box->value` にリセット (cond/step の評価用)
6. step 評価
7. 戻ってループ

これにより各反復のクロージャが iter ごとに別の box を持ち、`fs[0]() === 0`,
`fs[1]() === 1`, ... が正しく動作する。

### 呼び出し経路

```
node_call1(fn_node, arg_node, ic@ref)
   │
   ├─ ic.cached_fn ヒット?
   │      └─ jstro_inline_call(c, ic.cached_fn, ic.cached_body, ic.cached_nlocals,
   │                           thisv, args, argc)  ← inlined
   │
   ├─ JsFunction だが IC ミス?
   │      └─ ic 更新 → js_call_func_direct
   │
   └─ JsCFunction (C 実装)
          └─ js_call (一般経路)
```

`jstro_inline_call` は `js_call_func_direct` を `static inline always_inline`
にしたバージョン。dispatcher 関数内に展開されるので、再帰呼び出しサイトで
indirect call (`call *%rdx`) 1 回ぶんの帯域を節約できる。

### `js_call_func_direct` の中身

1. `nlocals` 個ぶんの `JsValue` を `alloca`。
2. 引数を frame の先頭にコピー (rest parameter があれば余り引数を `JsArray`
   としてまとめて最後のスロットへ)。
3. 残りの local スロットを 0 (= `undefined`) で初期化 (calloc 同等)。
4. CTX の `this_val`, `cur_upvals`, `cur_args`, `cur_argc` をスタック上に退避し、
   新しい値をセット。アロー関数なら `bound_this` (生成時にキャプチャした `this`) を優先。
5. `EVAL(c, fn->body, frame)` を実行。
6. 戻り値は `JSTRO_BR == JS_BR_RETURN` なら `JSTRO_BR_VAL`、そうでなければ
   `undefined`。`JSTRO_BR` を NORMAL に戻す。
7. 退避した CTX 状態を復元。

例外で抜ける場合は **longjmp** が `js_call_func_direct` をスキップして
最寄りの `try` フレームへ飛ぶ。alloca した frame は longjmp が暗黙的に
スタックを巻き戻すので解放される。

### method call

`obj.foo(args)` は `node_method_call`:

1. `obj` を評価。
2. shape IC (member_get と同じ構造) で `foo` のスロットを取り出す。ミス時は
   prototype chain (`o->proto`) を辿って fall back。
3. 引数をスタック上に集める (spread があれば `node_method_call_spread` で展開)。
4. `obj` を `this` として `js_call`。

### `new` と class

`new C(args)`:

1. `C` を評価。`fn->home_proto` を遅延作成。
2. 新しい JsObject を作成 (proto = `fn->home_proto`)。
3. CTX の `new_target` を C に設定し、`js_call_func_direct` で起動。
4. コンストラクタが non-object を返すと `self` を、object を返すとそれを結果にする。

`class Sub extends Base { ... }`:

1. parser がクラス本体を以下に desugar:
   ```js
   var __super__ = Base;
   var C = function /* constructor */ () { ... };
   __chainProto__(C, Base);
   C.prototype.method1 = function() { ... };
   ...
   __defAccessor__(C.prototype, "name", "get", getter_fn);
   /* static blocks */
   ```
2. メソッド本体内の `super` 参照は parser が `__super__` のキャプチャに
   変換される (closure として upvalue 経由)。
3. ランタイム関数 `__chainProto__(Sub, Base)` が `Sub.prototype.[[Prototype]]
   = Base.prototype` を設定。
4. `__defAccessor__(target, key, "get"|"set", fn)` がスロットを `JS_TACCESSOR`
   オブジェクトに変換 (両 half を順に追加)。

### private fields `#name`

lexer が `#` を識別子の一部として読むので、`obj.#name` は通常のプロパティ
アクセス相当 (キーが `#name` という JsString)。真のプライバシは保証されないが、
`#` プリフィックスは予約語なので衝突しにくい。

### `arguments` / `new.target`

CTX に `cur_args` / `cur_argc` / `new_target` を持ち、`js_call_func_direct`
が呼び出し時にセット/復元。`node_arguments` は `cur_args`/`cur_argc` を
JsArray にコピーして返す。`node_new_target` は `cur_new_target` を返す。

ユーザーが `arguments` を識別子として宣言した場合は通常のスコープ解決
(`scope_lookup`) が優先され、暗黙の binding は使われない。

## 例外処理 (`throw` / `try-catch-finally`)

V8 や CPython のように **C 言語の `setjmp` / `longjmp`** を使う:

- `try { ... }` の入口で `setjmp(frame.jb)`。`c->throw_top` のリンクト
  リストにフレームを push。
- `throw e` は `js_throw` を呼び、`c->last_thrown = e` をセットしてから
  `longjmp(c->throw_top->jb, 1)`。
- catch 側で `setjmp` から戻ってくる。`c->last_thrown` が値。`catch (e)`
  なら e を frame slot にバインド。
- finally は (1) try が完了したあと、または (2) catch で例外を捌いた
  あと、または (3) catch で再度例外が発生した場合、いずれでも実行される。

### break/continue/return との分担

- `throw` → longjmp (深いスタックを一気に解ける)。
- `return` / `break` / `continue` → グローバル `JSTRO_BR` (`JS_BR_RETURN` /
  `JS_BR_BREAK` / `JS_BR_CONTINUE`) で伝播。`break <label>` は `JSTRO_BR_LABEL`
  にラベル名を載せる。
- 算術 / property access ノードは BR チェックを持たない (longjmp 化により不要)。
  チェックは loop / `seq` / `if` / `call` の戻り際でのみ行う。

## イテレータプロトコル (`for-of`)

`node_for_of`:

1. iter expr 評価。
2. Array / String → 専用 fast path。
3. それ以外 → `@@iterator` プロパティ (= `Symbol.iterator` の文字列キー化)
   を探し、ファクトリを呼び出して iterator obj を得る。なければ value 自身が
   `next()` を持つかをダックタイピングで確認。
4. `iter.next()` をループで呼び、`{done: true}` で抜ける。`{value}` を
   loop var に書き込み body を評価。
5. `for (let X of ...)` の destructuring 形式は parse 時に `let __tmp = nextValue`
   + 内部で pattern bind に展開。

`Map` / `Set` の `[Symbol.iterator]` は `JsMapIter` を返し、`next()` で
`{value: [k,v], done}` のような結果を返す。

## 正規表現 (`js_regex.c`)

NFA バックトラッキングマッチャ。pattern source をそのまま保持し、`.test`
/ `.exec` 時に解釈実行する。コンパイル前計算なし。

サポート:
- リテラル文字、`. * + ? {m,n}`、`^ $`
- 文字クラス `[abc]` `[^abc]` `[a-z]`、ショートカット `\d\D\s\S\w\W \b\B`
- グループ `(...)`、非キャプチャ `(?:...)`
- 交替 `|`
- フラグ `i g m s` (u/y/d は受理だが無視)

非サポート: lookahead/lookbehind、backreference、named group、Unicode property。

## モジュール (`require` / `import`)

`require(path)`:

1. パス解決: `path`, `path.js`, `path/index.js` の順で探す。
2. キャッシュ: `g_modules` リンクトリストでパスをキー。
3. 未キャッシュなら read+parse。本体を `(function(module, exports, require) {
   ...本体... })` でラップして `PARSE_STRING` → 即時呼び出し。
4. `module.exports` を取り出してキャッシュ更新+返却。

ES `import` / `export` は parser が CommonJS 相当に desugar:

- `import { a, b as c } from "p"` → `const __m = require("p"); const a = __m.a; const c = __m.b;`
- `import x from "p"` → `const x = require("p").default;`
- `import * as ns from "p"` → `const ns = require("p");`
- `export const x = ...` → `const x = ...; module.exports.x = x;`
- `export default expr` → `module.exports.default = expr;`

トップレベルで `module` / `exports` / `require` が参照できる前提なので、
ユーザーが `node` のようなコマンドラインで実行する場合は wrapper が必要
(または最上位は CommonJS 同等にする)。

## 自動 GC (mark-sweep)

`js_gc_alloc` で確保した全オブジェクトは `c->all_objects` に侵入リスト
形式 (`gc.next`) で繋がれる。GC は **safepoint 駆動の stop-the-world
mark-sweep**。

### マークフェーズ

ルート集合:
- グローバル / 各種 proto オブジェクト (`object_proto`, `array_proto`, etc.)
- 文字列 intern bucket (`intern_buckets`)
- モジュールキャッシュ
- `c->this_val` / `c->cur_args[0..argc]` / `c->new_target` /
  `c->last_thrown` / `JSTRO_BR_VAL`
- **`c->frame_stack` 連鎖** — 後述の `js_frame_link` チェイン

`mark_value` は `JV_IS_PTR` のみ追跡。type に応じて `mark_object_struct`
(shape + slots + proto)、`mark_array` (dense + sparse entries + proto)、
`mark_function` (body はノードなので不動だが upvals + bound_this + own_props)、
`mark_string` (intern table の生存確認)、`mark_shape` (parents + transitions)
を再帰呼び出し。

### スイープフェーズ

`all_objects` を線形走査し、現在の dead 色のものを `gc_finalize` で解放
(slots / dense / upvals / map entries 等の malloc 領域も)。GC 終了時に
`g_mark_live` ⇄ `g_mark_dead` を flip するので、sweep 後の色リセットは不要。

### `js_frame_link`: alloca フレーム連鎖

```c
struct js_frame_link {
    JsValue              *frame;     // alloca'd local slots
    uint32_t              nlocals;
    JsValue              *args;      // caller-provided args
    uint32_t              argc;
    struct js_frame_link *prev;
};
```

- `js_call_func_direct` / `jstro_inline_call` が **callee_frame + args** を
  リンクに登録。リンクは callee の C スタックに置くだけなので O(1)。
- 各 call dispatcher が **arg 評価中** に半構築 argv を pin する一時リンクを
  積む。`new T(f(), g())` で `f()` 結果を argv[0] に置き、`g()` 評価中に
  GC が起きてもこの pin で argv[0] が live と判定される。
- `js_throw_frame` (try-catch 用) に `frame_stack` の snapshot を持たせ、
  longjmp で巻き戻った際にチェインを正しい深さに戻す。

### セーフポイント

`js_gc_alloc` は GC を起動しない (使われたばかりの半構築値が C スタック
にある可能性が高いため)。代わりに以下の文境界でのみ `jstro_gc_safepoint`
が起動条件 (allocated > threshold) を確認:

- `node_seq` / `node_seqn` の各文後
- `node_for` / `node_for_let` / `node_while` / `node_do_while` の反復後
- `node_for_of` / `node_for_in` の反復後

閾値は前回 GC 後の生存数の 2× で動的調整 (floor 4096 / ceiling 1M)。
`gc_disabled` カウンタが正のときは safepoint でも skip (現状は使用箇所
なし、`js_str_concat` 等の delicate path 用に予約)。

### Weak ref / finalizer

未実装。`WeakMap` / `WeakSet` は現状 strong ref で代用しているため、
キーが他から参照されなくなっても解放されない。

## ASTroGen との接続

ASTro フレームワークの dispatcher / evaluator 分離パターンを使う。
`node.def` の各 `NODE_DEF` に対して ASTroGen が生成する:

- `EVAL_<name>` — 本体 (always_inline)
- `DISPATCH_<name>` — node から operand を読んで `EVAL_<name>` を呼ぶ
- `ALLOC_<name>` — node を割り当て
- `HASH_<name>` — Merkle ハッシュ計算
- `DUMP_<name>` — pretty print
- `SPECIALIZE_<name>` — 部分評価された C を出力 (現状未使用; SD bake 未駆動)

dispatch は関数ポインタ経由 (`n->head.dispatcher(c, n, frame)`)。`EVAL_ARG`
マクロが child node から `dispatcher` を取り出して間接呼び出しを行う:

```c
#define EVAL_ARG(c, n) ((*n##_dispatcher)((c), (n), (frame)))
```

ASTroGen が生成する `DISPATCH_<name>` がこの間接呼び出しに先んじて
`n->head.dispatcher` を渡しているため、SD 化されればこの間接呼び出しが
直接呼び出しになる (jstro はまだ SD 化を駆動していない)。
