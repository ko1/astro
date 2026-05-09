# arcel performance notes

## Bench harness

`benchmark/run.rb` で arcel-interp / arcel-AOT / celgo_ref / **celcpp_bench**
の 4-way 比較。すべて同形式の `bench` サブコマンドを持ち、ハーネスは
`<bin> bench -e '<expr>' [-i '<json>'] -n <iters>` を呼び、
`<iters> <elapsed_ns> <ns_per_op>` を集約する。

ベンチ前提:
- bindings は **bench loop の前に 1 回だけパース**して pin
  (cel-go の `prg.Eval(binds)` も bindings 再利用が前提)
- per-eval transient arena は毎反復 reset
- warmup 1k 反復、計測 2M 反復、3 試行のメディアン

ベンチ案件:

| 名前 | 狙い |
|---|---|
| `arith_const` | 純 dispatch overhead — AOT の上限 |
| `bool_ladder` | short-circuit ladder — branch predictor sensitive |
| `field_access_shallow` / `_deep` | hash lookup chain の hot path |
| `predicate_user` | realistic policy: age + enum + `in [...]` |
| `list_all_small` / `_med` | quantifier × N — body specialization |
| `list_exists` | early exit path |
| `string_starts` / `string_contains_ladder` | C 文字列 op が hot |
| `k8s_admission_ish` | realistic K8s ValidatingAdmissionPolicy 式 |

## ベンチ結果 (2M iter / case, 3 試行のメディアン)

x86_64 / Ubuntu 24.04 / gcc 13.3:

| case | arcel/plain | arcel/AOT | celgo | **celcpp** | AOT vs celgo | **AOT vs celcpp** |
|---|---:|---:|---:|---:|---:|---:|
| `arith_const`            |   71 ns/op |   60 ns/op |   137 ns/op |   116 ns/op |  2.28× |  **1.93×** |
| `bool_ladder`            |   27 ns/op |    5 ns/op |    56 ns/op |   213 ns/op | 11.20× | **42.60×** |
| `field_access_shallow`   |   56 ns/op |   24 ns/op |   207 ns/op |   549 ns/op |  8.62× | **22.90×** |
| `field_access_deep`      |   70 ns/op |   28 ns/op |   195 ns/op |   791 ns/op |  6.96× | **28.25×** |
| `predicate_user`         |   80 ns/op |   44 ns/op |   522 ns/op |   670 ns/op | 11.86× | **15.23×** |
| `list_all_small` (5)     |  128 ns/op |  111 ns/op |  1204 ns/op |   977 ns/op | 10.85× |  **8.80×** |
| `list_all_med` (100)     | 2361 ns/op | 2049 ns/op | 19723 ns/op | 14676 ns/op |  9.63× |  **7.16×** |
| `list_exists` (100)      | 1069 ns/op | 1130 ns/op | 10467 ns/op |  7475 ns/op |  9.26× |  **6.62×** |
| `string_starts`          |   14 ns/op |    7 ns/op |   101 ns/op |   223 ns/op | 14.43× | **31.86×** |
| `string_contains_ladder` |   73 ns/op |   58 ns/op |   201 ns/op |   353 ns/op |  3.47× |  **6.09×** |
| `k8s_admission_ish`      |  118 ns/op |   52 ns/op |   601 ns/op |  1167 ns/op | 11.56× | **22.44×** |

幾何平均 (arcel-AOT vs):
- **cel-go**: ~9×
- **cel-cpp**: ~14×

realistic K8s ValidatingAdmissionPolicy で **cel-go の 11.6×、cel-cpp の 22.4×**。
list iteration で cel-go 比 9〜10×、cel-cpp 比 6〜9×。
constant fold が効く `bool_ladder` で cel-cpp 比 42.6×、`string_starts` で 31.9×。

## AOT が刺さる構造

ASTro の partial evaluation がやってる主な仕事:

1. **AST literal の C リテラル化** — `node_str_lit("@example.com", 12)` を
   SD に焼き込み、後段の `memcmp(s, "@example.com", 12)` を gcc が
   1 命令の cmp に固定
2. **dispatcher の devirtualize** — `EVAL_ARG(c, body)` → 子 SD の
   直接呼び出し → さらに inline (SD-in-SD)
3. **constant fold** — `true && false || true` のような定数式は
   tree 全体が `return 1;` まで畳まれる (bool_ladder で 5 ns/op)
4. **ヘルパの inline** — value.h で `arcel_field` / `arcel_eq` /
   `arcel_starts_with` 等を `static inline`、SD compile 時にまるごと
   展開 → memcmp の constant fold + dead code 削除

## 実装した最適化 (時系列)

1. **`(VALUE)` キャスト除去** (arcel_gen.rb): VALUE が struct な
   サンプルで SD compile 失敗を回避。
2. **binary-safe な `const char *` SD 焼き込み** (arcel_gen.rb +
   `arcel_fprint_blob_lit`): `\NNN` 8進エスケープで NUL/非印字対応。
   `<x>` operand と `<x>_len` operand のペア命名規約を導入。
3. **macro body は EVAL_ARG 経由** (eval_with_bind を statement-expr
   macro 化): SD-specialized 子 SD が dispatch されるよう。
   list_all 系で plain → AOT で +35%。
4. **chunked arena** (`arcel_arena_chunk` linked list): grow 時に
   既存ポインタを invalidate しない (= nested alloc 安全)。
5. **arena_alloc bump fast path inline** (value.h): 1 add + 1 cmp + 1 store。
6. **`arcel_field` / `arcel_lookup_ident` / `arcel_eq` inline**
   (value.h): SD で `memcmp(k.s.p, "<literal>", N)` を 1 命令の cmp に
   固定可能に → field_access_shallow が 60 → 24 ns/op (60% カット)。
7. **`arcel_starts_with` / `arcel_ends_with` / `arcel_contains` inline**:
   string_starts が 22 → 7 ns/op (cel-cpp 比 31.9×)。
8. **`arcel_in` / `arcel_size` inline**: predicate_user で +10〜15%。
9. **`arcel_index` inline**: k8s_admission の `labels["team"]` で効く。
   k8s_admission が 70 → 52 ns/op。
10. **`arcel_push_bind` / `arcel_pop_bind` inline**: macro hot loop。
11. **constant list folding** (parser.c): `[a, b, c]` の全要素が pure
    literal なら parse 時に 1 回 build して cache、`node_const_list(idx)`
    として emit。`role in ["admin", "user"]` 系で per-eval alloc 不要に。
    predicate_user が 64 → 44 ns/op。

## 残る最適化候補

- **map literal の constant fold** (`{...}` 全要素 literal の時)
- **`xs.matches(re)` の事前 compile** — 現状毎回 POSIX regcomp。
  AOT 時に regex_t を SD const_pool に pin か、astrogre backend に切替
- **Hash table for big maps** — 現状は flat array linear scan。
  N >= 8 で hash table に切り替え (`arcel_field` / `arcel_index` 内分岐)
- **list literal 子要素の節 specialize** — `arcel_node_arr[idx]` 経由
  だと AOT で個別 SD が選ばれない (= variadic children の制約)。
  list 要素ごとに名前付き operand を持つ別 node 種を generate するか

## 計測手順

```sh
# kernel symbol で perf を見たいので perf_event_paranoid を緩める
sudo sysctl kernel.perf_event_paranoid=1

make
perf record -F 999 -g -- ./arcel bench -e '<expr>' -n 50000000
perf report --stdio | head -40

# specialize されたコードを直接見る
nm code_store/all.so | grep ' T SD_'
objdump -d code_store/all.so | sed -n '/<SD_xxxx>:/,/^$/p'
```

## 既知の罠

- **CCACHE_DISABLE=1**: `astro_cs_build` の `make` が ccache 経由で落ちる
  (sandbox 環境)。`make` / `ruby` を呼ぶ前に prefix 必須。
- **bench iter 数**: 1M 未満だと subprocess + clock_gettime オーバヘッドで
  結果が荒れる。2M 以上推奨。
- **CEL string の codepoint vs byte**: `'\377'` は U+00FF (UTF-8 で 2 bytes)、
  `b'\377'` は 1 byte 0xFF。CEL parser は string と bytes literal で
  エスケープの解釈を分ける必要あり (実装済)。
- **`\xHH` greedy hex consume**: SD literal 焼き込みで `\xc3\x9fe` は
  `\xc39f` として誤解釈される。3桁固定の 8進 `\NNN` を使うこと。
- **macro body は EVAL_ARG 経由**: helper 関数で `EVAL(c, body)` を
  呼ぶと AOT の SD 特殊化が拾われない。NODE_DEF body の中で直接
  `EVAL_ARG(c, body)` するか、statement-expression macro 化する。
- **inline は size 制約あり**: `arcel_field` のような小さい関数
  (~20 命令) は inline で win。`arcel_to_string` のような大きい関数
  (snprintf 経由) を inline すると SD body 肥大で逆に遅くなる
  (`feedback_inline_caution`)。
