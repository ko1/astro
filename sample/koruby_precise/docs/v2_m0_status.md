# v2 M0 実装状況 (2026-06-13)

> ⚠ **M0 時点のスナップショット**。以降の実装で変わった値がある — 例えば下の
> 「値表現」は当時 nil=0 / false=2 / true=4 / Symbol `(id<<3)|6` だったが、現行は
> nil=0 / **false=4 / true=20 / undef=36**、Symbol は **`(id<<4)|0xC`** (`context.h` が正)。
> 現状の機能一覧は [done.md](./done.md) を見ること。

[v2_design.md](./v2_design.md) / [v2_spec.md](./v2_spec.md) だけを実装根拠に
ゼロから書いた M0 の記録。v1 系譜のコード持ち込みなし (設計どおり)。

## 実装済み (M0 スコープ)

- **値表現**: nil=0 / false=2 / true=4 / fixnum LSB=1 / Symbol `(id<<3)|6` /
  heap = 8-aligned ptr。nil=0 なのでゼロ初期化ページ・ゼロクリアが
  そのまま有効値 (truthiness は `(v|2)!=2` の 1 命令)
- **slots ABI**: cursor 常に top、staged @child は負 offset、
  `c->slots_top` の書き込みは `korb_alloc` のみ。8 MiB MAP_NORESERVE +
  guard page、`KORUBY_SLOTS_BYTES` で可変。frame push で slots limit +
  machine-stack probe (C スタック再帰が先に尽きるため) → SystemStackError
- **VALUE_REF / VALUE_SLICE**: `runtime/astro_ref_template.h` (値型名から
  生成、v2_design §5.3)。builtin ABI は `(CTX*, VALUE *slots, VALUE_SLICE)`
- **RESULT**: NORMAL / RETURN / RAISE + UNWRAP/CHECK。c->state / errinfo
  なし。例外は RESULT.value で運搬、unwind 中の backtrace 蓄積は libc のみ
  (unwind 経路に GC 点なし)
- **@child 二形態** (koruby_gen.rb): `VALUE_REF x@child` = staged
  (staging = rooting)、`VALUE x@child` = register (最後の child のみ、
  生成器が強制)。DISPATCH / SD 両方で同型を生成
- **lvar 一本モデル** (§7.8 推奨側): `off = index - locals_cnt - chain` を
  parse 時 bake。call の staged 引数窓 = callee param slots (コピーなし)
- **言語**: Integer/String/Symbol/true/false/nil リテラル、算術
  (floor div/mod)、比較、&& || ! 単項-、lvar (+= 等含む)、
  if/unless/elsif/三項/modifier、while/until、def + 位置引数 (0..3 引数)、
  puts/p/print (可変長 builtin)、String の + * == < 系
- **例外**: ZeroDivisionError / TypeError / ArgumentError / NoMethodError /
  SystemStackError / NotImplementedError → CRuby 4.0 形式 stderr + exit 1
  (`file:line:in 'meth': msg (Class)` + from 行、深い unwind は中略)
- **未対応構文**: parse 時 exit ではなく `node_unsupported` (実行時
  NotImplementedError) に落とす — rubyharness がその行まで採点できる

## ゲート (v2_spec §6 M0)

- GC=copy (moving) default で **STRESS / STRESS+PURGE green**
  (fib / 文字列 churn / 再帰 / CAT=integer スイープが非 STRESS と同 PASS 数)
- **AOT**: `--aot-compile` (実行 + 終了時 bake) → cached SD swap 実行
  (swap 数を -v で表示、`ASTRO_AOT_STRICT` で swap=0 を exit 3 に) →
  `--plain`。bare `--aot-compile` が bake しない v1 型バグなし
- **rubyharness**: make test 完走 (crash 0、PASS はサブセット幅相応)、
  make bench 5 モード完走。fib: interp 0.27s / aot+cached 0.13s /
  cruby 0.56s。method body は code_repo 経由で各自 AOT entry
  (runtime dispatch 経由のため)
- **GC backend switch**: `make GC=mark` / `GC=bump` ビルド・動作確認
  (marker file で再 link 保証)

## 未着手 / M0 で意図的に落としたもの

- **§7.8 一本 vs 二本の実測比較**・**R2 (depth 焼き込みで SD dedup 減) の
  計測** — 一本のみ実装。比較計測は M0 出口条件の残件
- **make codeql** (§10 の CodeQL 3 クエリ) と **audit build**
  (`ASTRO_REF_CHECK` hook は template にあるが検査実体なし)
- **VALUE_SLICE @children** (可変長 staging)。call は 0..3 引数の固定
  arity ノード。4+ 引数 / splat は M1 (§13 #4/#8)
- **Float** (後回し可)、Bignum (overflow は NotImplementedError)、
  受信者メソッド呼び出し (`x.even?` 等)、block/Proc/closure (M0 対象外)
- `--pg-compile` は AOT と同じ bake (PGSD/HOPT は M1)、`--build` 未対応
- メソッド表・シンボル表は線形探索 (M0 規模では非ボトルネック)
- 複数 `-e`、Ruby レベル ARGV

## 運用メモ

- sandbox 環境で AOT bake の make が ccache で落ちる場合は
  `CCACHE_DISABLE=1` (docs/code_store_quirks.md の既知問題)
- 性能の数値は計測条件込みで perf.md を立ててから記録する
  (このファイルの数値は配線確認レベルのスモーク値)
