# ASTro JIT の設計

核心アイデアは [idea.md](./idea.md)。JIT は「実行中に特化コードをコンパイル・ロードする」
という核心機構の応用であり、コンパイル済みコードはすべて Merkle ハッシュをキーに
共有される。保存・ロードの低レベル機構は [idea_code_store.md](./idea_code_store.md)。

## 1. 階層型キャッシュアーキテクチャ (L0/L1/L2)

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

## 2. 通信メッセージ

2種類:

- **query(h)**: ハッシュ h に対応するコンパイル済みネイティブコードがあるか問い合わせ
- **compile(h, src)**: ハッシュ h に対応するソースコード src のコンパイルを要求

メッセージ形式: (種類, サイズ, ハッシュ値, ペイロード) の4つ組バイナリプロトコル。

## 3. AST の状態遷移

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

## 4. コードストアの実装

ファイルシステム + 共有オブジェクト (.so) ベース:

- **ローカルコードストア** (L0, L1): `<hash>.so` ファイル。`dlsym()` で `SD_<hash>` 関数を検索
- **リモートコードストア** (L2): `<hash>.o` ファイル。なければ C コンパイラで生成
- **all.so**: 複数の .o をリンクしてまとめた共有オブジェクト。L0 起動時にまずこれをロード

効率化: L1 が暇なタイミングで既存の .o を all.so にまとめてリンカで生成。

統一 API (`astro_cs_*`) としての整理は [idea_code_store.md](./idea_code_store.md) を参照。

## 5. JIT トリガー条件

- プログラムロード時: 各 AST について `query(h)` を発行（既存キャッシュの利用）
- ある関数を100回以上実行したとき: `compile` メッセージで特化を要求

## 6. AOT との併用

- naruby は AOT (PGC) + JIT を共存。koruby / pystro 等は AOT 中心で
  起動時に既存 SD をロードして使う運用が確立。
- 初回ロード時バーストを AOT が吸収、ホットパスを JIT が再最適化、という構図は naruby で実証済み。

## 7. 予備評価 (PPL2026, prime? ベンチマーク)

| 条件 | 実行時間(秒) |
|---|---|
| JIT なし | 13.64 |
| JIT あり（初回） | 1.11 |
| JIT あり（2回目, キャッシュ済み） | 1.05 |

JIT-on で 12× speedup。2回目はコンパイル済みコードがハッシュで引けるため、
コンパイルコストなしで初回より速い。naruby の AOT/PG 評価は
[idea_evaluation.md](./idea_evaluation.md)。
