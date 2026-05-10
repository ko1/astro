# baruby 性能ノート

仕様は [spec.md](spec.md)、実装は [runtime.md](runtime.md)、
未対応・残タスクは [todo.md](todo.md) を参照。

baruby は GC testbed が主目的なので **絶対性能の最適化はまだ何もして
いない**。本ドキュメントは初期状態のベンチ結果と、観察された性能特性の
記録。チューニング指針は todo.md (P1 — パフォーマンス) を参照。

## 1. 計測環境

| 項目 | 値 |
|---|---|
| CPU | AMD Ryzen 9 5900HX |
| OS | Linux 6.8 (x86_64) |
| Compiler | gcc 13.3.0 |
| libgc | Boehm 8.2.6 (Ubuntu パッケージ `libgc1` / `libgc-dev`) |
| Build flags | `-O3 -ggdb3 -march=native -fno-plt` |
| baruby mode | `--plain` (= AST インタプリタ、code_store なし) |

`bench/run.rb -n 3` を 3 回回したときの best / median を記載
(memory note: bench は ~1s 持続スケールで取る)。

## 2. モード別実測値

`bench/run.rb -n 3` を実行、各 mode で 3 回繰り返した best 値:

| bench         | plain (s) | aot (s) | pg (s) | aot 比 | pg 比 |
|---|---:|---:|---:|---:|---:|
| binary_trees  | 0.96      | 0.64    | 0.94   | 1.51× | 1.03× |
| list_alloc    | 1.16      | 0.51    | 0.50   | 2.27× | 2.32× |
| string_concat | 1.02      | 0.88    | 0.88   | 1.16× | 1.16× |

- `alloc_MB` (plain): binary_trees 320.8 / list_alloc 763.8 / string_concat 1147.3
- `GCs` (plain): 12 / 1148 / 1706
  (`GC_get_total_bytes` / `GC_get_gc_no`)
- どれも plain で ~1s 持続。`alloc/sec` は 350MB/s〜1.1GB/s — 完全に
  allocator-bound と見なしてよいレンジ。

AOT は `-c` で AST 全体と各関数本体を SD\_\<hash\>.c に bake → all.so に
リンク → dlopen。PG は `-p` で 1 回プレーン実行のあと cc->body から
PGSD\_\<hopt\>.c を bake (= 観測した body との直接呼び出しが SD に
焼き込まれる)。PG が plain と大差ない bench (binary_trees) は 1 回
ループで終わる構造のため、prof-driven inlining の利得が小さい。

各ベンチの所感:

### binary_trees (depth 21)

- 4M 個 + 内部ノード ≈ 8M セルのバイナリツリーを構築 → check。
  各ノード = 2 要素 BaArray (`[left, right]` または `[0, 0]` の葉)。
- 1 BaArray ≈ 16B header + items 2 個 16B + alloc 余白 = ~40B。8M セル
  で ~320MB、12 GCs。GC time 自体は wall の数 % 程度 (libgc は
  thread-local mark にしかいかないので軽い)。
- ホットパスは `tree[0] == 0 && tree[1] == 0` の判定 + 2 回の `[]`
  アクセス。`node_call_aget` の type branch が毎回 array に倒れる →
  branch predictor は hit。

### list_alloc (10M iter)

- ループ内で 4 要素配列を 1 回 alloc → 即捨て。short-lived alloc の
  density が一番高いベンチ。
- alloc/sec ≈ 750MB/s = 18M alloc/s。1148 GCs は libgc が小ヒープを
  維持しようとして頻繁に sweep している印象。
- `[1, 2, 3, 4]` リテラルが parse 時に
  `ary_push(ary_push(ary_push(ary_push(ary_new, 1), 2), 3), 4)` に
  展開されるので、1 イテレーション = `1 ary_new` + `4 ary_push`。
  各 push の `realloc` が capa 倍々戦略で `0 → 4 → 8` と成長 →
  最終的に 4 要素を持つ capa=8 の配列を毎回新規確保する形。

### string_concat (5M iter)

- `"abc" + "def" + "ghi"` を 5M 回。各 `"abc"` などのリテラルは
  eval 毎に fresh alloc (intern なし)。
- 1 イテレーションで 3 リテラル + 2 concat = 5 BaString alloc + 3 つの
  payload 別 alloc = 8 alloc。~1.1GB / 5M iter = 220B/iter。
- 1706 GCs。short-lived な多数の小オブジェクト = 典型的な
  generational GC が効くワークロード (libgc は generational じゃない
  のでここでは relatively expensive)。

## 3. 既知のオーバーヘッド

- **タグ操作**: `INT2VAL` / `VAL2INT` が clarity 優先で `<<1` / `>>1`
  + or/mask の 2 命令ずつ。`-O3` で大半は畳まれるが、`node_add` の
  hot loop で見ると 1 cycle 単位の差が出るかも (要 perf record)。
- **eval 毎の string literal alloc**: ベンチ用途には feature だが、
  実用に降ろしたいなら todo.md 参照。
- **`node_call_*` の generic dispatch**: profile 化されていない →
  全 method op で type branch が runtime に残る。AOT specialization
  入れて `_ary` / `_str` variant に分けるのが筋 (todo.md P1)。

## 4. 比較対象 (CRuby)

CRuby との並行ベンチは未実施。todo.md にエントリあり。GC stress として
binary_trees の同等コードを CRuby で書くと、世代別 + write barrier の
ぶん 2-3× 速い見込み。ただし baruby は AST インタプリタ + libgc という
極めて単純な構成なので、その差はほぼすべて「処理系の素朴さ」由来。

## 5. AOT / PG モードの動作確認

§2 の表は 2026-05-10 検証済 (5 テスト + 3 ベンチ全部通過、plain と
出力一致)。SD\_\<hash\>.c は `-c` 時に `code_store/c/` に書き出される。
1 ファイルあたり inline static SD は 100〜400 個 (test_p1b で 403)、
all.so のエントリ (= public T シンボル) は AST root + 各関数本体ぶん
4-5 個。

新ノード (`node_str_lit` の `const char *` operand、`node_call_*` の
recv/idx/val、`node_spaceship`、`node_lshift`、`node_*_repeat`、
`node_to_s`、`node_to_i` など) はすべて `code_store/SD_*.c` 内で
`EVAL_<name>(...)` 形式に展開され、専用の HORG / HOPT ハッシュが
生成される。const char * の扱いは naruby の `node_call_builtin`
パターンを継承しているので問題なし。

JIT (`-j`) は `lstation.rb` 抜きでは動かないので unwired。`-j` 指定で
即 exit するように parser で wired ([todo.md](todo.md))。
