# ASTroGen の生成物と node.def

核心アイデアは [idea.md](./idea.md)。本書は ASTroGen ツールが何を生成するか、
`node.def` の形式、ノードの共通構造をまとめる。
実践的な書き方（context.h / node.h / Makefile の整備等）は [usage.md](./usage.md) を参照。

## 1. 生成物

`lib/astrogen.rb` (約 780 行の Ruby スクリプト) が、`node.def` から以下を自動生成:

| 生成ファイル | 内容 |
|---|---|
| `node_head.h` | AST ノードの構造体定義（union でまとめる）、関数ポインタ typedef、NodeKind 構造体 |
| `node_eval.c` | EVAL_xxx 関数（ユーザ定義のロジックをそのまま出力） |
| `node_dispatch.c` | DISPATCH_xxx 関数（フィールド抽出 → EVAL呼び出し） |
| `node_hash.c` | HASH_xxx 関数（Merkle ツリーハッシュ計算） |
| `node_alloc.c` | ALLOC_xxx 関数 + NodeKind 定義 |
| `node_specialize.c` | SPECIALIZE_xxx 関数（特化Cコード生成=部分評価器） |
| `node_dump.c` | DUMP_xxx 関数（デバッグ用ノード表示） |
| `node_replace.c` | REPLACER_xxx 関数（子ノード差し替え） |

生成タスクは `register_gen_task` で登録されており、サブクラスでカスタムタスク（例: GC マーク関数）を追加可能。

## 2. node.def の形式

```c
NODE_DEF [@option]
node_name(CTX *c, NODE *n, type1 operand1, type2 operand2, ...)
{
    // C code for evaluation
    // EVAL_ARG(c, child_node) で子ノードを評価
}
```

- 最初の2引数 `CTX *c, NODE *n` は必須（コンテキストとノード自身）
- `NODE *` 型のオペランドは子ノード（lazy: body が `EVAL_ARG` で評価する）
- `VALUE <name>@child` で **strict 引数化** — DISPATCH が子を評価して VALUE
  として渡す。snapshot 場所は `Node#child_storage_expr` で言語ごとに選択可
  (デフォルト `sp[i]`、libgc 系言語なら C ローカルに変更可)
- `@noinline` オプションで特化時のインライン抑制が可能
- オペランド名末尾に `@ref` を付けると、ノード本体に値を埋め込まずポインタ経由で扱う（インラインキャッシュ等のミュータブルな副情報用）

## 3. サポートするオペランド型

- `NODE *` — 子 AST ノード（特別扱い: ディスパッチャ経由の呼び出し）
- `int32_t`, `uint32_t` — 整数フィールド
- `const char *` — 文字列フィールド
- その他ユーザ定義型（ハッシュ関数のカスタマイズが必要な場合あり）

可変長の子ノード列（`call(f, a, b, c)` のような arity 可変ノード）の扱いは
[idea_variadic.md](./idea_variadic.md) を参照。

## 4. NodeHead 構造

全ノードは共通ヘッダ `NodeHead` を持つ:

```c
struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;          // PGC: hash_opt が確定している
        bool is_specialized;
        bool is_specializing;       // 再帰的特化防止
        bool is_dumping;            // 再帰的ダンプ防止
        bool no_inline;             // 特化時のインライン抑制
    } flags;
    const struct NodeKind *kind;
    struct Node *parent;
    node_hash_t hash_value;             // Horg: 構造ハッシュ (AOT)
    node_hash_t hash_opt;               // Hopt: profile-baked ハッシュ (PGC)
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;  // 関数ポインタ
    enum jit_status { ... } jit_status; // JIT 状態管理
    unsigned int dispatch_cnt;          // ディスパッチ回数
    int line;                           // 診断用ソース行番号
};
```

各ノード型は `NodeKind` 構造体を持ち、デフォルトディスパッチャ・ハッシュ関数・特化関数・ダンプ関数・子ノード差し替え関数へのポインタを格納。`NodeKind` の構造体定義は `node_head.h` に自動生成される。
