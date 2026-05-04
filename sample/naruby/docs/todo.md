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

二段階で解決:

**Step 1 — PG (HORG-only)**: 「sp_body は parse 時にリンク + 構造ハッシュ
から除外 + SD で direct call ベイク」の三段構え。

- `naruby_gen.rb` で `NODE * sp_body` の `hash_call` を 0 に上書き →
  HASH が sp_body の中身に依らず安定。再帰関数によるハッシュサイクル
  も自動回避。
- `naruby_parse.c` の callsites 機構を pg_mode にも拡張。`def f` を
  parse 終わった時点で `callsite_resolve` が node_call2 の sp_body を
  本体 NODE に書き戻す。前方参照・自己再帰どちらも OK。
- `naruby_gen.rb` で sp_body の `build_specializer` を上書き。SD は
  `SD_<HORG(sp_body)>` を C 上の定数として埋め込んだ direct call を
  発行する。再定義時は slowpath が default dispatcher に降格 + sp_body
  書き換えで正しさを保つ。

**Step 2 — HOPT/PGSD chain (2026-05-04)**: 非再帰 call chain を LTO で
1 関数に折り畳むため、ASTro の HOPT 機構を有効化。

- `hash_node()` (HORG, sp_body 除外) と `hash_node_opt()` (HOPT, sp_body
  込み + cycle break) の二段ハッシュ。
- bodies は `SD_<HORG>` (AOT) と `PGSD_<HOPT>` (PGC) の両形を bake、
  `code_store/hopt_index.txt` で `(HORG, file, line) → HOPT` を永続化。
- `node_def` 実行時 + top-level で `astro_cs_load(n, name)` を呼び、
  PGSD chain への wire-up が成立。

実測 (詳細 [perf.md](perf.md) の 15 ベンチ × 13 列マトリクス):

| bench | n/plain | n/aot-c | n/pg-c | n/lto-c | gcc-O3 |
|---|---:|---:|---:|---:|---:|
| fib(40)        | 5.36 | 0.99 | 0.57 | 0.56 | 0.13 |
| call (10 段)   | 9.70 | 2.34 | 1.19 | 0.58 | 0.33 |
| chain40        | 8.89 | 3.91 | 3.01 | 0.92 | 0.25 |
| gcd            | 3.71 | 0.18 | 0.17 | 0.16 | 0.15 |
| collatz        | 3.67 | 0.16 | 0.15 | 0.15 | 0.14 |
| early_return   | 4.25 | 0.28 | 0.29 | 0.28 | 0.28 |
| prime_count    | 6.95 | 0.45 | 0.45 | 0.44 | 0.43 |

全 14 ベンチ (loop は DCE 除外) で **yjit を上回り (1.0-14×)**、
**実ワークロード系 (gcd / collatz / early_return / prime_count / compose)
で gcc -O3 と並ぶ (≤ 1.1×)**。redefinition も cold/warm 期待通り。

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

### ~~ベンチマークの sustained スケール~~ ✅ 整備済 (2026-05-04)

15 ベンチに整理 (再帰 4 / loop 1 / chain pass 3 / chain 算術 2 / chain 合成 1 /
chain 定数 1 / 投機分岐 1 / 制御フロー 3)。各題材は naruby/plain で
sustained 1 秒以上、シンセティック chain 系は `__attribute__((noinline,noipa))`
+ accumulate で gcc 側の DCE / CSE を防止。詳細 [perf.md](perf.md)。

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
| perf.md | ✅ |
| README.md | △ — 旧モデル前提の記述が残っている。次回更新時に英訳 + 新フローに書き換え |
