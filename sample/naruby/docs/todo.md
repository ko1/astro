# naruby 未対応 / 既知の制限

実装済み機能は [done.md](done.md)、言語仕様は [spec.md](spec.md)、
ランタイム構造は [runtime.md](runtime.md) を参照。

## 言語

naruby は論文評価のための最小サンプルとして作られたので「Ruby サブセット」
としてはかなり狭い。直近で価値が出そうな順に並べる。

### 値型

| 項目 | 状態 | 必要な作業 |
|---|---|---|
| `nil` | ❌ | `node_nil` 追加。VALUE は int64_t のままなので `(VALUE)0` を割り当てれば衝突しない |
| `true` / `false` | ❌ | 1 / 0 を返す `node_true` / `node_false` で済む。Truth-y 判定は既に「0 でなければ真」 |
| 浮動小数 | ❌ | VALUE が int64 なので NaN-boxing か union 化が必要。**メモリで `feedback_no_nan_boxing` の方針** — 値表現変更が必要なら別案を先に相談すること |
| 文字列 | ❌ | ヒープオブジェクト + GC が要る。骨格としては大物 |
| Symbol / Array / Hash / Range / Regexp | ❌ | 同上 |

### 制御構造

| 項目 | 状態 | 備考 |
|---|---|---|
| `return` | ✅ | RESULT envelope (= VALUE + state bit) で実装済 (2026-05-02)。詳細は [runtime.md §3.1](runtime.md) |
| `break` / `next` / `redo` | ❌ | RESULT に state bit 追加で対応可能 (RESULT_BREAK / RESULT_CONTINUE)。while ノードでキャッチ |
| `case` / `when` | ❌ | parser のみで実装可能 (if/elsif の糖衣) |
| `unless` / `until` | ❌ | parser に PM_UNLESS/PM_UNTIL のケースを追加 |
| `for ... in` | ❌ | Range 必須 |

### 関数

| 項目 | 状態 | 備考 |
|---|---|---|
| デフォルト引数 | ❌ | `def f(a, b=1)` |
| キーワード引数 | ❌ | `def f(a:, b:)` |
| ブロック / yield | ❌ | naruby 全体で「ブロックなし」を前提に組まれているので影響範囲大 |
| `return value` | ❌ | 上記 |
| レシーバ付き呼び出し | ❌ | `obj.method` — Object 系が必要 |
| 単項マイナス | △ | リテラル `-3` は通るがプリフィックス演算子としては未テスト |

### OOP

class / module / 継承 / mixin / 定数 / メタクラス、すべて未対応。
論文評価対象として不要だったため意図的に切ってある。やるなら abruby を
参考にする (Class 構造体 + method 構造体 + ivar shape など)。

## 実装側 / 性能

### ~~Profile-Guided AOT が新コードストアで効いていない~~ ✅ 修正済 (2026-05-02)

修正アプローチ: 「sp_body は parse 時にリンク + 構造ハッシュから除外
+ SD では indirect dispatch」の三段構え。HOPT は使っていない。

- `naruby_gen.rb` で `NODE * sp_body` の `hash_call` を 0 に上書き →
  HASH が sp_body の中身に依らず安定。再帰関数 (fib_body の中の
  `fib(n-1)` の sp_body=fib_body) によるハッシュサイクルも自動回避。
- `naruby_parse.c` の callsites 機構を pg_mode にも拡張。`def f` を
  parse 終わった時点で `callsite_resolve` が node_call2 の sp_body を
  本体 NODE に書き戻す。前方参照・自己再帰どちらも OK。
- `naruby_gen.rb` で sp_body の `build_specializer` も上書き。SD は
  `SD_<HASH(sp_body)>` を C 上の定数として埋め込んだ direct call を
  発行する (cn で `extern RESULT SD_<hash>(...)` の forward decl を
  emit、SPECIALIZE 再帰はスキップ — body は別エントリとして bake)。
  これで `def f` 再定義 → slowpath が `sp_body` を新 body に書き換え
  + `n->head.dispatcher` を default に巻き戻して indirect 経路に降格
  → 次回呼び出しは interpreter 経由で正しい body を呼ぶ、が成立する。

実測 (fib(40), `bench/bench.rb`, 全ベンチ末尾 `p result` 統一後):
naruby/plain 6.95s → naruby/aot 1.21s → naruby/pg **0.91s**。gcc -O0
(0.57s) と -O1 (0.48s) の中間。**ループ + 算術系 (gcd / collatz /
early_return)** では gcc -O3 と並ぶ (詳細は [perf.md](perf.md))。
redefinition (`p(f(10)); def f redefine; p(f(10))`) は cold/warm
どちらも期待通り 11→20。

### JIT 統合の現状

`astro_jit.c` の自前 dlopen と `runtime/astro_code_store.c` の dlopen
は重複している。本当はどちらかに寄せたい。

- L1 (compile worker) は `astrojit/l1_so_store/all.so` を吐く
- runtime は `code_store/all.so` を吐く

両方が同時に「同じ SD_<hash>」を作りうる。今は astro_jit 側が先に当たれば
JIT、外れて runtime 側が当たれば AOT、という二段階探索。整理する余地がある。

### ノード詰め直し / -a モード

`-a` (`record_all`) は ALLOC のたびに code_repo に登録するが、
新コードストアでは「親エントリの SD_<hash>.c の中で再帰的に specialize
される」のでコードストアに余計なエントリが増えるだけで損益不明。
パフォーマンス確認のうえで挙動を整理したい。

### node_exec.c, naruby_code.c は dead

`node_exec.c` (1331 行、古い OPTIMIZE/SPECIALIZE 実装) と `naruby_code.c`
(10 行、ASTroGen 試作の名残) は現在どこからも include されていない。
削除候補だが破壊的なので残してある。

### ベンチマークの sustained スケール

`bench/fib.na.rb` 1 ファイルしか動く題材がなく、しかも fib(40) ≈ 5 秒。
他の `bench/*.na.rb` は parser や評価系の未対応 node にひっかかる
ものが多い。**sustained 1 秒以上を maintain しつつ広いプロファイルを
取りたい**ので、新しい bench 題材を増やす価値がある。

### Makefile の旧ターゲット

`make ctrain` / `make ptrain` は旧モデル (`node_specialized.c` を
再ビルド) を前提にしたフロー。新モデルでは「`./naruby -c file` を
1 度実行 → 次から `./naruby file` で warm load」だけで済むので、
これらのターゲットも整理した方がよい。

## ドキュメント

| 項目 | 状態 |
|---|---|
| spec.md | ✅ |
| done.md | ✅ |
| todo.md | ✅ (このファイル) |
| runtime.md | ✅ |
| perf.md | ✅ (実測値は要更新) |
| README.md | △ — 旧モデル前提の記述が残っている。次回更新時に英訳 + 新フローに書き換え |
