# ASTro: AST-Based Reusable Optimization Framework

著者: 笹田耕一 (ko1@st.inc, STORES株式会社)

論文:
- VMIL 2025: "ASTro: An AST-Based Reusable Optimization Framework"
- PPL 2026: "ASTro による JIT コンパイラの試作"

## 1. 問題意識

高性能なインタプリタを作るには、一般的に以下の段階を経る:

1. AST を構築し、木を辿るインタプリタで実行（簡単だが遅い）
2. バイトコードVM を作る（性能向上するが開発コスト大）
3. JIT コンパイラを追加（さらに高速だが複雑）

各段階で VM の設計・JIT バックエンドなど言語ごとに専用の実装が必要。
Truffle/Graal や RPython は高性能だが、重量級ツールチェインに依存し、ポータビリティやデバッグ性に難がある。

## 2. ASTro の核心アイデア

### 2.1 部分評価 + C コンパイラ

- インタプリタの **部分評価 (Partial Evaluation)** の結果を **C のソースコード** として出力
- 汎用の C コンパイラ（gcc, clang 等）でコンパイルすることで、高品質なネイティブコードを得る
- C コンパイラは成熟した最適化基盤であり、新たなバックエンド開発が不要
- 第一二村射影: インタプリタ I をプログラム P に対して部分評価すると、P の残余プログラム PE(I, P) が得られる

### 2.2 ディスパッチャとエバリュエータの分離

ASTro の最も重要な設計判断:

- **エバリュエータ (EVAL_xxx)**: ノードの評価ロジック本体。ユーザが node.def に記述
- **ディスパッチャ (DISPATCH_xxx)**: ノードのフィールドを取り出してエバリュエータに渡す薄い関数。ASTroGen が自動生成

この分離により:
- 部分評価は **ディスパッチャだけを特化** すれば良い（エバリュエータは変更不要）
- 特化ディスパッチャは具体的な関数ポインタを埋め込むため、C コンパイラがインライン展開可能
- ユーザ定義のエバリュエータに手を加えずに最適化が実現

例: `1 + 2 * 3` の AST を特化すると、最外部のディスパッチャは `mov $0x7, %eax; ret` にまでインライン化される。

### 2.3 Merkle ツリーハッシュ

- 各 AST ノードに Merkle ツリーハッシュを付与
- ハッシュは (ノードの種類, 各属性のハッシュ) から計算（子ノードは再帰的）
- 同一構造の部分木は同一ハッシュ → **プロセス間・マシン間でコンパイル結果を共有可能**
- 特化ディスパッチャの関数名は `SD_<hash>` 形式

## 3. ASTroGen ツール

`lib/astrogen.rb` (約580行の Ruby スクリプト) が、`node.def` から以下を自動生成:

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

### 3.1 node.def の形式

```c
NODE_DEF [@option]
node_name(CTX *c, NODE *n, type1 operand1, type2 operand2, ...)
{
    // C code for evaluation
    // EVAL_ARG(c, child_node) で子ノードを評価
}
```

- 最初の2引数 `CTX *c, NODE *n` は必須（コンテキストとノード自身）
- `NODE *` 型のオペランドは子ノード（ディスパッチャが自動的にディスパッチ関数ポインタも渡す）
- `@noinline` オプションで特化時のインライン抑制が可能
- `@rewritable` オプションで `replaced_from` フィールドを追加（実行時のノード書き換え対応）

### 3.2 サポートするオペランド型

- `NODE *` — 子 AST ノード（特別扱い: ディスパッチャ経由の呼び出し）
- `int32_t`, `uint32_t` — 整数フィールド
- `const char *` — 文字列フィールド
- その他ユーザ定義型（ハッシュ関数のカスタマイズが必要な場合あり）

### 3.3 NodeHead 構造

全ノードは共通ヘッダ `NodeHead` を持つ:

```c
struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool is_specialized;
        bool is_specializing;   // 再帰的特化防止
        bool is_dumping;        // 再帰的ダンプ防止
        bool no_inline;         // 特化時のインライン抑制
    } flags;
    const struct NodeKind *kind;
    struct Node *parent;
    node_hash_t hash_value;
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;  // 関数ポインタ
    enum jit_status { ... } jit_status; // JIT 状態管理
    unsigned int dispatch_cnt;          // ディスパッチ回数
};
```

各ノード型は `NodeKind` 構造体を持ち、デフォルトディスパッチャ・ハッシュ関数・特化関数・ダンプ関数・子ノード差し替え関数へのポインタを格納。`NodeKind` の構造体定義は `node_head.h` に自動生成される。

## 4. 4つの利用モード

### 4.1 Plain Interpreter（ベースラインインタプリタ）

- 部分評価を使わず、DISPATCH → EVAL の木巡回インタプリタとして実行
- node.def さえあれば動作する最もシンプルな形態

### 4.2 AOT Compiler（事前コンパイル）

- パース後、AST を部分評価器に渡して特化 C コードを生成
- C コンパイラでコンパイルし、特化インタプリタ or スタンドアロン実行ファイルを生成
- コンパイル時間は含まないが、実行は高速

### 4.3 Profile-Guided (PG) Compilation

- 1回目の実行でプロファイル情報を収集
- 2回目以降、プロファイル情報に基づいて特化
- call サイトのインラインキャッシュ情報を特化器に渡せる（node_call2）

### 4.4 JIT Compiler（実行時コンパイル）

実行中に C コンパイラを呼び出して特化コードを動的にロード。

## 5. ASTro JIT の設計

### 5.1 階層型キャッシュアーキテクチャ (L0/L1/L2)

```
Execution Machine                    Compile Machine
┌─────────────────────┐             ┌──────────────────┐
│ Interpreter Process │             │                  │
│ ┌───────┐ ┌──────┐ │             │  Remote code     │
│ │Interp │ │ L0   │ │   L1        │  store           │
│ │Thread │ │Thread│ ├────(Unix)───┤                  │
│ └───────┘ └──────┘ │   socket    │  ┌────┐ ┌────┐  │
│                     │             │  │ L2 │ │ L2 │  │
│ Local code store    │             │  └────┘ └────┘  │
│ H→code mapping      │             │  C compilers     │
└─────────────────────┘             └──────────────────┘
```

- **L0**: インタプリタプロセス内のスレッド (C, Pthread)
  - インタプリタスレッドから同期キューでリクエストを受ける
  - 非同期にディスパッチ関数を差し替え
  - ローカルコードストアを参照
- **L1**: 同一マシン上のデーモンプロセス (Ruby)
  - Unix domain socket で L0 と通信
  - TCP で L2 と通信
  - 複数の L0 と 1つの L2 に接続
- **L2**: コンパイル用マシン上のデーモン (Ruby)
  - ソースコードを受け取り、C コンパイラでコンパイル
  - 複数の L1 と通信可能
  - L1 と L2 は同一マシンでも別マシンでもよい

### 5.2 通信メッセージ

2種類:

- **query(h)**: ハッシュ h に対応するコンパイル済みネイティブコードがあるか問い合わせ
- **compile(h, src)**: ハッシュ h に対応するソースコード src のコンパイルを要求

メッセージ形式: (種類, サイズ, ハッシュ値, ペイロード) の4つ組バイナリプロトコル。

### 5.3 AST の状態遷移

各ノードは以下の状態を持つ:

```
Unknown → Querying → NotFound → Compiling → Compiled
                   ↘ (found)→ Compiled
```

- **Unknown**: まだ query していない
- **Querying**: query 送信済み、返答待ち
- **NotFound**: コンパイル済みコードが存在しない
- **Compiling**: compile 送信済み、返答待ち
- **Compiled**: コンパイル済みネイティブコードあり

### 5.4 コードストアの実装

ファイルシステム + 共有オブジェクト (.so) ベース:

- **ローカルコードストア** (L0, L1): `<hash>.so` ファイル。`dlsym()` で `SD_<hash>` 関数を検索
- **リモートコードストア** (L2): `<hash>.o` ファイル。なければ C コンパイラで生成
- **all.so**: 複数の .o をリンクしてまとめた共有オブジェクト。L0 起動時にまずこれをロード

効率化: L1 が暇なタイミングで既存の .o を all.so にまとめてリンカで生成。

### 5.5 JIT トリガー条件

- プログラムロード時: 各 AST について `query(h)` を発行（既存キャッシュの利用）
- ある関数を100回以上実行したとき: `compile` メッセージで特化を要求

## 6. naruby（評価用言語）

"Not A Ruby" — Ruby の文法だが機能を大幅に制限。node.def は約300行。

### 6.1 ノード型一覧 (21種)

| カテゴリ | ノード |
|---|---|
| リテラル | `node_num` (整数) |
| 制御フロー | `node_seq`, `node_if`, `node_while` |
| 変数 | `node_scope`, `node_lget`, `node_lset` |
| 関数 | `node_def`, `node_call`, `node_call2`, `node_call_static`, `node_call_builtin` |
| 二項演算 | `node_add`, `node_sub`, `node_mul`, `node_div`, `node_mod` |
| 比較 | `node_eq`, `node_neq`, `node_lt`, `node_le`, `node_gt`, `node_ge` |

### 6.2 ランタイム

- 値の型は符号付き整数のみ
- バリュースタック方式（固定サイズフレーム）
- グローバル関数テーブル (name, arity, AST) の3つ組
- インラインキャッシュ: function-table version でキャッシュ有効性を判定
- フロントエンド: Prism パーサ（Ruby 標準パーサ）を利用し、ALLOC_* で AST を構築

### 6.3 ベンチマーク結果 (x86_64)

| 構成 | loop | fib | call | prime_count |
|---|---|---|---|---|
| naruby/interpret | 0.786 | 4.870 | 6.760 | 6.170 |
| naruby/compiled (AOT) | 0.001 | 1.093 | 3.435 | 0.444 |
| naruby/pg (Profile-Guided) | 0.001 | 1.143 | 2.061 | 0.443 |
| gcc -O0 | 0.042 | 0.480 | 1.121 | 0.490 |
| gcc -O2 | 0.001 | 0.115 | 0.318 | 0.434 |

AOT コンパイルで gcc -O0 に迫る性能。loop ベンチマークではループ自体が最適化で消える。

### 6.4 JIT 予備評価 (prime? ベンチマーク)

| 条件 | 実行時間(秒) |
|---|---|
| JIT なし | 13.64 |
| JIT あり（初回） | 1.11 |
| JIT あり（2回目, キャッシュ済み） | 1.05 |

## 7. 既知の課題と今後の方向

### 独自ローダ
- 現在は .so ファイルベースだが、ページアライメントにより小さな関数でもメモリ浪費
- 専用のネイティブコードローダーで密にメモリ上にロードする必要あり
- ただし計算機環境ごとの実装が必要で、ポータビリティとのトレードオフ

### コードストア上限サイズ
- 現在は生成コードを削除しない → 実運用では LRU 等で上限管理が必要

### 投機的プリフェッチ
- L2 への query が h1, h2, h3... と連続する場合、h4, h5... を投機的にプリフェッチ可能

### AOT + JIT の併用
- 実行時情報なしの AOT（そこそこ速い）+ 実行時情報ありの JIT（さらに速い）を組み合わせ
- 初回ロード時バーストを AOT が軽減、ホットパスを JIT が最適化

### コードサイズ膨張
- 部分評価を無差別に適用するとコードサイズが爆発
- ハッシュ関数のカスタマイズで制御可能（例: 大きな定数は固定ハッシュにまとめる）

### 例外処理
- C での例外実装は (1) 毎回エラーフラグチェック or (2) setjmp/longjmp
- C++ の try/catch も選択肢

### 静的型システム
- 現在は動的型付き言語のみ検証。静的型情報を活用した最適化は未探索

## 8. リポジトリ構成

```
/home/ko1/ruby/astro/
├── lib/
│   └── astrogen.rb           # ASTroGen コア (約580行 Ruby)
├── sample/
│   ├── calc/                 # 最小例 (3ノード: num, add, mul)
│   │   └── node.def
│   ├── naruby/               # Ruby サブセット (21ノード、JIT対応)
│   │   ├── node.def          # ノード定義 (約300行)
│   │   ├── Makefile
│   │   ├── main.c            # エントリポイント
│   │   ├── naruby_parse.c    # Prism パーサ連携
│   │   ├── node.c            # ノード実装
│   │   ├── astro_jit.c       # JIT 実装 (L0)
│   │   ├── astro_jit.h
│   │   ├── context.h         # 実行コンテキスト
│   │   ├── prism/            # Prism パーサ (submodule)
│   │   └── bench/            # ベンチマーク
│   └── abruby/               # Ruby サブセット (40+ノード、CRuby C extension)
│       ├── node.def          # ノード定義
│       ├── abruby_gen.rb     # ASTroGen 拡張 (GC マーク関数生成)
│       ├── abruby.c          # C extension エントリ
│       ├── node.c            # ランタイム
│       ├── context.h         # 実行コンテキスト
│       ├── node.h            # ノード宣言
│       ├── builtin/          # ビルトインクラス (Integer, String, Array 等)
│       ├── lib/abruby.rb     # Prism AST → AbRuby AST 変換
│       ├── exe/abruby        # CLI ツール
│       └── test/             # テスト (607 tests)
└── docs/
    ├── idea.md               # 設計思想
    ├── usage.md              # 使い方ガイド
    ├── 2025_astro_VMIL2025.pdf
    └── ppl2026_astro_jit_cr.pdf
```
