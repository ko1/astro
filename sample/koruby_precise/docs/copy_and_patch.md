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
