# baruby Done

[spec.md](spec.md) — 言語仕様、[runtime.md](runtime.md) — 実装、
[todo.md](todo.md) — 残タスク、[perf.md](perf.md) — ベンチ。

## 2026-05-10 — 初期フォーク

`sample/naruby` から `sample/baruby` を切り出し、Array + String + libgc
を導入。GC testbed として独り立ちさせた。

### 言語面

- naruby の int64-only から **LSB-tagged VALUE** に拡張 (1 = fixnum、
  0 = ptr、raw 0 = false/nil)。
- ヒープ型 **Array (BaArray)** と **String (BaString)** を追加。
  共通 `ObjectHeader` に type tag。
- 比較 / `&&` / `||` を `VAL_TRUE` / `VAL_FALSE` 正規化に変更。
  既存の `&&` 実装が `node_num(0)` (= INT2VAL(0) = raw 1, truthy) を
  false 相当として使っていた潜在バグを修正。
- 専用ノード `node_true` / `node_false` 追加。

### ノード追加

- `node_ary_new` / `node_ary_push` — リテラル評価のチェイン展開用。
- `node_str_lit(const char *, uint32_t)` — eval 毎に fresh alloc。
- メソッド desugar 用 dispatch nodes:
  `node_call_size`, `node_call_aget`, `node_call_aset`,
  `node_call_push`, `node_call_pop`。型タグで Array/String を branch。

### パーサ

`PM_ARRAY_NODE` / `PM_STRING_NODE` の "unsupported" stub を実装に置換。
`PM_CALL_NODE` で receiver が non-NULL かつメソッド名が builtin 表に
ある場合は対応する dispatch ノードに lower。
`PM_OR_NODE` も実装 (`PM_AND_NODE` と同型)。

### 値表現と既存ノードの調整

- `node_num`: `INT2VAL(num)` で wrap。
- `node_add`/`sub`/`mul`/`div`/`mod`: untag → op → tag。`node_add` のみ
  string concat (`baruby_str_concat`) も runtime branch で受け持つ。
- `node_lt`/`le`/`gt`/`ge`/`eq`/`neq`: tagged 値のまま signed 比較
  (untag 不要)、結果を `VAL_TRUE`/`VAL_FALSE` に正規化。

### libgc 統合

- `context.h` で全 system header の後ろに `malloc` / `calloc` /
  `realloc` / `strdup` / `free` を `GC_*` macro で wrap (asom と同じ
  パターン)。
- `main.c` 冒頭で `GC_INIT()`。
- Makefile の link line に `-lgc`。
- `BARUBY_GC_STATS=1` で `__GC_STATS__` 行を出力 (alloc_bytes /
  heap_bytes / gc_count、libgc の `GC_get_*` 由来)。

### ベンチ

`bench/binary_trees.ba.rb` (depth 21、~1s)、`bench/list_alloc.ba.rb`
(10M iter、~1s)、`bench/string_concat.ba.rb` (5M iter、~1s)。
ランナー `bench/run.rb` が plain/aot/pg を選んで全 bench を順に実行、
時間 + GC 統計を表示。`make bench` でも一発実行可。

### 動作確認 (`--plain` のみ)

- `test.ba.rb` (fib 20) で再帰 + 整数演算 OK (10946)。
- `test_ary.ba.rb` で配列 / 文字列 / index / size / push / pop /
  concat の挙動が期待通り。
- 3 ベンチがすべて完走、時間が ~1s スケールで GC が走っていることを
  確認 (12〜1700 collections)。

AOT / PG / JIT モードでの新ノード動作は未検証 ([todo.md](todo.md) P0)。

### 削除した naruby 資産

- `naruby_codegen.rb` (本人コメントで obsolete)
- `naruby_code.c` (生成済み AST のテストダンプ)
- `lstation.rb` (JIT サーバ — `-j` 自体を unwired にした)

## 過去の経緯

baruby 命名: naruby = "**n**ot **a** ruby"、abruby = "**a b**it ruby"
の中間 — "**ba**rely a ruby" → baruby。
