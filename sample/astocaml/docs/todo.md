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

(2026-05 検討時の優先度メモ。詳細な見積りは「## 検討した候補」を参照)

**短期で効きそう**:

- **B2. closure capture の call IC** — filter / map のような `p h` (lref 由来 closure) の call site でも IC キャッシュ。 sieve / nqueens で +30% 期待。 既存 `app_cache` infra の receiver-side 拡張で済むのでリスク低
- **B1. 連続 match_arm 融合** — `[] | h::t` のチェーンを 1 ノード化して per-arm dispatch を省略
- **A3. 型特化 binop の融合** (バンバン路線) — `add_then_sub_int(a,b,c)` 等、安全な算術連鎖だけ。`if + cmp_int` の失敗を踏まえて UI 側に dispatcher swap が effects free なものに限定
- **C1. HOPT (profile-derived hash) の実装** — PGO の自然な発展、`runtime/astro_code_store.c` 拡張で他 sample にも波及

**中規模**:

- **A1. Direct call 再挑戦 (batch AOT 化)** — streaming AOT model を two-pass parse → 一括 AOT に変えれば mutual recursion 含めて成立するはず。fib +20% / ack +15% 期待
- **D1. variant / record の type registry** — `type t = ...` 宣言を tracking。型エラー検出 + match_arm spec の足場
- **D2. 型特化 match_arm** — D1 連動、既知バリアントなら test を bare cmp に
- **A2. closure body inlining at call site** — 再帰関数の body を bounded depth で展開。SD 爆発リスクあり、PGO データで hot のみに限定推奨

**framework 機構の活用**:

- **C2. JIT (background compile)** — 起動 → interp で hot 検出 → BG thread で SD compile → atomic dispatcher swap。起動遅延ゼロ + 定常 AOT 並み
- **C3. cross-process code store** — Merkle hash で別 process と SD 共有

**マイクロ**:

- **算術の overflow check** (現状 wrap-around; `__builtin_*_overflow` で fixnum range 維持)
- **String SSO** (small string optimization)
- **frame の reuse** (TCO 中の frame をスタックライクに使い回す)
- **string copy elimination** (`^` で必ず alloc + copy)
- **環境チェーンキャッシュ** (深い `lref(d, i)` のための parent-walk キャッシュ — 現状の bench は影響軽微なので優先度低)
- **クラス間 method table 共有** — `__class_build` で per-class table 化、method send IC が cross-instance で効く
- **primitive call spec** — `print_int` などで型 check skip

**範囲外 (要相談)**:

- **F1. tagged float / unboxed VALUE** — 値表現変更
- **F2. 真の codegen backend** — asm 直接出力で C compiler を排除

## 試したが諦めた最適化 (詳細は `perf.md`)

- **Tiny closure + `c->reg` で frame skip** — 共有 reg の save/restore とキャッシュ pressure で逆に regress、撤去
- **Unbox channel (`_iu` 系)** — 値表現の意味論変更が必要、要相談で別 session
- **`node_appN_direct` (call 先 body の焼き込み)** — fib 単体 +20% だが mutual recursion で AOT 整合崩壊。streaming AOT model のままでは解けず
- **`if + cmp_int` 融合 (`node_if_lt_int` 等)** — 機械語は綺麗 (50→28 命令) だが runtime regress、原因不明

## 検討した候補 (2026-05 整理、未着手)

各案について期待効果と実装規模をメモ。優先順は上の「## 性能向上のための今後の課題」を参照。

### A. 関数呼び出しコスト削減 (fib/tak/ack 効く)

| 案 | 期待効果 | リスク・規模 |
|--|--|--|
| A1. Direct call 再挑戦 | fib +20%, ack +15% | streaming AOT → batch model 化 (中) |
| A2. closure body inlining at call site | fib +10-20% | SD codegen + bounded depth (中) |
| A3. tagged int 演算チェーンの融合 | fib +5-10% | 専用ノード追加 (小、安全) |
| A4. ABI (dispatcher signature) 変更 | fib +10%? | framework 大改造 (大) |

### B. List 処理 (nqueens/sieve 効く)

| 案 | 期待効果 | リスク・規模 |
|--|--|--|
| B1. 連続 match_arm 融合 | nqueens/sieve +20% | match_arm 構造拡張 (小) |
| B2. closure capture call IC | sieve +30% | 既存 IC infra 拡張 (小) |
| B3. cons の GC_typed_malloc | sieve +5-10% | Boehm typed alloc 学習 (小) |
| B4. 短いリストを inline array で表現 | nqueens/sieve +10-20% | 値表現変更 (要相談) |

### C. ASTro framework 機構

| 案 | 期待効果 | リスク・規模 |
|--|--|--|
| C1. HOPT 実装 | 全般 +5-10% | runtime/astro_code_store.c 改修 (中) |
| C2. JIT (background compile) | 起動遅延ゼロ + 定常 AOT 並 | スレッド + atomic dispatcher swap (大) |
| C3. cross-process code store | コンパイル時間ゼロ化 | dlopen/atomic 設計 (中) |

### D. 型推論の更なる活用

| 案 | 期待効果 | リスク・規模 |
|--|--|--|
| D1. variant/record type registry | 型エラー検出 + match spec の足場 | 中 |
| D2. 型特化 match_arm | nqueens/sieve +5% | D1 連動 (小) |
| D3. 関数 return 型を caller が利用 | 軽微 | 全 call site 改修 (小) |

## ベンチマーク

`./astocaml -c bench/<x>.ml` (warm cache、min of 5、PGO 適用後):

| ベンチ | 結果 | 時間 |
|--|--|--|
| ack    | 4093          | 0.081 s |
| fib    | 9227465       | 0.113 s |
| nqueens| 724 (×3)      | 0.253 s |
| sieve  | 3738566 (×8)  | 0.279 s |
| tak    | 9 (×5)        | 0.054 s |

公式 OCaml 4.14.1 との対戦:

| bench | astoc(-c) | ocaml(top) | ocamlc(BC) | ocamlopt |
|--|--|--|--|--|
| ack | **0.08** | 0.15 | 0.11 | 0.010 |
| fib | **0.11** | 0.34 | 0.23 | 0.039 |
| tak | **0.05** | 0.26 | 0.15 | 0.015 |
| nqueens | 0.25 | 0.23 | 0.17 | 0.018 |
| sieve | 0.28 | 0.26 | 0.24 | 0.025 |

ack / fib / tak で **astocaml AOT が ocamlc bytecode を超え** ocamlopt の **2.9× (fib) / 3.6× (tak) / 7× (ack)** まで詰まった。 nqueens / sieve は list 処理が重く ocamlc の 1.2-1.5× 遅。

ベースライン (Phase 1 完了時) からの累積:
- fib: 2.62 s → 0.113 s (**23×**)
- tak: 1.23 s → 0.054 s (**23×**)
- ack: 1.20 s → 0.081 s (15×)
- nqueens: 1.59 s → 0.253 s (6.3×)
- sieve: 1.21 s → 0.279 s (4.3×)
