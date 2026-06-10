# 論文評価 — naruby とベンチマーク

核心アイデアは [idea.md](./idea.md)。本書は論文 (VMIL2025 / PPL2026) の評価に使った
**naruby** の構成とベンチマーク結果をまとめる。

ASTro は `sample/` 以下に多数のサンプルを抱えており、教育用の最小例から
本格的な動的言語、関数型言語、スタックマシン、DSL までを横断的にカバー
する。サンプル横断の比較は [`samples.md`](./samples.md) に集約してあるので、
本書では論文評価で使った naruby を代表として要約する。各サンプルの
実装詳細・性能数値は `sample/<lang>/README.md` と
`sample/<lang>/docs/{done,todo,perf,runtime}.md` に。

## 1. naruby — 論文評価用 Ruby サブセット

"Not A Ruby" — Ruby の文法だが機能を大幅に制限。`node.def` は約 570 行、
36 ノード型。1 バイナリで 4 つの実行モード (interpret / AOT / PG / JIT)
を切り替えられる、フレームワーク自身の評価用言語。

主なノード分類:

| カテゴリ | ノード |
|---|---|
| リテラル | `node_num` (整数) |
| 制御フロー | `node_seq`, `node_if`, `node_while` |
| 変数 | `node_scope`, `node_lget`, `node_lset` |
| 関数 | `node_def`, `node_call`, `node_call2`, `node_call_static`, `node_call_builtin` |
| 二項演算 | `node_add`, `node_sub`, `node_mul`, `node_div`, `node_mod` |
| 比較 | `node_eq`, `node_neq`, `node_lt`, `node_le`, `node_gt`, `node_ge` |

## 2. naruby ランタイム

- 値の型は符号付き整数のみ
- バリュースタック方式（固定サイズフレーム）
- グローバル関数テーブル (name, arity, AST) の3つ組
- インラインキャッシュ: function-table version でキャッシュ有効性を判定
- フロントエンド: Prism パーサ（Ruby 標準パーサ）を利用し、ALLOC_* で AST を構築

## 3. ベンチマーク (VMIL2025 当時, x86_64)

| 構成 | loop | fib | call | prime_count |
|---|---|---|---|---|
| naruby/interpret | 0.786 | 4.870 | 6.760 | 6.170 |
| naruby/compiled (AOT) | 0.001 | 1.093 | 3.435 | 0.444 |
| naruby/pg (Profile-Guided) | 0.001 | 1.143 | 2.061 | 0.443 |
| gcc -O0 | 0.042 | 0.480 | 1.121 | 0.490 |
| gcc -O2 | 0.001 | 0.115 | 0.318 | 0.434 |

AOT コンパイルで gcc -O0 に迫る性能。loop ベンチマークではループ自体が最適化で消える。

JIT の予備評価 (PPL2026) は [idea_jit.md](./idea_jit.md) §7 を参照。

## 4. 他のサンプル

| sample | 概要 |
|---|---|
| `calc` | 6 ノードの最小チュートリアル |
| `abruby` | CRuby C 拡張版 Ruby サブセット (VALUE / Prism / GC を流用) |
| `koruby` | スタンドアロン Ruby、**optcarrot 完走** |
| `luastro` / `jstro` / `pystro` | Lua 5.4 / JS ES2023 / Python 3 サブセット |
| `ascheme` / `astocaml` / `asom` | R5RS Scheme / OCaml サブセット / SOM Smalltalk |
| `pascalast` / `castro` | Pascal / C サブセット (静的型) |
| `aforth` / `wastro` | Forth / WebAssembly 1.0 (スタックマシン) |
| `astr` | R サブセット (vectorized) |
| `astrogre` / `nuq` | DSL: 正規表現エンジン / `jq` クローン |

横断分析と性能ハイライトは [`samples.md`](./samples.md) §1 の言語ラインナップ表を参照。
