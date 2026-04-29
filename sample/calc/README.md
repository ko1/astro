# calc — toy calculator on ASTro

ASTro でいちばん小さい end-to-end サンプル。整数の四則演算 (+ 余り) だけを
扱う電卓 REPL。`node.def` は 6 ノードのみで、ASTroGen が生成したインタプリタ
+ ディスパッチャ + 部分評価器をそのまま使う。ASTro 側の機能 (Merkle ハッシュ、
特化、code store による `.so` 共有) を最小コードで一通り体験できる。

ASTro フレームワーク全体については
[`../../docs/idea.md`](../../docs/idea.md) を、ASTroGen の使い方は
[`../../docs/usage.md`](../../docs/usage.md) を参照。

## ノード

| ノード | オペランド | 意味 |
|---|---|---|
| `node_num` | `int32_t num` | 整数リテラル |
| `node_add` | `NODE *l, NODE *r` | `l + r` |
| `node_sub` | `NODE *l, NODE *r` | `l - r` |
| `node_mul` | `NODE *l, NODE *r` | `l * r` |
| `node_div` | `NODE *l, NODE *r` | `l / r` |
| `node_mod` | `NODE *l, NODE *r` | `l % r` |

## ビルド & 実行

```sh
make            # ./calc を生成
./calc          # REPL 起動
calc> 1 + 2 * 3
=> 7
calc> (10 - 3) % 4
=> 3
```

CLI:

```
-q, --quiet     特化進捗を抑制
--no-compile    code store を一切使わない (純インタプリタ)
```

入力ごとに以下が走る:

1. 自前の再帰下降パーサ (`main.c`) が AST を構築
2. `astro_cs_compile` → `astro_cs_build` → `astro_cs_reload` で
   `code_store/SD_<hash>.{c,o}` を生成・リンクして `all.so` に投入
3. `astro_cs_load` でノードのディスパッチャを特化版へ差し替え
4. `astro_cs_disasm` で生成された機械語を表示
5. `EVAL` で実行、結果を `=> N` で表示

部分評価が効くと、たとえば `1 + 2 * 3` の最外部ディスパッチャが
`mov $0x7, %eax; ret` まで畳まれるのが `astro_cs_disasm` で確認できる。

## ファイル

```
sample/calc/
├── README.md             この文書
├── Makefile              ASTroGen 起動 + ./calc ビルド
├── node.def              6 ノード定義
├── node.h / context.h    NODE / NodeHead / CTX 宣言
├── node.c                ランタイム配線 (生成 .c の include)
└── main.c                パーサ + REPL ドライバ
```

`node_alloc.c` / `node_dispatch.c` / `node_eval.c` / `node_hash.c` /
`node_specialize.c` / `node_dump.c` / `node_replace.c` /
`node_head.h` は ASTroGen が `node.def` から生成。
