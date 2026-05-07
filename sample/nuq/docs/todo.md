# todo.md — nuq 残作業

実装済みは [`done.md`](./done.md)、ベンチ結果は [`perf.md`](./perf.md)、
ランタイム解説は [`runtime.md`](./runtime.md)、言語仕様は
[`spec.md`](./spec.md)。

jq 公式テスト **524/526 (99.6%)** PASS。残る 2 件は decnum 必須で
原理的に通せない (下記 A-1)。それ以外は実用上ほぼ問題ない。

おおむねインパクト / 実装コスト順:

## A. 互換性

### A-1. decnum (任意精度 10 進数) — 大規模、優先度低
jq 公式テスト残 2 件 (#128 / #457) と複数の `try (.+ "x") catch .` 系で
影響。gmp ベースのバックエンドが必要。

- `9E+999999999` のような巨大指数を symbolic に保つ必要がある
- `1000000000000000002` (>2^53) の整数精度を保つ必要がある
- jq 本体も decnum ビルド必須

実装コスト大。値表現に bignum / decnum 型を追加し、parser / printer /
算術全箇所を分岐。`have_decnum` フラグで sniff されている部分は
既に jq の non-decnum branch を踏むので、互換上のニーズは限定的。

### A-2. 真の正規表現 (`match` / `capture` / `scan` / `sub` / `gsub`)
project memory `regexp_astrorge` の方針: `sample/astrogre` を完成させて
integrate。

- 現状 `test` / `gsub` / `sub` / `splits` は **substring 一致のみ**
  (literal 引数なら jq 互換)
- `match` / `capture` / `scan` は未実装

`astrogre` 側が integrate-ready になり次第つなぐ。

### A-3. `.[]` を含む代入 LHS
`(.foo[]) = X` で配列各要素に X を入れる、のような jq の機能。
`extract_path` を拡張するか、accessor に path_eval バリアントを追加。
今は `..` 経由 (`(.. | select(...)).foo |= ...`) で代用可能なので
優先度低い。

### A-4. `tostream` / `fromstream`
未実装。streaming JSON I/O を必要とするユースケースで。

## A-OOM. arena 内 dead 累積による peak memory blow-up

per-run arena は run 終了で wholesale reset するが、run 内で生まれた
dead intermediate を mid-run で回収する仕組みが無い。`map(F) | sort
| map(G) | ...` のような chain や、特に `reduce range(N) as $x ([]; .
+ [$x])` のような accumulating mutation で **peak memory が dead
intermediate の総和** になる。

実測 (input 9 byte の `reduce range(50000) as $i ([]; . + [$i]) |
length`):

| engine | peak RSS | elapsed |
|---|---:|---:|
| jq    |    5 MB  | 0.02 s |
| jaq   |    4 MB  | 0.01 s |
| gojq  |   47 MB  | 10.4 s |
| nuq   | 11313 MB | 11.8 s |

(計算結果は全エンジン一致 50000、メモリ使用量だけが破滅)

jq / jaq は refcount ベースの unique-ref 検出で `acc + [x]` を
**in-place push** に最適化、O(N) で済ます。nuq は arena で全コピー、
O(N²) アロケが reset まで居残り。

直し方の候補 (実装コスト順):

1. **handle-based VALUE access** + Cheney semispace copying GC: 各
   `struct nuq_obj *` raw ptr を pin する transient roots stack を
   導入。precise root tracking で safe な moving GC が実装可能。
   `nuq_op_add_slow` / `nuq_clone` / その他 helper を全部書き直す
   必要あり (中規模リファクタ)
2. **safe-point copying GC**: GC を NODE_DEF entry など raw ptr が
   無い時点でしか動かさない。helper 中の alloc は `gc_defer++` で
   GC 抑制。reduce/foreach/map の iter 境界に safe point 挿入
3. **refcount + COW**: 各 nuq_obj に refcount、`+`/`+=` 等の clone
   側で refcount=1 を見たら in-place mutation。jq の方式。VALUE
   assignment 全箇所で inc/dec が必要 (大規模)
4. **mark-sweep** (non-moving): bump alloc を捨てて size-class
   freelist + tracing GC。VALUE は不変、root tracking は (1) と同
   レベル

prototype で (1) は試したが、helper 中の raw ptr が allocator 経由で
GC を触ると stale になる問題で動作不能。停止解決まで保留。

## B. パフォーマンス

### B-1. streaming pipe
`f | g` は LHS 出力を一度 EMIT pool に集めてから RHS を回す。stack
discipline で巻き戻しはするが per-stage のスライスは確保される。
長 stream (`.[]` 経由の large array iteration) で memory 効率が
落ちる。CPS chain にすれば slice 化を消せる。

**注意**: これは alloc pattern の話で specialization の話ではない。
pipe は既に SD specializer が両側に入る形になっている。AOT が interp
を引き離す効果は無い (interp も同じ alloc を使う)。実用 100MB でも
問題が出ていないので優先度低い。

### B-2. EMIT pool の sub-call 巻き戻しのコスト
現在 `c->pool_top = top0` を毎 NODE_DEF 終端で書き戻す。stack discipline
は正しいが、pool_top の load/store が hot loop に立つ。per-call で
stack 上 (alloca) の固定 buffer + overflow spill にすれば、top の状態が
register 常駐になる。`pyramid` bench (現状 0.84×) に効く可能性。

### B-3. AST fusion 追加ルール
現行: 4 ルール + 右辺エッジ fusion (詳細 `runtime.md` § 7)。

未実装の候補:
- `[body] | unique` — 中間配列があっても hash で重複早期排除
- `[body] | sort_by(K)` — in-place sort + key 抽出の合体
- `range(N) as $x` の foreach パターンの単純化
- `{key: V} | .key` → `V` (path 抽出の peephole)
- `[body] | reverse` → reverse-emit-collect

健全性検証は意味保存が肝。jq 公式テスト + ローカル差分テストで常時
チェック。意味判断に迷う rewrite は入れない (副作用、エラータイミング、
multi-emit 順序、`and`/`or` 短絡を破らない)。

### B-4. PGO (型 feedback)
ASTro framework に部品 (`HOPT(n)` ハッシュ、`swap_dispatcher`、
`hopt_index.txt`、`#ifndef NODE_SKIP_COLD`、`-p / --pg-compile`
driver) は揃っていて luastro / naruby が実用してる。nuq には未配線。

ただし jq は集合演算が中心で AST level の hot loop が薄いので、PGO
よりも AST fusion の方が相性がいい — done.md / perf.md 参照。

## C. 内部整理

### C-1. side table の分離
`runtime.c` は eval helpers + side table が同居。`tables.c` 等に
切り出し。

### C-2. `def`-with-filter-args の cleanup
`def map(f): ...; map(.+1)` で `f` を 0-引数の pseudo-def として CTX
に登録するが、関数復帰時に **個別に pop していない** (`func_cnt` を
watermark で巻き戻す)。動くが脆い。proper "call frame" + 関数
スナップショット保存に。

### C-3. parser の重複
`parse_postfix` と `parse_term_for_keyword` で `.foo / .[i] / .[] /
.[a:b]` のロジックが両方にコピーされている。

### C-4. JSON parse 位置 tracking の精度
`fromjson` のエラーメッセージ位置情報。現状 jq 互換のために `'<x>'`
を見たら matching `'` の次まで position を進めるヒューリスティック
で動いているが、フォーマル化したい。

## D. 仕様 / 互換性 (細部)

### D-1. 数値整形の jq 互換
非常に大きい / 小さい double で jq と微妙に出力が変わる可能性あり。
要差分テスト。

### D-2. `@csv` の quoting
今は単純に `"` を `""` に置換。jq の処理は full RFC 4180 ではないが
微妙な差があり得る。

### D-3. エラーメッセージ format の網羅性
done.md に書いた「jq 互換の `at line ... column ...` 形式」は
fromjson 中心。他の builtin は terse なまま。jq の "Cannot iterate
over X (...)" 等の正確な表記に揃える余地。

## E. 拡張: JSON 以外を query する

### E-1. 新しい front-end parser
- **YAML / TOML**: そのまま `nuq_obj` に落とせる。`--input yaml` で
  parser を選ぶ仕組み
- **CSV / TSV**: ヘッダ行をオブジェクトキーに、後続行を 1 オブジェクト
  ずつ stream emit
- **XML**: 表現に選択肢が複数 (badgerfish / `{tag, attrs, children}`)
  要設計議論

### E-2. value 表現を抽象化
今は `struct nuq_obj` 直アクセス。`value_provider_t` の vtable 経由に
すれば、SQL のような行 + カラム表現や grep 風の行ストリームでも
nuq のフィルタ言語が使える。実装コスト大、恩恵が見えない限り
着手しない。

## F. テスト / ベンチ拡張

### F-1. fuzzing
クラッシャ潰し。

### F-2. ストリーム入力ベンチ
今は `n` を stdin で渡すだけ / file を読むだけ。`-R` 1 行ずつ /
NDJSON / `inputs` ストリームの bench を整備したい。

### F-3. 大規模 NDJSON ベンチ
`qj` (大規模 NDJSON 寄せの実装) を見つけ次第追加して比較。

## 設計上の妥協 (変えない)

- **GC は Boehm-Demers-Weiser**。VALUE が 8-byte aligned ポインタ +
  1-bit fixnum タグで conservative GC で安全
- **オブジェクトは順序保持** (parallel array で挿入順、+ lazy hash
  idx で lookup O(1))
- **string slice はコピー** (jq に揃える、buffer 共有はしない)
- **値表現は IEEE-754 double + 62-bit fixnum** (decnum 未対応)
