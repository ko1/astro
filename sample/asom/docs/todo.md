# asom TODO

実装済み機能は [done.md](done.md) を参照。ランタイム構造は [runtime.md](runtime.md) を参照。

## 目次

- [TestSuite で残っている失敗](#testsuite-で残っている失敗)
- [未実装の SOM 機能](#未実装の-som-機能)
- [性能改善](#性能改善) — 型特化 / 既存ノードの IC / Double / GC
- [AreWeFastYet 残り](#arewefastyet-残り)
- [ASTro 連携の深化](#astro-連携の深化)
- [メンテ・整備](#メンテ整備)

## TestSuite で残っている失敗

なし — TestSuite 24 ファイル / 221 アサーション全部 pass（IntegerTest
25/25 含む）。Bignum は GMP-backed LargeInteger で実装済み（[done.md
GMP-backed LargeInteger](done.md#gmp-backed-largeintegerbignum) 参照）。

## 未実装の SOM 機能

### `String` 関連

- [ ] `String>>charAt:` 戻り値が String (1 文字) になっている。SOM 標準では Integer (UCS code) を返すべき
- [ ] `String>>indexOf:` の戻り値（現在実装はあるが SOM 標準と微妙にズレ）
- [ ] エスケープシーケンス: `\xNN` 16 進、`\u{...}` Unicode は未対応
- [ ] String hash の SOM++ / PySOM 互換性検証

### Class side / リフレクション

- [ ] `Class>>methods` が ordered insertion list 順を返すが、SOM 標準は documented でない（ぐらいに振る舞っているかは未確認）
- [ ] `Class>>methods` with `signature beginsWith: #'class '` の class-side methods を返す API（abruby が実装している類似のもの）
- [ ] `Method>>invokeOn:with:` (現状 `perform:` 経由)
- [ ] `System>>printStackTrace`（現在 stub）

### Block / 制御フロー

- [ ] Block の `numArgs`, `numLocals`（reflective）
- [ ] `Block>>onException:do:`（exception handling 自体未実装）

### Vector 等の Smalltalk クラス

- 起動時 overlay merge で `Vector`, `Set`, `Dictionary` 等は Smalltalk-side で動作するが、bootstrap class として primitive を持っていないので AOT で速くならない
- 必要なら C で書き直し (`asom_vector.c` 新設)

## 性能改善

### Truffle warm peak とのギャップを埋める優先 4 項 (perf.md cost-model 参照)

[perf.md の Sieve cost-model 解析](perf.md#truffle-warm-peak-との差を埋める道筋--sieve-cost-model)
で示したとおり、現在 asom-aot は per-iter ~13 命令 / Truffle は ~3 命令。
4 つの最適化で **interp 比 10×、Truffle 比 2×** まで詰まる見込み:

- [ ] **(1) inline-frame locals の C local 化 (1.5-2× 想定)** — `whiletrue_pool`
      / `to_do_pool` / `iftrue_pool` / `and_pool` の inline body について、
      escape しない判定された branch では SD shard 関数の C ローカル変数として
      宣言、`frame->locals[slot]` 経由を消す。castro の VLA frame と同じ知見。
- [ ] **(2) PG-driven shape guard hoisting (1.3-1.4×)** — PG warmup で型安定
      が観測された node について、SD shard を `entry shape guard + 内部
      guard なし hot path + miss で generalized SD に切替` 構造で emit。
      `hopt index` スタブ ([§PG profile-aware ハッシュ](#pg-profile-aware-ハッシュ-hopt))
      に型安定性 metadata を載せて使う。
- [ ] **(3) 範囲解析 → bounds check elimination (1.1×)** — AST PE 時に
      range propagation pass を入れ、`whileTrue:` の cond と array index の
      関係から bounds 消去。
- [ ] **(4) 範囲解析 → overflow check elimination (1.05×)** — 上の延長で
      `__builtin_*_overflow` を pure 演算に。

(1)(2) が大半を稼ぐので先、(3)(4) は range analysis pass が要るので後。

### 型特化ノード — AOT を効かせる主要施策 ✓ (Integer + Array)

**実装済み (2026-04-29)**:

```
node_send1_intplus / intminus / inttimes                ← Integer +,-,*
node_send1_intlt / intgt / intle / intge / inteq        ← Integer <,>,<=,>=,=
node_send1_arrayat / node_send2_arrayatput              ← Array at:, at:put:
```

詳細は [done.md#型特化ノード](done.md#型特化ノードinteger-演算-array-アクセス) 参照。
パース時のセレクタマッチで特化版を発行 → 型 guard miss で
`swap_dispatcher` 経由で汎用 send1/send2 に戻る。

**残タスク**:

- [ ] `node_double_plus / ...` Double 版（unboxed double が前提なので Flonum 化と同時）
- [ ] `node_string_concat`（`+` of String）— 利用頻度低め
- [x] **`node_array_at / at_put`** — 実装済 (2026-04-29)
- [x] **制御フロー・インライン化** — `ifTrue: / ifFalse: / ifTrue:ifFalse: /
      whileTrue: / whileFalse:` の専用ノード。パース時、0p/0l/no-nested-block
      で発火。詳細は [done.md](done.md#制御フローインライン化)
- [x] **`to:do:` / `to:by:do:` / `timesRepeat:` の inline 化** — 1 / 3 / 0 引数
      ブロックそれぞれ stack 版 + heap pool 版を発行（escape 可能性に応じて
      自動選択）。詳細は [done.md](done.md#todo-インライン化)
- [ ] Polymorphic Inline Cache（`asom_callcache` を 1-element → 2..4 element）
- [ ] Block の 0p/0l 制限の緩和: 0-param N-local も alloca で OK にできる
- [ ] nested block を含む本体の inline 化（escape 解析が要る）

### Inline Method Cache の 1-element → polymorphic

- [ ] miss 時に PIC エントリを足す（serial が同じで複数 class まで OK な版）

### Double の unboxing — Flonum tagging 済み、shape unbox が次

**実装済み (2026-04)**: 2-bit タグ拡張 + biased-exponent 圧縮で
**Double を VALUE word 内に即値で持つ** Flonum tagging。詳細は
[done.md Flonum tagging](done.md#flonum-taggingdouble-を-value-即値化) と
[perf.md §5 / §10](perf.md#5--flonum-taggingcommit-f0bca38)。Mandelbrot
の中間 Double 即捨てパターンで効くが、mantissa 低 2bit ≠ 00 の値は
heap-fallback。

**残タスク**:

- [ ] **Shape-based field unbox** — Ball / Tree の field レベル Double が
      boxed のまま。`make bench-aot` 単独で Bounce / TreeSort で Truffle
      に負けがちな主因（PG warmup で `node_send1_dbl*` 特化が入ると逆転）
- [ ] AOT-stage の Double リテラル特化 — 片 operand に double リテラル
      がある送信を parser-time に `node_send1_dbl*` で発行
- [x] **`node_double_plus / ...` Double 版** — 実装済（[done.md
      `node_send1_dbl*` 型特化](done.md#node_send1_dbl-型特化abruby-ミラー)）

### GC — Boehm 導入済み

**実装済み (2026-04)**: Boehm-Demers-Weiser conservative GC を最小実装で
リンク。`malloc` / `calloc` / `realloc` / `strdup` / `free` を `context.h`
末尾でマクロ wrap。`__thread` 修飾子を global pool 系から削除して TLS
scan 問題を回避。詳細は [done.md Boehm-Demers-Weiser conservative GC](done.md#boehm-demers-weiser-conservative-gc) と
[perf.md §8](perf.md#8--boehm-gc-commit-ebc7201)。bench は inner ~1 秒で
major collection 走らずほぼ変わらず（Truffle / SOM++ と同条件に）。

**残タスク**:

- [ ] long-running TestHarness を全部回し続けたときの leak / RSS 計測
      ハーネス（テストはリストに入っているが未実装）

### AOT subseq の selector lookup ピル

SD コードが bare 文字列リテラル (`"whileTrue:"` 等) を `asom_send` に渡し、
intern 経由でないため `asom_class_lookup` の hash probe が miss → strcmp
linear fallback。

候補:
- ASTroGen `node_specialize.c` を asom 専用 subclass で override し、selector 引数を `asom_intern_cstr("whileTrue:")` 越しに emit する
- もしくは selector を NodeKind に紐づけて compile-time constant pointer を埋め込む

## AreWeFastYet 残り

未試行:

- [ ] **Havlak** — 制御フローグラフ生成、ループ階層分析
- [ ] **CD** — 衝突検出 (Aircraft Collision Detection)
- [ ] **Knapsack** — 0/1 knapsack DP
- [ ] **PageRank** — 反復行列計算

これらは全て `SOM/Examples/Benchmarks/<Name>/` 以下にあり。動かす際にエラーが出ても
既存の primitive で大体サポートできる範囲。

## ASTro 連携の深化

### `--compiled-only` の振る舞い

abruby は `--compiled-only` で interpreter dispatcher が使われたときに
abort する (= 全エントリが SD 化されているか確認するモード)。asom は
フラグを定義したが未実装。

- [ ] ALLOC 時に dispatcher を NULL に置き、cs_load miss なら abort

### PG profile-aware ハッシュ (Hopt)

- [ ] `HOPT(n)` を実装し、profile counters を含むハッシュにする
- [ ] PGSD_<Hopt> 別 SD コード生成（現状 Hopt == Horg なので AOT と同じ）
- [ ] hopt_index.txt 経由の lookup 動作確認

### JIT 連携 (`naruby/astro_jit` 流)

- [ ] L0 thread でバックグラウンドコンパイル
- [ ] 100 dispatch 超で submit、完了後にディスパッチャ swap
- [ ] L1/L2 リモートコンパイル farm

これは ASTro JIT デモとしての本来の使い方。

## メンテ・整備

- [ ] パーサのエラーメッセージに行番号 + 列 + ソース引用
- [ ] スタックトレースに source location（`NodeHead.line` を有効化）
- [ ] `make` を sandbox 環境でも build できるよう `CCACHE_DISABLE` を Makefile で自動設定
- [ ] リーク量計測ハーネス、long-running TestSuite で OOM しないか
- [ ] CI: github actions で `make test`, `make testsuite`, `make bench` を毎 push
- [ ] `--dump-ast` (現状 OPTION 定義のみで未実装)
- [ ] `Symbol VALUE` インターン pool の hash table を grow 対応に
- [ ] パーサの一部マジックナンバー (`ASOM_MAX_LOCALS_PER_SCOPE = 64` 等) を可変に
