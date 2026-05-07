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

### A-3. `tostream` / `fromstream`
未実装。streaming JSON I/O を必要とするユースケースで。

## B. パフォーマンス

### B-1. 線形性解析の拡張

現状 `linearity.c` は `node_add(node_identity, RHS)` → `node_add_inplace`
の書換えのみ。これを以下にも広げると効く:

- **string concat の in-place push**: `reduce range(N) as $i (""; . + "x")`
  が現状 O(N²)。string buffer の `realloc` ベースの growable 拡張が必要
  (今は `nuq_make_string` が固定サイズ buffer を combined alloc する設計)
- **object merge の in-place**: `reduce E as $k ({}; . + {$k: V})` 系。
  array とほぼ同じ条件でいける
- **`|=` 経由の append**: `.acc |= . + [$x]` も `acc + [$x]` と同じ
  pattern なので、線形性が判定できれば in-place mutation 可能
- **user-def 経由の linearity propagation**: 現在 user-def の call は
  scope ごと UNKNOWN で bail。def body も解析して、linear-在 / linear-not
  を伝播すれば `def push($i): . + [$i]; reduce ... as $i ([]; push($i))`
  のような書き方も最適化可能

### B-2. streaming pipe
`f | g` は LHS 出力を一度 EMIT pool に集めてから RHS を回す。stack
discipline で巻き戻しはするが per-stage のスライスは確保される。
長 stream (`.[]` 経由の large array iteration) で memory 効率が
落ちる。CPS chain にすれば slice 化を消せる。

**注意**: これは alloc pattern の話で specialization の話ではない。
pipe は既に SD specializer が両側に入る形になっている。AOT が interp
を引き離す効果は無い (interp も同じ alloc を使う)。実用 100MB でも
問題が出ていないので優先度低い。

### B-3. EMIT pool の sub-call 巻き戻しのコスト
現在 `c->pool_top = top0` を毎 NODE_DEF 終端で書き戻す。stack discipline
は正しいが、`pool_top` の load/store が hot loop に立つ。per-call で
stack 上 (alloca) の固定 buffer + overflow spill にすれば、top の状態が
register 常駐になる。`pyramid` bench (現状 0.92× vs jq) に効く可能性。

### B-4. AST fusion 追加ルール
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

### B-5. PGO (型 feedback)
ASTro framework に部品 (`HOPT(n)` ハッシュ、`swap_dispatcher`、
`hopt_index.txt`、`#ifndef NODE_SKIP_COLD`、`-p / --pg-compile`
driver) は揃っていて luastro / naruby が実用してる。nuq には未配線。

ただし jq は集合演算が中心で AST level の hot loop が薄いので、PGO
よりも AST fusion / 線形性解析の方が相性がいい — done.md / perf.md 参照。

`tree_paths` (現在 jq の 0.18×、唯一の big-data 大敗) のような
path-walk 系は型 feedback でなく allocator 周りの改善 (path-array の
arena 配置、prefix sharing) の方が効きそう。

## C. 内部整理

### C-1. side table の分離
`runtime.c` は eval helpers + side table (lit / interp / obj / args /
def / pat / pat_alt / fmt / user_args) が同居。`tables.c` 等に
切り出し。

### C-2. `def`-with-filter-args の cleanup
`def map(f): ...; map(.+1)` で `f` を 0-引数の pseudo-def として CTX
に登録するが、関数復帰時に **個別に pop していない** (`func_cnt` を
watermark で巻き戻す)。動くが脆い。proper "call frame" + 関数
スナップショット保存に。**注**: 関連して、`nuq_defs_eval` の fd cache
を入れた (per-input leak 修正の副産物) ので、再 entry 時の整合性が
若干複雑。リファクタするならこの cache も整理対象。

### C-3. parser の重複
`parse_postfix` と `parse_term_for_keyword` で `.foo / .[i] / .[] /
.[a:b]` のロジックが両方にコピーされている。

### C-4. JSON parse 位置 tracking の精度
`fromjson` のエラーメッセージ位置情報。現状 jq 互換のために `'<x>'`
を見たら matching `'` の次まで position を進めるヒューリスティック
で動いているが、フォーマル化したい。

### C-5. `GC_malloc` 名前残置
libgc 依存除去後も `GC_malloc` / `GC_malloc_atomic` / `GC_realloc` という
名前のマクロが context.h に残っていて、`calloc(1, sz)` / `realloc` への
リダイレクトになっている。段階的に明示 `malloc` / `calloc` / `realloc` に
書き換えると分かりやすい (機能差なし、ただし zero-init を期待する箇所
には `calloc` を使うこと)。

### C-6. parser の `strdup_n` 一回限り leak
parse 時に lexer が token を `strdup_n` で複製して `tok.s` に保存、
次の token で上書きされて漏れる (~2 KB / parse、process 終了で OS
回収)。intern 後に free するとよい。impact 小、優先度低。

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

### F-4. valgrind を CI に組込み
`make valgrind` を作って、代表的な workload (real bench の小さい
サブセット + Q3 reduce + path-walk) を valgrind の memcheck 配下で
通す。memory access errors / per-input leak が回帰したら検出できる。

## 設計上の妥協 (変えない)

- **メモリ管理は per-run arena + Cheney copying GC + scratch arena**:
  - 永続領域 (AST / リテラル / `--arg*` / module data / CTX / intern):
    plain `malloc` / `calloc` でプロセス終了まで保持
  - 中間 VALUE: per-run arena に bump alloc → 16 MB しきい値で minor GC
    (Cheney semispace) → run 終了で wholesale reset
  - NODE_DEF / runtime のスナップショットバッファ (snapshot ターナリ
    の heap 分岐): scratch arena に bump alloc → run 終了で reset
  - **外部 GC ライブラリ依存なし** (libgc 不要、`libm` + `libc` のみ)
- **線形性解析で `acc + [$i]` を in-place mutation に降格**: parse 後の
  AST static 解析で「単一 dot 消費」を判定。`acc + [$i]` reduce が
  O(N²) → O(N) に
- **オブジェクトは順序保持** (parallel array で挿入順、+ lazy hash
  idx で lookup O(1))
- **string slice はコピー** (jq に揃える、buffer 共有はしない)
- **値表現は IEEE-754 double + 62-bit fixnum** (decnum 未対応)
