# 即値埋め込み (copy-and-patch 風ローダ) の検討 — 2026-09-06

## 動機

AOT の SD は木の形 (hash) ごとに 1 つを共有し、site 固有の値 — メソッド名 ID
(`mid@sym`)、行番号、Symbol リテラル、inline cache のアドレス、子ノードのアドレス —
は実行時に NODE から読む (`n->u.node_call.mid` / `n->u.node_plus.lhs->u.node_ivar_get.ic`
…)。これは code store を intern 順に依存させない (順序非依存、1b327c48) ための設計だが、
実行時には次のコストになる:

- **AST の pointer chase**: 葉の ic に届くまで深さぶんの依存ロード
  (`@x = @x + 3` で 4 ノード = 4〜6 cache line)。L1d miss の割り付けで、call SD の miss の
  ~25% が NODE/ic の行、~30% が korb_method/body head (後者は fat ic で解消済み)。
- **hit 経路で使わない値の先読み**: 0 引数 call の SD 先頭で `line` と `mid` を無条件に
  ロードし callee-saved に抱える (RAISE 時の `e->line = line` で必要なため gcc は沈めない)。
  「miss 側で node から読み直す」書き換えでは命令数は 1 つも減らなかった (実測)。

## 上限の実測 (実験パッチ `trials/2026-09-06-koruby-precise-perf/patches/05-bake-syms-experiment.diff`)

koruby_gen.rb に実験用フラグ (`KORUBY_BAKE_SYMS=1`) を足し、`mid` / `line` / Symbol リテラルを **hash に含めて
SD に即値として焼く** (同一プロセスで bake → run するときだけ正しい。順序非依存性は
壊れるので実験専用)。optcarrot 180 frames AOT、ローカル `perf stat -r 3`:

| | round 3 (共有 SD) | 即値 bake |
|---|---:|---:|
| 命令数 | 21.53G | **20.23G (−6.0%)** |
| L1d miss | 336M | **261M (−22%)** |
| L1i miss | 1.4M | 13.9M (10×) |
| SD 数 / all.so | 502 / 3.32 MB | 535 / 3.86 MB |
| bake 時間 | 34 s | 93 s |

即値化は D 側に大きく効くが、site ごとにコードが複製されて I 側が悪化する。
net は sp4 の時間で判定した (round 3 vs 即値 bake、3 round 交互、YJIT 294〜299 で不変):

| round | round 3 | 即値 bake |
|---|---:|---:|
| 1 | 176.7 | **190.3** |
| 2 | 171.1 | **190.5** |
| 3 | 170.9 | **189.3** |

**+10.6%** (172.9 → 190.0)。I キャッシュの悪化より D 側と命令数の削減が勝つ。

hash に ID / 行番号 / Symbol 値を含めるので **自己検証**になる: intern 順が bake 時と
違う node は hash が一致せず SD が見つからないだけ (interp に落ちる、`--compiled-only`
なら poison)。誤った即値で走る経路は無い。preload_store (prelude) も、boot 時の intern と
prelude parse は user code より前で決定的なので同じ binary なら一致する。
`--build` (wasm 埋め込み) は起動時に re-intern するので、codegen フラグを切って従来の
runtime 参照で焼く。

注意: この実験は ic / 子ノードのアドレスまでは焼いていない (プロセスごとに変わる)。
copy-and-patch ならそれも即値にできるので、上限はもう少し上。

## 実装案

### A. 本物の copy-and-patch (ELF relocation を穴にする)

- SD の C ソースで site 固有値を `extern` シンボル経由で参照する
  (`extern const char HOLE_mid[]; mid = (uint32_t)(uintptr_t)HOLE_mid` 等)。
  gcc は再配置エントリ (`R_X86_64_32S` / `R_X86_64_64`) を残すので、それが穴になる。
- ローダは SD の `.text` を **node インスタンスごとに** 実行可能領域へコピーし、
  再配置を実際の値 (mid、line、`&n->u.X.ic`、子 NODE* …) で解決して
  `head.dispatcher` に据える。ランタイム関数への `call` は `-fno-plt` +
  `R_X86_64_PLT32`/`PC32` を絶対アドレスの trampoline に向けるか、`-mcmodel=large`。
- 利点: 共有 SD (`.so`) はそのまま「テンプレート」として残るので順序非依存性を保てる。
  ic/子ノードのアドレスも即値になり、AST の pointer chase が消える。
- コスト: x86-64 専用の再配置適用器 (数百行)、SD ごとの relocation 表の抽出
  (`.o` を読む: `astro_cs_build` が `o/` を持っているので入手可能)、
  インスタンス数ぶんのコード (I キャッシュ) — hot な body だけに限る仕組み
  (PG 情報か呼び出し回数) が要る。wasm には使えない。

### B. プログラム単位の bake (穴なし) — **実験のみ (ツリーには入れない)**

- `mid` / `line` / Symbol リテラルを hash に含めて即値で焼く。検証表は要らない: hash が
  一致した SD だけが使われるので、intern 順がずれた node は miss するだけ。
- intern 順は boot → prelude → parse 順で決定的なので preload_store も一致する。
- `--build` は bake 時の symbol 表を emit し `korb_ctx_new_seeded` で seed
  (boot の intern 順が bake プロセスと違うため。実測: `__zlib_crc32` 236 vs 1071)。
  埋め込み exe の出力一致・命令数 3.56G (interp 11.9G) を確認。
- 残る欠点: ic / 子ノードのアドレスは焼けないので pointer chase は残る。

### C. 中間: NODE 表のインデックスを焼く

- 子ノード・ic を「アドレス」ではなく、その body の NODE 配列内 index で参照する
  (`base[K].u.X.ic`)。K は parse ごとに決定的なので焼ける。依存ロードが深さ d から 1 に
  なる。順序非依存性は保てるが、生成器 (astrogen) の子参照の出し方を変える必要がある。

## 判断

(framework 側の設計は `docs/idea_code_store.md` §7: 穴の抽象、dlopen+初期化コード (pool) 経路と
ローダ経路の両対応。)

B は +10.6% だが、**SD を 1 プログラムの intern 順に鍵づけする**ので、別プログラムの
同じ形のノードが SD を共有できなくなる (user 指摘: 「同じプログラムだけじゃない」)。
`line` を含めた分は同一プログラム内の共有も減る。ASTro の Reusable の原則に反するので
ツリーには入れず (tag `perf-send-cache-bake-experiment`、trials の patch)、上限の参照値として残す。
本命は A: 共有テンプレートはそのまま、ローダがインスタンスごとにコピーして穴
(mid / line / ic / 子 NODE*) を埋める。

## 相対評価: CRuby / YJIT / interp / AOT を同一ラウンドで (2026-09-06)

同じ機械 (sp4)・同じ optcarrot・同じ ROM・同じ 180 frames を、**1 つの時間窓で 6 構成を交互に**
best-of-3。全セル checksum 59662 (= 同じ絵を出している)。CRuby / YJIT は `bin/optcarrot`、
koruby は require 不要 bundle (koruby 専用の File shim を含むので CRuby では動かない) 経由。

| 構成 | fps (3 round 中央値) | 全 round | CRuby 比 | YJIT 比 |
|---|---:|---|---:|---:|
| CRuby 4.1.0dev (JIT 無し) | 62.6 | 62.0 / 62.6 / 62.7 | 1.00× | 0.21× |
| CRuby + YJIT | 301.3 | 299.2 / 301.3 / 302.4 | **4.82×** | 1.00× |
| koruby **interp** (`--plain`) | 72.2 | 72.7 / 72.2 / 72.0 | 1.15× | 0.24× |
| koruby **AOT** master `0ffc4c0f` | 173.2 | 173.0 / 180.4 / 173.2 | 2.77× | 0.57× |
| koruby **AOT** + P (pool) | 208.8 | 208.3 / 209.4 / 208.8 | 3.34× | 0.69× |
| koruby **AOT** + P + L (loader) | **227.2** | 227.2 / 226.4 / 227.3 | **3.63×** | **0.75×** |

読み方: この 2 経路は「木を歩くインタプリタ」ではなく **AOT でコンパイル済みのコード**の話で、
1 日で YJIT 比 0.57× → **0.75×**、CRuby 比 2.77× → **3.63×** に動いた。interp (1.15×) と AOT (3.63×)
の差 3.1× が ASTro の部分評価そのものの効き。

**「compiled である」ことの担保** (trials `logs/rel/compiled-evidence.txt`):

- 3 本とも `--compiled-only` で実行している。このモードは **未特殊化 body に 1 回でも
  インタプリタ dispatch が来た時点で abort (exit 7)** するので、AOT の行にインタプリタ実行が
  混ざることはない (混ざれば数字ではなくエラーになる)。
- 起動時の swap 実数: `swapped 1983 dispatchers (program + 2008 method bodies)` — program root と
  全メソッド body が baked SD に差し替わっている。
- L の行はさらに `29/2027 bodies instantiated (233 KB), 0 failed` = hot 判定で選ばれた 29 body が
  コピー＋即値 patch された実体。
- interp 行 (`--plain`) は user code を木で歩く。ただし固定 prelude は baked SD のまま
  (prelude は preload.so の SD)、つまり 72.2 fps は**インタプリタ側に有利**な値。

## 結果: 経路 P (穴 + pool、2026-09-06、`docs/idea_code_store.md` §7.7)

SD は site 固有値を NODE から読まず、SD インスタンスの穴の表 `n->head.pool` (`astro_hole_t[]`、
load 時に生成 `SD_<h>_fill` が埋める) から `P[k]` で読む。inline SD には `P + off` を隠し引数で渡す。
hash・呼び出し規約・SD 数 (502) は不変、all.so は 3.32 → 3.88 MB (`_fill` のぶん)。

sp4 (`trials/.../logs/p2/optcarrot-ab.txt`, master `0ffc4c0f` vs pool, 3 round 交互):

| round | master | pool | CRuby+YJIT |
|---|---:|---:|---:|
| 1 | 171.7 | **205.7** | 295.6 / 298.4 |
| 2 | 172.4 | **204.7** | 297.7 / 297.6 |
| 3 | 171.9 | **206.4** | 299.8 / 297.6 |

**+19.7%** (171.9 → 205.7、中央値)。checksum 全セル 59662。CRuby 比 3.4×、YJIT 比 0.69×。
即値 bake 実験 (+10.6%) を超えた: ic / 子 NODE* まで表に載り、SD 複製 (I キャッシュ 10×) が無い。

ローカル `perf stat -r 3` (optcarrot 180f `--compiled-only`):

| | master | pool | pool + `-fno-tree-slp-vectorize` |
|---|---:|---:|---:|
| 命令数 | 21.52G | 21.34G (−0.9%) | 21.29G (−1.1%) |
| cycles | 6.29G | 5.48G (−13%) | 5.44G (−13.5%) |
| L1d miss | 396M | **148M (−63%)** | 148M |
| L1i miss | 2.02M | 1.72M | 1.69M |

命令数がほぼ不変で L1d miss が 6 割減 = 設計どおり「依存ロードの深さと NODE 行の散らばり」が消えた。

罠 (pool 版だけ nested_loop が AOT 3.57× 遅かった): SD の `-O3` で SLP ベクトル化が staged
slot への scalar store 2 本 (`movq -0x20(%rbx)` / `-0x18`) を直後の 16B load (`vmovdqu`) にまとめ、
store-forwarding が失敗してループが 3× (cycles 465M → 1398M、命令数は減っている)。
`-fno-tree-slp-vectorize` で 388M (master の 465M より速い)。master も同フラグで 424M (−9%)。
optcarrot には中立 (5.48 → 5.44G)。koruby の SD CFLAGS に入れた (`main.c` `koruby_extra_cflags`)。

microbench 53 本 (aot+cached pool/master, `logs/p2/compare.md`): 15 本 ≥3% 速く、4 本 ≥3% 遅い。
速: send 0.56 / fannkuch 0.73 / hashiter 0.89 / while 0.89 / intdiv 0.92 / nbody 0.92 / bitops 0.94 /
exception 0.94 / strfmt 0.94。遅: casewhen 1.09 (ローカル perf stat: 命令 +1.2% / cycles +6.5%、
フラグは無関係 = pool の per-entry コスト) / aryidx 1.05 (19→20 ms) / fib 1.04 (ローカル cycles 同値) /
ivar 1.04。

ゲート (ローカル): corpus 4901/1 FAIL (既存)、STRESS+PURGE 4899/3/0 CRASH (既存と同一)、
rubyspec 6 ディレクトリ (language / core/kernel / array / module / proc / basicobject) を HEAD binary と
同時刻に実 mspec で比較して pass/fail/err 完全一致、`--build` 埋め込み exe 出力一致 (1127 `_pool`
シンボル)。wasm: pool 版 SD .c 1126/1126 が wasm32 clang でコンパイル可。ただし master 時点で
ホスト側 `korb_runtime.o` が WASI 未対応シンボル (`pwd.h` / `chroot` / `fchdir` / `tzset` /
`LONG_MAX` / `CLOCK_*_CPUTIME_ID`) で落ちるため .wasm の実行確認は未 (P とは無関係)。

## 結果: 経路 L (ローダ、spike、2026-09-06、`docs/idea_code_store.md` §7.7)

`runtime/astro_loader.c`。bake 時 `ASTRO_CS_PATCH=1` で store に `op/` (`-fno-pic -mcmodel=medium
-DASTRO_SD_PATCH` の .o、穴 = `_astro_hole_base + k` への R_X86_64_64) を作り、`KORUBY_INSTANTIATE=hot`
で起動直後の計数トランポリン (N dispatch、既定 200000) 後に `KORUBY_HOT_RATIO` (既定 0.005) 以上の
body だけ `.text/.rodata` をコピーして穴を即値に patch (`=all` は全 body)。既定 off。

sp4 (`trials/.../logs/l2/`, pool `fbd6fa31` vs pool+loader hot 0.005 / 5M, 3 round 交互):

| round | pool | pool + loader (hot) | CRuby+YJIT |
|---|---:|---:|---:|
| 1 | 208.5 | **223.9** | 299.6 / 297.4 |
| 2 | 206.2 | **224.6** | 296.7 / 296.1 |
| 3 | 207.3 | **223.3** | 296.7 / 292.7 |

**+8.2%** (207.3 → 223.9)。master からの累計 171.9 → 223.9 (**+30%**)、CRuby 比 3.7×、YJIT 比 0.75×。
optcarrot では 29/2027 body (233 KB) がインスタンス化される (ROM 読込が先頭 2M dispatch を占めるので
窓は 5M)。microbench 53 (hot:200000 窓, `logs/l2/compare.md`): 6 本 ≥3% 速 (exception 0.90 / gcd 0.92 /
ackermann・binary_trees・tak 0.95)、4 本 遅 (poly 1.22 = 二峰性 / aryidx 1.05 / casewhen・nbody 1.04)。

初版はチャンクごとに `mprotect` して RX にしていたため 4KB 粒度で、同じ構成が +4.6% だった
(`logs/l1/`)。arena を memfd の二重マップ (RW ビュー + 低位 2GB の RX ビュー) にして 16B 詰めに
したら **+8.2%** に伸びた: 使用量は all で 20.0 → 15.4 MB、hot で 308 → 233 KB だが、効いたのは
バイト数より密度 (i-cache / i-TLB)。ELF 側が 4KB を要求している箇所は無い (`.o` に PT_LOAD は無く、
`sh_addralign` は最大 32 = `.rodata.cst32`)。

機構 (ローカル `perf stat`、他セッションの負荷ありで命令数のみ信頼):

| optcarrot | pool | loader all (2027 体, 20 MB) | loader hot 0.005 (29 体, 308 KB) |
|---|---:|---:|---:|
| 命令数 | 21.29G | 19.63G (−7.8%) | 20.04G (−5.9%) |
| L1d miss | 141M | 71M (−50%) | 79M (−49%) |
| L1i miss | 2.2M | 19.8M (9×) | 13.0M (6×) |
| cycles | 5.52G | 6.22G (+12.7%) | (負荷で比較不能; sp4 で +4.6%) |

- 即値化は D 側にさらに効く (命令 −6〜8%、L1d miss 半減) が、インスタンス化は同形 body の SD 共有
  (I キャッシュ共用) を失うので `all` は net 負。hot 限定で正になる。
- ループ内の穴は `movabs` を毎回再マテリアライズする (asm 即値はループ外へ出ない) ので、pool の
  `P[k]` (L1 hit) と命令数は同じ。効くのは D miss の多い大きな body だけで、nested_loop 級の
  小ループでは 10B 命令ぶんコードが太るだけ (ローカル 2.13G → 2.35G 命令)。
- 罠 1: 穴を素の C 式 `base + k` にすると gcc が addressing の displacement に割ったり
  `lea -0x54(%r12)` で別の穴から導出する → 即値を独立に patch できない。`asm("movabsq $%p1,%0" ::
  "i"(P+k))` で不透明化 (patch モードは inline SD を always_inline)。
- 罠 2: hot finish で entry 配列を qsort すると、生きているトランポリンの entry ポインタが別 body を
  指し nil の `<<` で落ちる。orig を先に読み index 配列を sort。
- ゲート: corpus 4901/1 (loader ビルド、既定 off)、`all` / `hot` で 17 bench + optcarrot の出力が pool と
  一致、インスタンス化失敗 0 (2027 体)。
- 罠 3: `runtime/astro_loader.c` が koruby の Makefile の依存に無く、**境界チェックの commit が
  一度もコンパイルされずに「通って」いた**。依存を足して焼き直したら、その境界チェック自体に
  (a) 4 byte 書く再配置にも 8 byte の余裕を要求、(b) 消費側は SPECIALIZE を通らないので
  `head.nholes` が 0 のまま = 穴 addend 検査が全滅、という 2 つのバグがあった (optcarrot で
  1889/1983 失敗)。`astro_cs_pool_attach` が穴数を `head.nholes` に載せるようにして解決。

残り: `-fno-plt` の GOT 間接 call を直接 call にする (チャンクをホストの ±2GB に置く)、u32 穴を
`movl $imm32` (5B) にする、hot 判定を PG count に置き換える、`--build` / wasm は P のまま。

## テスト (2026-09-06)

詳細と全ログは trials `2026-09-06-koruby-precise-perf/` の「テスト」節 (`logs/tests/`)。要点:

- **全サンプル sweep** (29 サンプルをブランチと master worktree で直列ビルド + 各 test):
  `astro_spec_dedup_has` の引数を増やしたせいで naruby / baruby / baruby_precise が
  **ビルド不能**になっていたのを検出 → 1 引数に戻して修正 (`c26a6bc6`)。以後は
  ブランチと master が全サンプルで一致 (残る失敗は両側同一 = 既存)。
- **コーパス**: 4901/1 (既存)、STRESS+PURGE 4899/3/0 CRASH (既存と同一)。
- **AOT 差分**: 513 本を 1 つの共有ストア (1721 SD) に焼き、interp / AOT / loader all /
  loader hot の 4 モードで実行 → **511/513 が完全一致**、残り 2 件は master でも同挙動。
- **rubyspec 20 ディレクトリ** (約 15,700 examples) を master binary / ブランチ /
  ブランチ+loader で同一窓比較 → **20/20 完全一致**。
- **ローダ異常系 10 ケース** (op/ 削除・truncate・ヘッダ破壊・ビット反転・別 SD の .o・
  ratio 極値・GC STRESS): すべて出力一致、失敗は件数報告して pool にフォールバック。
- **ストア再利用**: A で焼いたストアで B が動き (出力一致・ストア不変)、逆も可。
  `--build` 埋め込み exe も出力一致 (`SD_*_pool` 1126)。
- **アプリ**: optcarrot 59662 / DOOM 17930386881013214317 / rubyboy 4747678158831331132 が
  CRuby・interp・AOT・loader(all/hot) で全一致。
