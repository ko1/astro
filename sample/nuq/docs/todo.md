# todo.md — nuq 残作業

実装済みは [done.md](./done.md)、ベンチ結果は [perf.md](./perf.md)、
ランタイム解説は [runtime.md](./runtime.md)。

おおむねインパクト / 実装コスト順。**B-2 / B-3 / B-5 / B-7 は実装済**
(`done.md` v0.2 / v0.3 節)。残ってるのは:

- **A 系**: 言語仕様の穴 (代入 / path / regex 等)
- **B-1**: streaming pipe (alloc pattern 改善、現ベンチ範囲ではほぼ
  影響無し)
- **B-4**: pool top の register 常駐化 (pyramid に効く可能性)
- **B-7 の追加 fusion ルール**: `[X] | unique` など
- **C 系**: CLI options
- **D / E**: 内部整理 / 細かい仕様差
- **F**: JSON 以外の入力 (将来)
- **G**: テスト / ベンチ拡張

現状での AOT-only 上振れの本筋は **PGO (型 feedback + guard)** で、
ASTro framework に部品はあるが nuq には未配線。`perf.md` の
「value 演算 inline」節を参照。

## A. 機能 — 言語仕様の穴

### A-1. `=` / `|=` / `+=` / `-=` / `*=` / `/=` / `%=` / `//=` 代入
jq の左辺は **path 表現**。`.foo[0].bar` は input から `["foo", 0,
"bar"]` という path 配列を取り出すフィルタとして振る舞う必要があり、
あらゆる accessor (`.foo`、`.[i]`、`.[]`、slice、…) が値ではなく path
を返す mode を持たねばならない。
**実装コスト**: 大。各 accessor に `path_eval` バリアントを追加 +
`update` バリアント (元 input + path + 新値 → 新 input)。

### A-2. path-aware `del / setpath / delpaths / paths(f) / leaf_paths`
A-1 と同根。`paths` 自体は実装あり (再帰的に key/index 列を集める)
だが、`del(.foo)` のように **path 表現を受け取って input を変えて返す**
やつはまだ無い。

### A-3. 真の `test / match / capture / sub / gsub / splits / scan`
`sample/astrogre` 経由で integrate する方針 (project memory
`regexp_astrorge`)。今 `test` は substring 一致のみ、それ以外は未実装。

### A-4. multi-elif chain
2 段以上の `elif` でパーサが落ちる。`parse_primary` の `IF` 分岐を
再帰的に書き直す。実装コスト: 小。

### A-5. `recurse(f)` / `recurse(f; cond)`
今の `recurse` は 0 引数 (= `..`) だけ。1 引数版は body の
fixed-point。jaq bench の `from`, `tree-flatten` 系で必要。

### A-6. その他文字列メソッド
`ltrimstr / rtrimstr`、`tojson` の indent option、`tostream / fromstream`、
`ascii_downcase` の Unicode 対応、`tonumber` の base prefix。

### A-7. `inside / contains` の再帰版
今は `==` 等価という超簡略版。jq の `contains` は string-in-string、
array subset、object subset の再帰判定が必要。

### A-8. `walk(f)` ほか高階組み込み
`def walk(f): ...` を builtin に登録するだけ。

## B. パフォーマンス (bench で具体化されたもの)

### B-1. streaming pipe (alloc 削減、AOT vs interp には効かない)
`f | g` は LHS 出力を一度 EMIT pool に集めてから RHS を回す (stack
discipline で巻き戻しはするが per-stage の slice は確保される)。
長 stream (`.[]` 経由の large array iteration) で memory 効率が落ちる。
CPS chain にすれば slice 化を消せる。
**注意**: これは alloc pattern の話で specialization の話ではない。
pipe は既に 1 個の `node_pipe(lhs, rhs)` で SD specializer は両側に
入れている。AOT が interp を引き離す効果は無い (interp も同じ
alloc を使う)。

### ~~B-2. 全 builtin を node 化~~ (DONE)
parser が `BUILTIN0/1/2/3` で各 builtin を専用 NODE allocator に
振り分けており、`node.def` には `node_b_length` `node_b_keys`
`node_b_map` ... と 60 個 dedicated NODE がある。runtime の
linear builtin table は無い。

### ~~B-3. opaque な value 演算を header inline に~~ (DONE 2026-05-06)
fast path を `context.h` の `static inline` に切り出し、slow path を
`value.c` の `_slow` 接尾辞付き関数に。inline 対象:

- `nuq_op_add / sub / mul / neg` (fix+fix overflow check 経由で fix)
- `nuq_eq` (same-VALUE / fix+fix shortcut)
- `nuq_cmp` (fix+fix で `(la>lb)-(la<lb)` 1 命令)
- `nuq_truthy` (fixnum は常に truthy)
- `nuq_make_int` (range 内なら NUQ_FIX、超えたら _slow)

効果 (vs jq):
- `min-max 1M`: 6.0× → **9.7×** (絶対値 33ms → 21ms)
- `sort 300k`: 3.5× → **5.6×** (36ms → 24ms)
- `group-by 100k`: 5.3× → **7.3×** (29ms → 21ms)
- `cumsum 500k`: 5.0× → **5.6×**

ただし `static inline` は interp 側からも見える (両者から context.h
を include) ので、interp と AOT が**両方**同じ程度伸びた。AOT が
interp を引き離す動きは出ていない (操作の type を SD 側で folded
する仕組みが今の ASTro にはない)。それでも nuq の絶対性能は確実に
向上したので、value 表現の C 関数 → header inline 化は
cross-sample に有効な定石として記録。

### B-4. EMIT pool の sub-call 巻き戻しのコスト
現在 `c->pool_top = top0` を毎 NODE_DEF 終端で書き戻す。stack
discipline は正しいが、pool_top の load/store が hot loop に立つ。
per-call で stack 上 (alloca) の固定 buffer + overflow spill にすれば、
top の状態がレジスタ常駐になる。pyramid bench に効く可能性。

### ~~B-5. オブジェクト lookup をハッシュに~~ (DONE 2026-05-06)
`nuq_obj.obj` に `uint32_t *idx` を追加、open-addressing (FNV-1a +
load factor ≤ 0.5)、`NUQ_OBJ_HASH_MIN=16` 超で lazy build。挿入順
parallel array は維持。`add` builtin にも `all_objects` fast path を
入れて pairwise `nuq_clone` カスケードを撲滅。kv 5k: 0.04× → 1.65×。

<!-- B-6 (ビルトイン dispatch の hash 化) は B-2 が done なので不要 → 削除 -->

### B-7. AST fusion (parse 時の peephole 書き換え)
ルール (現状 4 つ + 右辺エッジ fusion 実装済 — 詳細は done.md `v0.3` 節):

- ✅ `map(F) | map(G)` → `map(F | G)`
- ✅ `select(F) | select(G)` → `select(F and G)`
- ✅ `[body] | length` → `emit_count(body)` (try-catch / cumsum で大効果)
- ✅ `[body] | add` → `emit_fold_add(body)` (`add` kernel を共有)
- ✅ 右辺エッジ fusion: 左結合 chain を 1 段ずつ折り畳む

未実装の候補:

- `[body] | unique` → set 状の集約 (中間配列はあっても hash で重複を
  早期排除)。`unique` は内部で sort するので alloc 削減は限定的。
- `[body] | sort_by(K)` → in-place sort + key 抽出の合体。
- `range(N) as $x` の foreach パターンの単純化。
- `{key: V} | .key` → `V` (path 抽出の peephole)。
- `node_array(body) | node_b_reverse` → reverse-emit-collect。
- `add | unique` → 連続 fusion (`emit_fold_add` の出力に直接 unique を
  適用する形は今でも動くが、unique 側で sort するので大した差なし)。

健全性検証は意味保存が肝。jq との差分テスト 169 件で常時チェック。
意味判断に迷う rewrite は入れない (副作用、エラータイミング、
multi-emit 順序、`and`/`or` 短絡を破らない)。

## C. CLI / I/O

### C-1. `--arg name value` / `--argjson name value`
ユーザ提供変数。`$name` で参照。

### C-2. `--slurpfile` / `--rawfile`
ファイル全体を変数に bind。

### C-3. `input` / `inputs` builtin
multi-document iteration。

### C-4. `--seq` (RFC 7464)
レコードセパレータ `\x1e` 区切り。

### C-5. `-nc` のような連結 flag
今は `-n -c` と分けないと駄目。

### C-6. exit コード
jq は match なしで 1、エラーで 2 等。今は 0/1 だけ。

## D. 構造化 / 内部整理

### D-1. side table の分離
`runtime.c` は eval helpers + side table が同居。
`tables.c` 等に切り出し。

### D-2. `def`-with-filter-args の cleanup
`def map(f): ...; map(.+1)` で `f` を 0-引数の pseudo-def として CTX
に登録するが、関数復帰時に **個別に pop していない** (`func_cnt` を
watermark で巻き戻す)。動くが脆い。proper "call frame" + 関数
スナップショット保存に。

### D-3. `parse_postfix` と `parse_term_for_keyword` の重複
`.foo / .[i] / .[]/ .[a:b]` のロジックが両方にコピーされている。

## E. 仕様 / 互換性

### E-1. 数値整形の jq 互換
非常に大きい / 小さい double で jq と微妙に出力が変わる可能性。
要差分テスト。

### E-2. `@csv` の quoting
今は単純に `"` を `""` に置換。jq の処理は full RFC 4180 ではないが
微妙な差があり得る。

### E-3. エラーメッセージ format
今は terse でばらばら。jq の "Cannot iterate over X (...)" 等の
正確な表記に揃える。

## F. 拡張: JSON 以外を query する

### F-1. 新しい front-end parser
- **YAML / TOML**: そのまま `nuq_obj` に落とせる。`--input yaml` で
  parser を選ぶ仕組み。
- **CSV / TSV**: ヘッダ行をオブジェクトキーに、後続行を 1 オブジェクト
  ずつ stream emit。
- **XML**: 表現に選択肢が複数 (badgerfish / `{tag, attrs, children}`)。
  要設計議論。

### F-2. value 表現を抽象化
今は `struct nuq_obj` 直アクセス。`value_provider_t` の vtable 経由に
すれば、SQL のような行 + カラム表現や grep 風の行ストリームでも
nuq のフィルタ言語が使える。実装コスト大、恩恵が見えない限り
着手しない。

## G. テスト / ベンチ

### G-1. jq 公式テストの残り
`tests/jq.test` の path / assignment / regex 系は機能未実装で skip。

### G-2. fuzzing
クラッシャ潰し。

### G-3. ベンチ拡張
- `qj` (大規模 NDJSON 寄せの実装) を見つけ次第追加
- 入力 JSON を含むベンチ (今は `n` を stdin で渡すだけ)
- ストリーム入力ベンチ (`-R` / inputs 系)

## 設計上の妥協 (変えない)

- **GC は Boehm-Demers-Weiser**。VALUE が 8-byte aligned ポインタ +
  1-bit fixnum タグで conservative GC で安全。
- **オブジェクトは順序保持** (parallel array で挿入順、+ lazy hash idx
  で lookup O(1))。
- **string slice はコピー** (jq に揃える、buffer 共有はしない)。

## 進捗

- v0 (2026-05-05): 全 7 設計タスク完了。test 0 → 338 (うち約半分は
  system jq との差分)。実装中に jq と差分テストが値モデル / interp /
  try-stream / parse の細部のバグを 5 つ拾った (`done.md` 末尾参照)。
- v0.1 (2026-05-05 同日): jaq 流ベンチ追加 (jq / jaq / gojq との同条件
  比較、14 ケース)。bench 駆動で見つけた重要バグ:
  - **CTX を `calloc` で確保していたため `var_stack` が GC に回収される
    バグ** (`$x undefined` が n>=490 で発生) を修正。pystro / astr に
    倣い `GC_malloc(CTX)` に。upto が 3.4× → 39× / cumsum が 1.4× 遅
    → 1.1× 速と劇的改善。
  - `nuq_clone(object)` が `nuq_object_set` 経由で O(n²) 動作 →
    オブジェクト `+` 連結が O(n³)。直接 push に修正、kv n=5000 が
    119s → 0.25s。
  - `group_by` の挿入ソートを qsort に置換。group-by 100k が timeout
    → 94ms。
  - `node.def` から (ほぼ) 全 `@noinline` 撤去。
- v0.1 ベンチ結果: nuq は 14 中 11 で jq に勝ち (詳細は `perf.md`)、
  ack 329×、upto 39×、reverse 21× ほか大勝。残る 3 outlier (kv 25× 遅、
  add 2.7× 遅、pyramid 1.4× 遅) は B-1/B-2/B-4/B-5 が解けば解消する
  はず。
- v0.2 (2026-05-06): outlier 撲滅パス。bench 駆動で見つけた / 直したもの:
  - **`def` 本体を独立 SD entry に登録** (`nuq_compile_all_def_bodies`
    / `nuq_load_all_def_bodies` in runtime.c)。これが無いと再帰呼び出し
    は SD 化されない (top-level filter の SD は `EVAL(c, fd->body)`
    の runtime ポインタを越えられない)。upto 24× → 63× / ack interp
    比 +20%。
  - **B-5 完了**: object hash idx (open-addressing FNV-1a, load 0.5,
    threshold 16)。挿入順 parallel array は維持。
  - **`add` builtin の `all_objects` fast path**: pairwise `nuq_op_add`
    → `nuq_clone` カスケードを 1 fresh-object merge に置換 (kv 5k
    の根本対策、idx と組合せ)。
  - **`error` (0-arg) builtin 登録漏れ修正** (`filter.c` の BUILTIN0
    登録に 1 行追加)。pre-existing bug。try-catch 0.26× → 8.75×。
  - **object literal の fast path**: 全エントリ count==1 の典型ケース
    では cartesian iteration を skip、pool 直書きで GC_malloc を
    エントリごとに節約。static key は parser で `nuq_make_string`
    を 1 度だけ実行 → entry に VALUE で保存。
  - **EMIT pool の startup pre-grow** (4096 entries)。`nuq_pool_push`
    の UNLIKELY realloc 分岐がほぼ常に未踏になり、関数全体が分岐
    予測通りに通る。
- v0.2 ベンチ結果: micro 14 中 13 で jq 越え (pyramid のみ 0.96×)、
  実用 11/11 越え。outlier 0 件。`make bench` の出力は perf.md 参照。
- v0.3 (2026-05-06): value op の `static inline` fast path 化 + AST fusion
  4 ルール + 右辺エッジ fusion。`min-max 1M` 6.0×→9.7×、`sort 300k`
  3.5×→5.6×、`group-by 100k` 5.3×→7.3×、`try-catch 500k`
  8.79×→**12.89×**、`cumsum 500k` 5.0×→**7.5×**。詳細は done.md / perf.md。
