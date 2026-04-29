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

### IntegerTest 5/25 失敗 — 全部 Bignum (> 2⁶²) 系

| テスト | 内容 | 必要なもの |
|--------|------|-----------|
| testClassAndValueRanges | `1180591620717411303424` の round-trip | Bignum |
| testIntegerLiterals | `9223372036854775807` 等 64-bit 境界 | tagged を 64bit ↔ Bignum 自動切替 |
| testFromString | bignum 文字列パース | Bignum + `Integer fromString:` |
| testMin / testMax | bignum vs fixnum 比較 | Bignum 比較 |
| testAbsoluteValue | `-9223372036854775296 abs` | Bignum |

実装方針候補:
1. **GMP リンクして mpz_t をヒープオブジェクトに wrap**（abruby と同じ）
2. **62-bit overflow 検知 → mpz_t に昇格**（fast path 残しつつ）
3. **オーバーフローを諦めて 32-bit 整数演算でラップ**（簡単だが SOM 仕様違反）

abruby/ascheme は GMP を使っているので 1. が自然。`asom_bignum.c` を分けて、
`int_plus` 等で `__builtin_add_overflow` で検知し、bignum に昇格する形が
無難。

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

### 型特化ノード — AOT を効かせる主要施策

**現状**: AOT/PG コンパイルが ~3% しか効かない。理由は `asom_send` ホットパス
が primitive 関数ポインタ経由の間接呼出しで止まり、AST traversal が SD で
畳まれても残コストが律速。

**施策**: abruby 流に **型特化ノード** を導入する。

```
node_send1 (Integer + Integer)
  ↓ swap_dispatcher (実行時 type observation)
node_fixnum_plus(left, right) {
  if (LIKELY(ASOM_IS_INT(l) && ASOM_IS_INT(r))) {
    return ASOM_INT2VAL(ASOM_VAL2INT(l) + ASOM_VAL2INT(r));
  }
  // フォールバック: node_send1 セマンティクスに戻る
}
```

これにより:

1. SD 化したとき `node_fixnum_plus` の EVAL は完全インライン (関数ポインタ間接
   呼出し無し)
2. type guard が外れたら `swap_dispatcher` で general node に戻し、PIC 化
3. abruby 同等の AOT 1.5–2× 速度向上が期待できる

実装手順:

- [ ] `node_fixnum_plus / minus / times / lt / gt / le / ge / eq` 追加
- [ ] `node_double_plus / ...` 追加 (unboxed double を持つ場合)
- [ ] `node_string_concat`（`+` of String）
- [ ] `node_array_at / at_put`
- [ ] 実行時 type-feedback で general → 特化 にスワップ (`swap_dispatcher`)
- [ ] 失敗時に general に戻すレース安全な仕組み（abruby `swap_dispatcher` 参考）

### Inline Method Cache の 1-element → polymorphic

- [ ] miss 時に PIC エントリを足す（serial が同じで複数 class まで OK な版）

### Double の unboxing

**現状**: 全 Double は `asom_double_new` で `calloc` → ヒープオブジェクト。
Mandelbrot で SOM++ (unboxed) 比 1.9× 遅い主因。

候補:
- **NaN-tagging** (52-bit double + tag bits)
  → VALUE 表現を 64bit すべて使う。SmallInteger との同居が複雑
- **Float の immediate**（abruby Flonum スタイル: 60 bit double）
  → 微小な精度損失だが allocation 完全消滅
- **Double 専用ノード** (`node_double_plus` 等) で局所的にレジスタ持ち

abruby は Flonum 採用。asom も同じ路線で行くなら 32-bit 系含めて再設計が要る。

### GC

**現状**: `asom_invoke` がフレーム + locals[] を `calloc` するが free しない (リーク)。
ベンチは走り切るが TestHarness を全部回し続けるとメモリ膨張。

候補:
1. **Boehm GC** をリンク (transparent、依存追加)
2. **Naive mark-sweep** を自前実装 (root: c->frame chain, asom_global table, asom_intern pool, code_repo)
3. **Reference counting**（ループのある closure と相性悪い）

abruby は CRuby の GC を借用、ascheme は Boehm を使っている。asom は Boehm が
最短ルート。

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
