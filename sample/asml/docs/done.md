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
  `real` (boxed double) / `variant` / `exn`

### 制御構造

- `if .. then .. else`
- `case .. of pat => e | ...`
- `e1 ; e2` (sequencing — `(e1; e2; e3)` 形式)
- `let val/fun/datatype ... in expr [; expr]* end`
- `raise e`, `e handle pat => e | ...` (setjmp/longjmp ベース、深さ 256)
- `andalso` / `orelse` / `not` 短絡
- 末尾呼び出しトランポリン (`tc_fn` / `tc_argc` / `tail_call_pending`)

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

裸 ID で未登録のものは pattern では変数束縛として扱う (SML 慣習)。

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

`lower_expr` は推論結果 `ex->ty` を見てノードを選ぶ:

| EX 種別 | デフォルト | 型確定時 |
|---|---|---|
| `BO_ADD/SUB/MUL/DIV/MOD` | 常に `_int` | (int でなければ型エラー) |
| `BO_RDIV` | 常に generic `node_rdiv` | (real でなければ型エラー) |
| `BO_LT/LE/GT/GE/EQ/NE` | `node_lt` 等 (多相) | operand が `int` で `_int` |
| `UO_NEG` | `node_neg` | operand が `int` で `_int` |
| `UO_NOT` | (常に bool) | `node_not_bool` |
| `UO_DEREF` | (常に ref) | `node_deref_unchecked` |
| `EX_IF` | (常に bool 条件) | `node_if_bool` |
| `EX_ANDALSO/ORELSE` | (常に bool) | `node_andalso_bool` / `_bool` |
| `EX_ASSIGN` | (常に ref) | `node_assign_unchecked` |
| `BO_CONCAT` | (常に string) | `node_concat_str` |

generic 系 (`node_add` 等) は **コンパイル後 SD 上に残らない** ことを確認済
(`grep EVAL_node_ code_store/c/*.c`)。

## ASTro 統合

- `node.def` で全ノード定義 → ASTroGen 生成
- `astro_cs_init/load/compile/build/reload` を `-c` で起動時に呼ぶ
- closure body は parse 時に `aot_add_entry` 登録 → 1 つの top form の
  AOT で全関数本体が specialise される
- `@ref` operand: `gref_cache` (グローバル参照 IC) と `app_cache`
  (closure call site IC)。`asml_gen.rb` で扱い

## CLI

```
asml [options] [file.sml]
  -e EXPR        EXPR を評価して exit
  -c | --compile  各 top form を AOT compile
  -q | --quiet   進捗を抑制
  --no-compile   astro_cs_load を無効化 (specialised .so の復号スキップ)
```

引数なしで起動すると簡易 REPL (`- ` プロンプト)。
