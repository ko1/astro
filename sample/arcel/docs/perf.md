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

## ベンチの公平性 — どこまで信じていいか

「conformance 100% で interp ですら cel-go/cel-cpp の 2〜10× 速いって、何か
ずるしてない?」というのは正当な疑問なので、何が公平で何がそうでないか
読者向けに開示する。

### 公平にしている点

- **3 binary とも parse は loop の外で 1 回**。bindings も loop の外で 1 回
  (cel-cpp 公式 bench は `Activation` も per-iter に作るが、こちらは再利用
  = cel-cpp に有利な側に倒している)。
- **計測区間は loop だけ**。プロセス起動・builtin 登録 (`RegisterBuiltinFunctions`)
  ・JIT warmup などはどれも `clock_gettime`/`time.Now`/`steady_clock` の前後
  にあるので外。
- **同じ式・同じ入力データ**。conformance suite で意味的等価性は 808/808 で
  確認済み。
- **cel-cpp は `-c opt --enable_optimizations`** = cel-cpp 側の constant
  folding を有効化した状態。

### 構造的に arcel に有利な点 (= ずるではないがバイアス)

| 要素 | arcel | cel-go | cel-cpp |
|---|---|---|---|
| 値表現 | 16-byte tagged union (SysV ABI で 2 register 返り) | `ref.Val` Go interface (16-byte fat pointer + heap escape) | `CelValue` (24+ byte variant、box dispatch あり) |
| dispatcher | 関数ポインタ直接呼び出し | Go interface = vtable lookup + type check | virtual call + type-erased ExpressionStep |
| field access | `arcel_map.entries[]` の linear scan (typical N=5-10) — cache 線形 / hash 計算ゼロ | `Activation` 経由で Go `map[string]ref.Val` (hash compute) | `absl::flat_hash_map` (hash compute + bucket walk) |
| per-iter alloc | bump-arena reset (`used = 0` を chain で 5 命令) | Go GC が値オブジェクトを確保 / 回収 | **`google::protobuf::Arena` を毎回コンストラクト** (= 数百 ns の intrinsic コスト、API 上回避不能) |
| NODE 本体 | 小さい C inline 関数 → gcc が SROA + reg alloc | Go の interface 越しなので escape して heap 行き | C++ だが variant 介在で多段 dispatch |

これらは **言語選択 (Go の interface・C++ の variant) と API 設計 (cel-cpp
の per-iter arena) の差**で、ASTro/arcel 固有の魔法ではない。同じ設計を Go
や C++ で書けば arcel に近づくはずだが、両者とも production 用ライブラリ
として API 互換性を優先して現状を選んでいる。

### 「未対応 features を入れたら遅くならない?」

arcel は cel-spec の ~3.5% (29/837 ケース) を skip している。残り機能を
入れたときの cost 見積もり:

| 未対応機能 | 既存ベンチへの影響 | 理由 |
|---|---|---|
| timestamp / duration | **0%** | 専用 tag + 専用 op で別経路。int/string/list の hot path は無関係 |
| optional (`x.?y`) | **0%** | `?.` 用に別 node type を作れば AC_MAP の field access は不変 |
| proto wrappers | **0%** | unwrap は proto 経路のみ |
| ext.* lib (`strings.replace` 等) | **0%** | 関数を追加するだけで既存 NODE_DEF に手を入れない |
| container / type_env disable_check | **0%** | parse-time 型検査、runtime cost なし |
| **proto message literal** (`TestAllTypes{...}`) | **0〜5%** | `arcel_field` に `if (recv.tag == AC_PROTO)` 分岐が増える。always-map なベンチでは branch predictor が常に外すので penalty 数 ns 以下 |

最大のリスクは **proto 対応**で、これは:
- libprotobuf を抱える (~MB バイナリ増)
- field access の分岐が `AC_MAP` と `AC_PROTO` の 2-way に
- VALUE tag の追加で switch case 増 (jump table のまま)

それでも `k8s_admission_ish` のベンチ (cel-cpp 比 22.4×) が 15〜30% 程度
遅くなるくらいで、**15〜19× の差は維持できる**見積もり。

### 売り文句の現実的な範囲

「**realistic K8s ValidatingAdmissionPolicy で cel-cpp 比 22.4×**」には
注釈があって、現実の K8s admission webhook は input が **proto message**
(AdmissionReview) で来る。本ベンチでは入力を map で渡しているので
field access が hot path を通る。

proto 対応版を作ると:
- 入力 unmarshalling コスト (proto wire → 内部 repr) は **bench 計測外**
  (= bindings 構築時にやる) なのでベンチ数値は変わらない
- 実 K8s 流の比較で cel-cpp も同じ unmarshalling コストを払うので、
  「**eval 部分だけ**」を比べた相対は維持される
- ただし field access の `AC_PROTO` 経路ぶん 15-20% 程度の絶対 cost が
  乗ってくる

数字で言うと予測は:

| | 現状 (map 入力) | proto 対応後 (proto 入力、予想) |
|---|---:|---:|
| arith_const (proto 不使用) | 60 ns | 60 ns (無変化) |
| field_access_* (map → proto) | 24-28 ns | 30-35 ns (~20% 増) |
| **k8s_admission_ish** | 52 ns | 60-70 ns (15-30% 増) |
| cel-cpp 比 | **22.4×** | **15-19×** (推定) |

「**残り 4% の機能を入れると 15-20% 遅くなるが、それでも cel-cpp 比は
15× 残る**」というのが正直な見積もり。

### まとめ

- arcel が速いのは **「tagged union + 直接 function pointer の tree walker は、
  Go interface VM や C++ variant VM より構造的に速い」** という当たり前の
  事実を、conformance 守ったまま実測してるから
- ベンチ harness は **cel-cpp に若干有利**な側に倒してある (Activation
  再利用、constant_folding 有効化)
- 未対応機能 (proto 等) を入れると 15-30% 程度のコストが乗るが、cel-cpp
  比のオーダー (10×〜) は維持できる見込み
- AOT モードの大勝 (bool_ladder 42.6× 等) は別の話で、ASTro の partial
  evaluation そのものの効果

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
