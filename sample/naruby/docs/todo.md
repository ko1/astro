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

### ~~Profile-Guided AOT が新コードストアで効いていない~~ ✅ 修正済

段階的に解決:

**Step 1 — PG (HORG-only)** (2026-05-02): 「sp_body は parse 時にリンク
+ 構造ハッシュから除外 + SD で direct call ベイク」の三段構え。

- `naruby_gen.rb` で `NODE * sp_body` の `hash_call` を 0 に上書き →
  HASH が sp_body の中身に依らず安定。再帰関数によるハッシュサイクル
  も自動回避。
- `naruby_parse.c` の callsites 機構を pg_mode にも拡張。`def f` を
  parse 終わった時点で `callsite_resolve` が node_pg_call_<N> /
  node_call2 の sp_body / locals_cnt を本体 NODE に書き戻す。
  前方参照・自己再帰どちらも OK。
- `naruby_gen.rb` で sp_body の `build_specializer` を上書き。SD は
  `SD_<HORG(sp_body)>` を C 上の定数として埋め込んだ direct call を
  発行する。

**Step 2 — HOPT/PGSD chain** (2026-05-04): 非再帰 call chain を LTO で
1 関数に折り畳むため、ASTro の HOPT 機構を有効化。

- `hash_node()` (HORG, sp_body 除外) と `hash_node_opt()` (HOPT, sp_body
  込み + cycle break) の二段ハッシュ。
- bodies は `SD_<HORG>` (AOT) と `PGSD_<HOPT>` (PGC) の両形を bake、
  `code_store/hopt_index.txt` で `(HORG, file, line) → HOPT` を永続化。
- `node_def` 実行時 + top-level で `astro_cs_load(n, name)` を呼び、
  PGSD chain への wire-up が成立。

**Step 3 — フェアネス整理** (2026-05-04):

- AOT/interp も `node_call_<N>` で arity-N specialize。引数 eval +
  fresh frame `F[locals_cnt]` を PG と共通化し、AOT vs PG の差は
  「`cc->body` indirect vs `sp_body` baked-direct」の 1 点だけにする。
- `main.c` を `bake → reload → EVAL` 順に変更。cold 1st run でも SD
  経路で実行。`*-1st` セルが「compile + dlopen + run」を 1 回で測れる。
- C 比較ベンチ (`bench/*.c`) を `int64_t` に統一。値型幅を naruby
  (`VALUE = int64_t`) と揃える。

**Step 4 — 2 段ガード** (2026-05-04): `node_pg_call_<N>` /
`node_call2` の fast path に **`cc->body == sp_body` チェックを追加**。

- 旧 (serial 1 段) は parse 時 speculation と実行時 first body が
  異なる場合 (`def → call → def → call` 等) に baked-direct call が
  間違った body の SD を呼ぶ可能性があった。
- 新 (serial + body 一致) は 2 段で baked-direct を gate。slowpath は
  `cc->body` 経由 indirect で安全側に倒し、sp_body は parse 時値で
  固定 (= SD の baked 定数と常に整合)。
- Demote (dispatcher 降格) は廃止。fast path 自体が動的に正しさを
  保つので、SD は再定義後も残してよい。

**Step 5 — cold path の本体 binary 集約** (2026-05-04): 各 call variant
専用の slowpath (`node_call_0/1/2/3_slowpath`, `node_pg_call0/1/2/3_slowpath`,
`node_call_slowpath`, `node_call2_slowpath`) を `node_slowpath.c` に
すべて集約。シグネチャを `(c, n, fp)` に統一し、name/cc/sp_body/args は
NODE union から取得 + caller の fp で再評価。

- SD 側 (= fast path) は failure 時に何も spill しない。`jne` で
  単純な thunk へ tail-call。
- cold path 命令が SD から消えて I-cache 圧迫が減る → recursive
  bench で 15-20% 高速化 (fib lto-c 0.71→0.59、ackermann 0.85→0.79)。
- chain bench は inline 段ぶん 2 段ガードコストが残るので大きな
  改善はないが、SD 自体は引数 marshaling コードが削れた分軽量化。

**Step 6 — option を直交直 + 本物の PGO に再設計** (2026-05-04):
abruby に揃えて `-c` (AOT bake) と `-p` (PG bake) を独立フラグ化。
`-p` は **post-EVAL** に PGSDs を bake — `cc->body` (= 実際に観測した
body) を sp_body に書き戻してから出力するので、これで初めて runtime
profile に基づいた PGO になる (旧設計は parse-time `code_repo`
last-def-wins を sp_body に焼くだけで PGO ではなかった)。

- profile.[ch] 削除 (永続化は all.so + hopt_index.txt で十分)。
- naruby_parse.c に `all_pg_call_nodes` リスト + 専用 helper を追加、
  build_pgsd の AST walk に使う。
- node.c の `clear_hash` を has_hash_value / has_hash_opt 両方 invalidate
  に拡張。
- `node_call_<N>` と `node_pg_call_<N>` の使い分けを parser で実装:
  `-p` で pg_call、なしで call (sp_body なし、軽量)。
- bench は `--ccs -c` (aot-1st)、`-b` (aot-c)、`--ccs -c -p` (pg-1st)、
  `-p -b` (pg-c) の 4 列体系に。

**残課題 (Step 6 から繰り越し)**:
- 2 invocation 目で `node_call_<N>` → `node_pg_call_<N>` に自動 swap
  する仕組み (今は parser が `-p` フラグを見て kind を選ぶので、cached
  PG run は明示的に `-p -b` が必要)。`(Horg, location) → Hopt` の比較表
  を作って HOPT が違っても見つけられるようにする、というのが筋。
- LTO bake 後の cached run でたまに `-flto` 環境差由来らしき不安定値
  (ackermann lto-c で 0.92 vs 6.23 のばらつき)。再現条件を詰めたい。

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
