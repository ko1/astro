# todo.md — nuq 残作業

実装済みは [done.md](./done.md)、ベンチ結果は [perf.md](./perf.md)、
ランタイム解説は [runtime.md](./runtime.md)。

おおむねインパクト / 実装コスト順。bench で見えた具体的な outlier を
解消するもの (B-4, B-5) と、CPS 化で SD specialisation を全面解放する
もの (B-1〜B-3) が大物。

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

### B-1. streaming pipe
`f | g` は LHS 出力を一度配列に集めてから RHS を回す。長 stream
(`.[]` 経由の large array iteration) で memory 効率が落ちる。
代替: astrogre 流の continuation chain。少なくとも `.[]` から `g` への
転送だけ inline 化すれば、典型的な long pipeline のメモリ要件は減る。

### B-2. **全 builtin を node 化** (★ 最優先候補)
現状: `length`, `map(f)`, `select(f)`, `range(...)` などはすべて
**`node_call0/1/...` → 線形 builtin table → C 関数呼び出し** の
チェーンで dispatch される。SD specializer はノード単位で SD を焼くが、
`node_call*` の中身は `nuq_call_eval` (runtime helper) なので
inline 先で **builtin table lookup の cmp が定数畳み込みされない**。

これを **各 builtin を専用 NODE 化** することで、`length` →
`node_length`、`map(f)` → `node_map(f)`、`range(M;N)` → `node_range_2`
等にする。すると:

- builtin 名解決はすべて parse 時に終わる (table lookup ゼロ実行時)
- SD specializer が `node_length` の本体 (= `nuq_emit(c, nuq_length(c->input)); return BR_OK;`)
  を親 SD に inline できる
- 結果として `[range(n)] | reverse | length` が 1 個の SD 関数に
  fold-in され、range の for ループ・配列 push・reverse のループ・
  length の読み出し・最後の emit がすべて 1 関数の中で gcc に inline 最適化される

実装コスト: 中 (~70 builtin × 2-3 行 = 200 行のメカニカルな移植)。
B-1 と組合せると nuq AOT が interp を引き離せる絵が見える。

### B-3. SD specialization を pipe stage 越境で
B-1 + B-2 が前提。pipe を CPS 化すれば連結チェーンが 1 SD になる。

### B-4. emit_buf を per-call alloca ベース に
今は `c->emit_buf` が heap 配列。sub-eval ごとに新しい array を
`GC_malloc`。pipe stage の hot loop に立つので、固定サイズ alloca +
spill で大半は GC 不要にできる。pyramid bench (1.4× 遅) などに
効くはず。

### B-5. **オブジェクト lookup をハッシュに** (★ kv outlier の根本対策)
`nuq_object_get` / `_set` / `_has` は parallel array を線形走査。
小規模 object (n < 50 程度) では十分速いが、大型 object (kv bench で
n=5000 → 250ms vs jq 9.5ms = **25× 遅**) で破綻する。
- 案 1: object に `key_hashes[]` を持たせ、線形走査で fingerprint
  比較 → `nuq_eq`。実装コスト: 小、効果は中。
- 案 2: open-addressing hash table 化。挿入順序を保つために array
  併設。pystro の dict と同等。実装コスト: 中、効果は大。

### B-6. ビルトイン dispatch の hash 化
B-2 で先に解決される (builtin table 自体が消える)。

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
- **オブジェクトは順序保持** (現状 parallel array、B-5 で hash 化予定でも
  順序は保つ)。
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
