# 残課題

Phase 1+2+3 の主要機能と性能改善 (gref キャッシュ・TCO・比較ファストパス・AOT) は実装済み。残りは細部と大型機能。

## 言語仕様 — 細かい未実装

### 型システム — 残り

- **バリアント / レコードの型** — `type t = ...` 宣言を type registry に登録し、コンストラクタ/フィールドアクセス時に型を引く (現状 `TY_ANY`)
- **オブジェクト型 (row polymorphism)** — `obj#m` を構造的サブタイプで
- **エラー位置の report** — 現状 nodes に位置情報がない; `n->head.line` を活用するためにパーサで設定
- **Recursive types** (`type 'a tree = Leaf | Node of 'a * 'a tree * 'a tree`) — occurs check をすり抜けるための明示的な箱付け

### 構文・小機能

- **`~-` 単項マイナスを関数値として** (`(~-) x` で `-x` 相当)
- **First-class module** (`(module M : S)`, `let module M = (val pack)`)
- **GADT** (`type _ t = ...`) — 型システム拡張のみ
- **`module rec` (相互再帰モジュール)**
- **First-class signatures** / **Module type expressions**
- **Effect handlers** (OCaml 5.0+) — try/with の拡張版

### オブジェクト系

- **Inherited fields の bare access** (subclass method 内で `inherit X` 経由のフィールド名は `self#field` 必須; 親クラスの field 集合をパース時に知るには parent class registry が要る)
- **virtual method**, **virtual class**
- **multiple inheritance** (`inherit A; inherit B`)
- **オブジェクト subtyping**, **row polymorphism** (型システム)

### Stdlib 充実

- **Map.Make / Set.Make** (functor は動くようになった; あとは標準 Map/Set のロジックを書くだけ)
- **Format** モジュール (Printf より高機能な pretty-printer)
- **Scanf**
- **Bigarray, Marshal, Sys, Unix** などの I/O / システム呼び出し
- **`open M.( ... )` 局所スコープ open expression** (M.foo. ( ... ) 構文)

### その他

- **ppx 拡張** (compile-time macro) — OCaml 本体のスコープ外
- **Mutable record fields** with `.f <- v` (現状 `node_field_assign` で動作するが完全網羅は未確認)
- **String 旧 API** (一部 deprecated だが互換性のため `String.set` 等が呼ばれることはある)

## 性能向上のための今後の課題

### インタプリタ・パスの最適化

- **環境チェーンキャッシュ** (深い `lref(d, i)` のための parent-walk キャッシュ; ascheme 風) — 現状未着手
- **クラス間 method table 共有** — `__class_build` の段階で (names, closures) を per-class にすることで method send IC が同一クラスの全インスタンスで効くようにする
- **PGC (Profile-Guided Compilation)** — dispatch_cnt を集めて hot ノードを Hopt 経由で再 specialize
- **JIT (background compile)** — AST 走査中に hot ノードを後段スレッドで compile
- **tagged float (Ruby 流の inline flonum encoding)** — boxed double の alloc を削減 (※ 値表現の変更は要相談)
- **node_app の specialization** (call site が多数ヒットする closure を直接 dispatch)

### マイクロ最適化

- **算術の overflow check** (現状は wrap-around; `__builtin_*_overflow` で fixnum range 維持)
- **Cons cell の小型化** (現状 union-based oobj は ~40 byte; 専用構造体に分けて cache pressure 低減)
- **String SSO** (small string optimization)
- **`make_app` で fn が closure-literal の場合は直接 frame 構築** (closure value 取り出しスキップ)

### メモリ管理

<!-- Boehm GC: 統合済 (perf.md / done.md 参照) -->
- **frame の reuse** (TCO 中の frame をスタックライクに使い回す — 既に同 size なら更新だけ)
- **string copy elimination** (`^` で必ず alloc + copy。短いケースで stack-allocated buffer)

### ASTro 機構の活用

- **`OPTIMIZE` フックの本格化** (現状 `astro_cs_load` を呼ぶだけ; hot 検出 → JIT も)
- **`HOPT` の実装** (profile-derived hash; 現状 `HORG` と同じ)
- **コードストアのクロスプロセス共有** (Merkle hash ベース)

## 試したが諦めた最適化 (詳細は `perf.md`)

- **Tiny closure + `c->reg` で frame skip** — 共有 reg の save/restore とキャッシュ pressure で逆に regress、撤去
- **Unbox channel (`_iu` 系)** — 値表現の意味論変更が必要、要相談で別 session
- **`node_appN_direct` (call 先 body の焼き込み)** — fib 単体 +20% だが mutual recursion で AOT 整合崩壊。streaming AOT model のままでは解けず
- **`if + cmp_int` 融合 (`node_if_lt_int` 等)** — 機械語は綺麗 (50→28 命令) だが runtime regress、原因不明

## ベンチマーク

`./astocaml -c bench/<x>.ml` (warm cache、min of 5):

| ベンチ | 結果 | 時間 |
|--|--|--|
| ack    | 4093          | 0.092 s |
| fib    | 9227465       | 0.149 s |
| nqueens| 724 (×3)      | 0.365 s |
| sieve  | 3738566 (×8)  | 0.372 s |
| tak    | 9 (×5)        | 0.066 s |

公式 OCaml 4.14.1 との対戦:

| bench | astoc(-c) | ocaml(top) | ocamlc(BC) | ocamlopt |
|--|--|--|--|--|
| ack | **0.10** | 0.19 | 0.12 | 0.012 |
| fib | **0.15** | 0.43 | 0.24 | 0.044 |
| tak | **0.07** | 0.27 | 0.19 | 0.016 |
| nqueens | 0.40 | 0.25 | 0.18 | 0.020 |
| sieve | 0.40 | 0.31 | 0.24 | 0.026 |

ack / fib / tak で **astocaml AOT が ocamlc bytecode を超え** ocamlopt の 3.5-7× まで詰まった。 nqueens / sieve は list 処理が重く ocamlc の 1.7-2× 遅。

将来計画:

- 環境チェーンキャッシュで深い lref を 1 hop に
- クラス間 method table 共有で method send IC を cross-instance でも効かせる
- 真の PGC (`HOPT` ベースの hot-only specialize) 導入
<!-- Boehm GC: 統合済 (perf.md / done.md 参照) -->
- OCaml 公式 `ocamlc -O0` との比較ベンチを追加
