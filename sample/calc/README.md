# calc — toy calculator on ASTro

`calc` は ASTro でいちばん小さい end-to-end サンプル。整数の四則演算 + 余り
だけを扱う電卓で、`node.def` はわずか 6 ノード。これを 1 本走らせるだけで
ASTro の主要機構 (Merkle ハッシュ → 部分評価 → C コンパイル → `dlopen` →
ディスパッチャ差し替え) を一通り体験できる。本 README はそのチュートリアル。

設計思想は [`../../docs/idea.md`](../../docs/idea.md)、ASTroGen の使い方は
[`../../docs/usage.md`](../../docs/usage.md) を参照。

## 1. インストールと最初の実行

### 前提パッケージ (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby libreadline-dev   # libreadline-dev は任意
```

ASTroGen を呼ぶために `ruby` (3.x) が必要。`libreadline-dev` を入れておくと
REPL で履歴・行編集が使える (なくてもビルドは通り、`fgets` にフォールバック)。

### ビルド

```sh
$ make             # ASTroGen を呼んで生成 → ./calc を build
$ ./calc -e '1 + 2 * 3'
7
```

REPL モード:

```
$ ./calc
calc> 1 + 2 * 3
=> 7
calc> (10 - 3) % 4
=> 3
calc> ^D
```

## 2. 言語

ノード 6 種、演算子 5 つ + 整数リテラル。これが全文法:

| ノード | オペランド | 意味 |
|---|---|---|
| `node_num` | `int32_t num` | 整数リテラル |
| `node_add` | `NODE *l, NODE *r` | `l + r` |
| `node_sub` | `NODE *l, NODE *r` | `l - r` |
| `node_mul` | `NODE *l, NODE *r` | `l * r` |
| `node_div` | `NODE *l, NODE *r` | `l / r` |
| `node_mod` | `NODE *l, NODE *r` | `l % r` |

[`node.def`](./node.def) は ~30 行。各 `NODE_DEF` ブロックがインタプリタ
本体 (`EVAL_xxx`) と部分評価のテンプレを兼ねている。

## 3. 中を覗く: `--disasm`

`--disasm` を付けると、ASTro が生成した特化コードの逆アセンブルが見える:

```
$ ./calc -q --disasm -e '1 + 2 * 3'
# compiled with GCC: (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
0000000000001100 <SD_dfb75fdabb0d5ef6>:
    1100:	endbr64
    1104:	mov    $0x7,%eax
    1109:	ret
7
```

定数式 `1 + 2 * 3` の最外部ディスパッチャが `mov $0x7, %eax; ret` まで
畳まれているのが見える。生成関数名 `SD_dfb75fdabb0d5ef6` は AST の
Merkle ハッシュで、同じ AST が再評価されればキャッシュヒットして再
ビルドはされない。

`--disasm` を付けないときは、特化と差し替えはバックグラウンドで黙って
走り、結果だけ表示される。ASTro の特徴である「ユーザコードを書き換えず
裏で C 化する」がそのまま見える。

## 4. 入力ごとに何が起きているか

`-e EXPR` あるいは REPL の各行で、内部はこう動いている
(該当コードは [`main.c`](./main.c) `evaluate()`):

1. 自前の再帰下降パーサ (同じく `main.c`) が AST を構築。
2. `astro_cs_compile(ast, NULL)` が `code_store/SD_<hash>.c` を吐く。
   ハッシュはノード種別 + 子のハッシュから決まる Merkle ハッシュなので、
   同じサブ AST は同じ `.c` を共有する。
3. `astro_cs_build(NULL)` が新しい `.c` を `gcc` し、`.o` を
   `code_store/all.so` にまとめる。
4. `astro_cs_reload()` が `all.so` を `dlopen`。
5. `astro_cs_load(ast, NULL)` が各ノードの `dispatcher` フィールドを
   汎用インタプリタから `dlsym("SD_<hash>")` で得た特化版に差し替え。
6. `EVAL(c, ast)` で 1 回ツリーを歩く。特化済みノードは特化関数を
   呼ぶだけなので、定数畳み込み済みの式なら 1 命令で答えが返る。

`--no-compile` を付けると 2-5 をスキップして純粋なツリーウォーカに
戻せる。同じ ASTroGen 出力が「インタプリタ」と「JIT のフロントエンド」
の両方に使える、というのが ASTro の中心アイデア
([`docs/idea.md`](../../docs/idea.md))。

## 5. CLI

| オプション | 効果 |
|---|---|
| `-e EXPR` | EXPR を 1 回評価して終了 (REPL を立ち上げない) |
| `--disasm` | 特化コードの逆アセンブルを表示 |
| `--no-compile` | 特化を一切行わない (純インタプリタ) |
| `-q`, `--quiet` | hit/miss の進捗メッセージを抑制 |
| `-h`, `--help` | 使い方を表示 |

## 6. ファイル構成

```
sample/calc/
├── README.md           この文書
├── Makefile            ASTroGen 起動 + ./calc ビルド
├── node.def            6 ノードの定義 (=言語仕様)
├── node.h, context.h   NODE / NodeHead / CTX 宣言
├── node.c              ランタイム配線 (生成 .c の include)
└── main.c              パーサ + REPL/-e ドライバ
```

ASTroGen が `node.def` から生成するファイル (すべて `node.c` から
include される):

| 生成ファイル | 中身 |
|---|---|
| `node_alloc.c` | `ALLOC_node_xxx()` コンストラクタ |
| `node_dispatch.c` | `DISPATCH_xxx` ラッパ (特化対象) |
| `node_eval.c` | `EVAL_xxx` 評価器本体 (`node.def` のコード) |
| `node_hash.c` | 各ノード種の Merkle ハッシュ |
| `node_specialize.c` | 部分評価器 (`SD_<hash>.c` を吐く) |
| `node_dump.c` | AST テキストダンプ |
| `node_replace.c` | `NODE *→NODE *` 置換ヘルパ |
| `node_head.h` | ノード種メタデータ構造体 |

C コンパイラ呼び出し / リンク / `dlopen` 等のランタイムは calc 専用
ではなく [`runtime/astro_code_store.{h,c}`](../../runtime/) に置かれて
いて、全サンプルで共有している。

## 7. 次に読むもの

- もう一段大きい例: [`sample/naruby/`](../naruby/) — 同じ仕組みで Ruby
  サブセット (整数のみ、21 ノード) を動かし、論文では JIT も評価。
- ASTroGen の DSL 仕様: [`docs/usage.md`](../../docs/usage.md)。
- 「なぜ AST に対する部分評価か」: [`docs/idea.md`](../../docs/idea.md)。
