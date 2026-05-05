# todo.md — nuq 残作業

実装済みは [done.md](./done.md)、性能ノートは [perf.md](./perf.md)、
ランタイム解説は [runtime.md](./runtime.md)。

おおむねインパクト / 実装コスト順。

## A. 機能 — 言語仕様の穴

### A-1. `=` / `|=` / `+=` / `-=` / `*=` / `/=` / `%=` / `//=` 代入
jq の左辺は **path 表現**。`.foo[0].bar` は input から `["foo", 0,
"bar"]` という path 配列を取り出すフィルタとして振る舞う必要があり、
あらゆる accessor (`.foo`、`.[i]`、`.[]`、slice、…) が値ではなく
path を返す mode をもつ必要がある。
**実装コスト**: 大。各 accessor に `path_eval` バリアントを追加 +
`update` バリアント (元 input + path + 新値 → 新 input)。

### A-2. path-aware `del / setpath / delpaths / paths(f) / leaf_paths`
A-1 と同根。`paths` 自体は実装あり (再帰的に key/index 列を集める)
だが、`del(.foo)` のように **path 表現を受け取って input を変えて
返す** やつはまだ無い。

### A-3. 真の `test / match / capture / sub / gsub / splits / scan`
`sample/astrogre` 経由で integrate する方針 (project memory
`regexp_astrorge`)。今 `test` は substring 一致のみ、それ以外は未実装。
astrogre の C API を `builtin.c` から呼ぶだけだが、capture を
オブジェクト形式 (`{offset, length, string, name}`) で返す変換層が
要る。

### A-4. multi-elif chain
`if a then x elif b then y elif c then z else w end` のように 2 段以上の
`elif` が来るとパーサが落ちる。`parse_primary` の `IF` 分岐を再帰的に
書き直す。実装コスト: 小。

### A-5. `recurse(f)` / `recurse(f; cond)`
今の `recurse` は 0 引数 (= `..`) だけ。1 引数版は body fixed-point。

### A-6. 文字列メソッドの欠落
`ltrimstr / rtrimstr / ascii / explode` はあるが `splits / scan / sub /
gsub` (regex 経路)、`tojson` の indent option、`tostream / fromstream`、
`ascii_downcase` の Unicode 大小文字、`tonumber` の `0xFF` ほか base
prefix。

### A-7. `inside / contains` の再帰版
`contains(b)` は jq では string-in-string、array subset、object
subset の再帰判定。今は `==` と等価という超簡略版なので落ちる
ケースが多い。

### A-8. `walk(f)`
木全体に `f` を bottom-up で適用するやつ。`def walk(f): . as $in |
... | f;` で書けるので、`def` 経由で組み込みとして登録するだけ。

### A-9. `getpath / setpath / delpaths` の path 配列対応
`getpath` は実装済 (path 配列で chain 連鎖)。`setpath` / `delpaths`
は未着手。A-1 と一緒に。

### A-10. `tostream / fromstream / truncate_stream`
streaming JSON 変換系。今は materialize ベースなので意味が変わる。
A-1 ほどではないが大きめ。

## B. パフォーマンス

### B-1. streaming pipe
`f | g` は LHS 出力を一度配列に集めてから RHS を回す。長 stream
(`inputs | …` や巨大 array `.[]`) では memory 効率が落ちる。
代案: astrogre 流の continuation chain に部分的に切替える。
- 実装コスト: 中。少なくとも `.[]` から `g` への直接転送だけ
  inline 化すれば、典型的な long pipeline の許容メモリは下がる。

### B-2. SD specialization が pipe stage を超えて効かない
B-1 と同根。今は各 helper (`nuq_pipe_eval` ほか) が runtime.c 側に
いて、SD specializer はノード単位で焼くだけ。Tree shape が動的に
変わる以上、全 chain の fold-in は本質的に B-1 を解決しないと
できない。

### B-3. ビルトインの dispatch
`builtin.c` の `table[]` を線形走査して `name_id` × `arity` で
マッチさせている。70+ エントリで cold だが、`map` `select` `length`
のホットパスは ~3 cmp で当たる位置に置いてある。
- 改善案: closed-addressing hash で `(name_id, arity)` → fn 直引き。
  実装コスト: 小。

### B-4. emit_buf の所有
`c->emit_buf` は `nuq_array` (= heap obj) を直接指している。
sub-eval のたびに新しい array を `GC_malloc` する。
- 改善案: per-call `alloca` バッファ + spill-on-overflow。SD
  specializer に「成長は実質起きない」を伝えられれば inline
  限界が伸びる。

### B-5. オブジェクト lookup を線形からハッシュに
`nuq_object_get` は今 `nuq_eq(keys[i], key)` で線形 O(n)。
キーは普通 string なので、object 側に `key_hashes[]` を持たせて
fingerprint 一致でフィルタすれば実用上は O(1) に近づく。
n が小さい間は無効化に近いので **メリットは大型 JSON 用**。

## C. CLI / I/O

### C-1. `--arg name value` / `--argjson name value`
ユーザ提供変数。`$name` で参照。実装は `nuq_var_push` を起動時に
呼ぶだけだが、parse 時に bind しておかないと参照側で詰まる。

### C-2. `--slurpfile name file` / `--rawfile name file`
ファイル全体を変数に bind。

### C-3. `input` / `inputs` builtin
multi-document iteration。

### C-4. `--seq` (RFC 7464)
レコードセパレータ `\x1e` で区切られた JSON 列の入力。

### C-5. exit コード
jq は match なしで 1、エラーで 2、ほか 5 など意味付けあり。
今は 0 / 1 だけ。

### C-6. `--debug-trace` / `debug` builtin
デバッグ補助。

## D. 構造化 / 内部整理

### D-1. side table の分離
`runtime.c` は eval helpers + side table (`lit_tab` / `interp_tab` /
`obj_tab` / `args_tab` / `def_tab` / `fmt_tab`) が同居。専用 TU に
分けたほうが見通しが良い。

### D-2. `def`-with-filter-args の cleanup
`def map(f): ...; map(.+1)` で f を 0-引数の pseudo-def として CTX に
登録するが、関数復帰時に **個別に pop していない** (`func_cnt` を
watermark で巻き戻す)。動くが脆い。proper "call frame" + 関数
スナップショット保存に。

### D-3. `parse_postfix` と `parse_term_for_keyword` の重複
`.foo / .[i] / .[]/ .[a:b]` のロジックが両方にコピーされている。
ヘルパ抽出。

### D-4. CTX → CTX_t typedef
`CTX_struct` → `CTX` の typedef は良いが `struct CTX_struct` を
直接書いている所がある。揃える。

### D-5. ASTro Code Store の build エラーは今プロセスに伝播しない
`fprintf(stderr, "astro_cs_build: make failed (exit 512)")` が出ても
exit 1 はしない (interpreter にフォールバックするため)。意図した
挙動だが、CI では `CCACHE_DISABLE=1` でしか build を成功させられない
場面があるので env 検出して warning に降ろすか、`--strict-aot`
オプションで失敗時 exit にできるようにするか検討。

## E. 仕様 / 互換性

### E-1. 数値整形の jq 互換
非常に大きい / 小さい double で jq と微妙に出力が変わるかもしれない。
`%.*g` で round-trip するまで precision を上げているが、jq は
独自実装。要差分テスト。

### E-2. `@csv` の quoting
今は単純に `"` を `""` に置換するだけ。jq は実は full RFC 4180
ではない (制御文字の扱いほか)。要差分。

### E-3. `keys` の key sort
キー全部 string なので memcmp で OK。jq の sort は full nuq_cmp と
同じはず。

### E-4. エラーメッセージ format
今は terse で内容バラバラ。jq の "Cannot iterate over X (...)" 等の
正確な表記に揃えたほうが、`try` でメッセージ判定するスクリプトが
動く。

### E-5. `null + null = null` 等の空集合演算
jq は `[1,2,3] - null` や `null + {a:1}` を許す (null をニュートラル
要素とみなす)。`+` は実装済。`-` などは要確認。

## F. 拡張: JSON 以外を query する

長期的に「JSON 以外も query できるといい」というオリジナル要望に
向けて。

### F-1. 新しい front-end parser
- **YAML / TOML**: そのまま `nuq_obj` の primitive + array + object に
  落とせる。`--input yaml` / `--input toml` のような flag で parser
  を選ぶ仕組みを用意するだけ。実装コスト: 低 (parser 自体は外部
  ライブラリでも自前でも)。
- **CSV / TSV**: ヘッダ行をオブジェクトキーに、後続行を 1 オブジェクト
  ずつ stream emit。
- **XML**: 表現に選択肢が複数 (badgerfish / `{tag, attrs, children}`
  triple ほか)。要設計議論。
- **MessagePack / CBOR**: バイナリ JSON 等価。並べる優先度低。

### F-2. value 表現を抽象化
今は `struct nuq_obj` 直アクセス (例: `NUQ_PTR(v)->arr.items[i]`)。
これを `value_provider_t` の vtable 経由にすれば、SQL のような行 +
カラム表現や grep ライクな行ストリームでも nuq のフィルタ言語が
使える。
- 実装コスト: 大。アクセサ全置換 + ホットパスでの perf 劣化。
  恩恵が見えない限り着手しない。

### F-3. 出力フォーマット
今は JSON pretty / JSON compact / raw string のみ。`--output yaml`
等を入れるなら F-1 と対。

## G. テストの拡充

### G-1. jq 公式テスト ([jq.test](https://github.com/jqlang/jq/blob/master/tests/jq.test))
今は 100 件中の subset (約 70 件) を `09_jq_canonical.diff.test` に
入れて差分。残り (path / assignment / regex 等) は機能未実装で skip。

### G-2. fuzzing
`make fuzz` で AFL/honggfuzz でクラッシャを潰す。

### G-3. ストレス
- 巨大 JSON (10 MB+) でメモリ動作確認。
- 深い nest (>1000) でスタック overflow チェック (recurse / interp
  ファンアウトが exponentially blow up しないか)。

## 設計上の妥協 (変えない)

- **GC は Boehm-Demers-Weiser**。VALUE が 8-byte aligned ポインタ
  + 1-bit fixnum タグなので、conservative GC で安全に追える。
- **オブジェクトは順序保持 parallel array**。挿入 O(1)、lookup O(n)。
  jq の典型的な n では十分速い (B-5 で要件次第)。
- **string slice はコピー (buffer 共有しない)**。jq も同じ。
  pystro は共有してるが、jq との互換のためコピーで揃える。
- **実装は tree walk + emit buffer**。CPS 化の話は B-1 で扱う。

## 進捗

- v0 (2026-05-05): 全 7 項目 (design / JSON parser / filter parser /
  evaluator / build wiring / test suite / future-extension plan) 完了。
  test 0 → 338 (+`*.diff.test` 経由で system jq との差分も)。
  実装中に jq との差分テストが値モデル (`values` の意味) /
  string interp / try-stream / parse 細部のバグを 5 つ拾った
  (`done.md` 末尾参照)。
