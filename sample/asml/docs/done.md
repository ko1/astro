# 実装済み機能

## パイプライン

```
source → lex → parse → expr (IR) → infer (HM) → lower → NODE → EVAL
```

各段階を完全分離。`expr` IR は型注釈を持つ中間表現で、推論はここに対して
行う。lowering は `expr->ty` を見て、operand 型が確定している式に対して
動的型チェックを省略した特殊化ノードを選ぶ。

## 言語

### 値の表現

- 1-bit タグ付き `int64_t` (`bit 0 = 1` で 63-bit fixnum, `bit 0 = 0` で
  8-byte aligned ヒープポインタ)
- シングルトン: `unit` / `true` / `false` / `nil` を静的 `mlobj` の
  アドレスで表現
- ヒープ型: `cons` / `string` / `closure` / `prim` / `tuple` / `ref` /
  `real` (boxed double) / `variant` / `exn` / `record` (sorted field-name 配列 + 値配列)

### 制御構造

- `if .. then .. else`
- `case .. of pat => e | ...`
- `e1 ; e2` (sequencing — `(e1; e2; e3)` 形式)
- `let val/fun/datatype ... in expr [; expr]* end`
- `raise e`, `e handle pat => e | ...` (setjmp/longjmp ベース、深さ 256)
- `andalso` / `orelse` / `not` 短絡
- 末尾呼び出しトランポリン (`tc_fn` / `tc_argc` / `tail_call_pending`)、
  parser 後の `mark_tail_calls` post-pass で tail-position の app1/app2 を
  `_tail_app*` に書換 (50M 段の tail recursion が定数スタックで動く)
- `ex_is_leaf(EX *)` で fn が leaf 判定 (body に EX_FN を含まない) →
  `node_fn` の is_leaf=1 で `APPN_FAST_PATH` の inline cache が初回 hit で
  fill されるので、毎呼び出し ml_apply に行かない

### 束縛

- トップ `val x = e` (var パターン)
- トップ `val (a, b) = e` (タプル分解、`$top` 経由で extractor を case で組む)
- トップ `fun f p1 ... pN = e [and ...]` (相互再帰、二段パース)
- トップ `datatype` 宣言 (型推論器の constructor 表に scheme 登録)
- ローカル `let val name = e`, `let fun f ... and ... `
- ローカル `val (a, b) = e` は **未対応** (case で代替)

### 関数

- `fn pat => body` (1 引数; パターンは destructuring 可)
- `fun f x y z = body` (N 引数 curry)
- `fun f (a, b) = body` (タプルパターン引数)
- 部分適用 (`partial_state` センチネルで `OOBJ_PRIM` を装う)
- over-application (`ml_apply` 内で再帰)
- closure に `is_leaf` フラグ — 体内に内側 closure が無いと
  C スタック alloca で frame 確保

### パターンマッチ

PAT struct を再帰的に持つ。`case` の各 arm は `node_match_arm` ノードに
desugar:

- ワイルドカード `_`
- 変数束縛 `x`
- リテラル: `int`, `string`, `true` / `false`, `()`, `[]`, `~int`
- `h :: t`, リストリテラル `[a, b, c]`
- タプル `(p1, ..., pN)`
- 0 引数コンストラクタ `Foo`
- 1 引数コンストラクタ `Foo p`
- record `{f1 = p1, ...}` および短縮形 `{f1, f2, ...}` (= `{f1 = f1, ...}`)

裸 ID で未登録のものは pattern では変数束縛として扱う (SML 慣習)。

### Record

- リテラル `{x = 1, y = 2}` — フィールドはパース時にアルファベット順で
  ソート (型 unify と structural eq が位置比較で済むように)
- フィールド選択 `#x e` (HM が record 型を確定すると `node_field` が
  選ばれて IS_RECORD チェックなしの linear search で値を取得)
- `#x` 単独 (引数なし) は `fn $r => #x $r` の closure に desugar — 高階で
  渡せる
- record パターン `{x, y}` / `{x = a, y = b}` を case / fun で使える
- `datatype foo = Pt of {x : int, y : int}` のような datatype 内 record 型
  もパース可
- structural equality / compare は field 名と値ペアを順に比較

### 演算子

- 整数: `+ - * div mod` (int -> int -> int)、単項 `~`
- 実数: `/` (real -> real -> real)
- 多相比較: `< <= > >= = <>` — 推論結果が `int` なら `node_*_int` に特殊化
- 文字列: `^` (string -> string -> string)
- リスト: `::` (cons), `@` (List.append、prim 経由)
- 参照: `!`, `:=`
- `op +` 等: 演算子を関数値として取得 (グローバルから `prim_*` を引く)

### 例外

- 組み込み: `Match` / `Div` / `Empty` / `Fail of string`
- `raise e` (e : exn)
- `e handle pat => h | ...`
- ユーザ定義 `exception E` は **未対応**

## 型システム

- Hindley-Milner full
- Algorithm W ベース、Remy のレベル法で一般化
- 値制約 (value restriction): syntactic value (literal, lambda, ctor of
  values, tuple of values, lref/gref) のみが let-poly で一般化される
- Constructor scheme: 各 datatype の各 ctor が `forall vars. (arg ->)? con args`
  形式の scheme を持つ
- `'a list`, `'a option`, `'a ref`, `T1 * T2`, `T1 -> T2` の組合せを推論
- 型エラーは行番号つきで報告 (`asml: type error at line N: cannot unify X with Y`)
- 推論未通過コードは exit 2 で実行を拒否 (動的チェックのフォールバックなし)

## 型駆動特殊化

`lower_expr` は推論結果 `ex->ty` を見て、operand 型に応じた専用ノードを
選ぶ。**動的型チェック (IS_INT, IS_BOOL, IS_REF, IS_STRING) を含む
generic ノードは node.def から削除済み** — HM が型を絞れなかった式は
infer 段階で reject される。

| EX 種別 | 推論結果 → ノード |
|---|---|
| `BO_ADD/SUB/MUL/DIV/MOD` | `_int` (HM が int を保証) |
| `BO_RDIV` (`/`) | `node_rdiv` (HM が real を保証) |
| `BO_LT/LE/GT/GE/EQ/NE` | int → `_int` / real → `_real` / string → `_string` / その他 → `_poly` (`ml_compare` / `ml_structural_eq`) |
| `UO_NEG` | int → `_int` / real → `_real` |
| `UO_NOT` | `node_not_bool` (HM が bool を保証) |
| `UO_DEREF` | `node_deref_unchecked` (HM が ref を保証) |
| `EX_IF` | `node_if_bool` (cond が bool) |
| `EX_ANDALSO/ORELSE` | `node_andalso_bool` / `_bool` |
| `EX_ASSIGN` | `node_assign_unchecked` (lhs が ref) |
| `BO_CONCAT` | `node_concat_str` (両辺が string) |

generic 系は **node.def から物理的に削除済み**。`node_add` を呼ぼうとしても
リンクエラーになる。コンパイル後 SD には `_int / _bool / _str / _real /
_string / _poly / _unchecked` 系しか出現しない。

削除した generic ノード:
- `node_add / sub / mul / div / mod / neg / abs`
- `node_radd / rsub / rmul` (`+. -. *.` を未サポートなので未到達)
- `node_lt / le / gt / ge / eq / ne` (動的 IS_INT fast-path 付きだった)
- `node_if / andalso / orelse / not`
- `node_concat / deref / assign`
- `node_let_pat` (実装したが未使用)

## ASTro 統合

- `node.def` で全ノード定義 → ASTroGen 生成
- `astro_cs_init/load/compile/build/reload` を `-c` で起動時に呼ぶ
- closure body は parse 時に `aot_add_entry` 登録 → 1 つの top form の
  AOT で全関数本体が specialise される
- `@ref` operand: `gref_cache` (グローバル参照 IC) と `app_cache`
  (closure call site IC)。`asml_gen.rb` で扱い
- `maybe_aot_compile` で `ASTRO_EXTRA_CFLAGS` に
  `-fno-stack-clash-protection -fno-stack-protector -flto -finline-limit=10000
  --param max-inline-insns-auto=400 --param inline-unit-growth=300` を
  注入 (astocaml と同). `-flto` で SD 間 inline、stack-clash protection の
  alloca probe loop を切る。fib(35) AOT が 1.59 → 0.16 s (10× 速)
- `node_topbind` の specializer は no-op (動的処理のみ)、closure body だけ
  specialise する戦略

## CLI

```
asml [options] [file.sml]
  -e EXPR        EXPR を評価して exit
  -c | --compile  各 top form を AOT compile
  -q | --quiet   進捗を抑制
  --no-compile   astro_cs_load を無効化 (specialised .so の復号スキップ)
```

引数なしで起動すると簡易 REPL (`- ` プロンプト)。
