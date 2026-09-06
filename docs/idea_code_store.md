# ASTro Code Store

特化コードの保存・ロードを統一的に扱うランタイムライブラリ。`runtime/astro_code_store.{h,c}` として提供。
「ハッシュでコードを一意に特定する」という核心アイデア ([idea.md](./idea.md) §2.4–2.5) の実装基盤であり、
AOT / PG / JIT ([idea_jit.md](./idea_jit.md)) すべてがこの上に乗る。

## 1. 設計方針

- **保存単位**: エントリノード（関数定義のボディ等）のサブツリー丸ごと。1エントリにつき1つの `.c` ファイル
- **エントリの選択**: 言語側が決める（メソッド単位、トップレベルスクリプト等）
- **保存形式**: C ソース + コンパイル済み共有オブジェクト (`.so`)
- **ロード方式**: `all.so`（全エントリをまとめたもの）を `dlopen` + `dlsym("SD_<hash>")`

## 2. API

実体は `runtime/astro_code_store.h`。

```c
// 初期化: store_dir/all.so があれば dlopen 済みにする。
//   src_dir : node.h / node_eval.c 等の場所 (生成 .c が #include する)
//   version : ホストバイナリの mtime 等。前回保存時と異なれば store を破棄
void astro_cs_init(const char *store_dir, const char *src_dir, uint64_t version);

// hash 検索 → ヒットすれば dispatcher を差し替え true。
//   file != NULL : PGC ルックアップ (Hopt) を先に試す
//   file == NULL : AOT のみ (Horg)
bool astro_cs_load(NODE *n, const char *file);

// 特化 C ソースを生成。
//   file == NULL : AOT — store_dir/c/SD_<Horg>.c
//   file != NULL : PGC — store_dir/c/SD_<Hopt>.c + hopt_index.txt 追記
void astro_cs_compile(NODE *entry, const char *file);

// store_dir 配下の全 .c を make -j → all.tmp.so → all.so atomic rename。
//   extra_cflags : Ruby 等の追加 -I/-D が必要なら渡す。NULL でも可
void astro_cs_build(const char *extra_cflags);

// build 後に呼ぶと all.<N>.so を新世代パスで dlopen し直す
// (dlopen のパス名キャッシュ回避; code_store_quirks.md 罠 1 参照)
void astro_cs_reload(void);

// 診断: 特化済みノードの dispatcher を objdump で逆アセンブル表示
void astro_cs_disasm(NODE *n);
```

## 3. 利用フロー

```
[1回目の実行]
  astro_cs_init("code_store", ".", VERSION)   all.so がない → load は miss
  ALLOC → astro_cs_load (miss)
  ... 実行 ...
  astro_cs_compile(entry, NULL)                code_store/c/SD_<Horg>.c 生成
  astro_cs_build(NULL)                         全 .c → .o → all.so
  astro_cs_reload()                            新世代 all.<N>.so を dlopen
  astro_cs_load(entry, NULL)                   hit → dispatcher 差し替え

[2回目の実行]
  astro_cs_init(...)                           前回の all.so をそのまま再利用
  ALLOC → astro_cs_load (hit) → 高速実行
```

## 4. モード別の使い分け

| モード | compile | build | load |
|--------|---------|-------|------|
| AOT | 実行後にオフライン | 同左 | 次回起動時 |
| PG | 1回目実行後 (Hopt 込み) | 同左 | 2回目起動時 (file 引数あり) |
| JIT | 実行中に非同期 | バックグラウンドで適宜 | コンパイル完了時に reload |

JIT の場合は `all.so` に加え、新規コンパイル分を個別 `.so` として `dlopen` することも可能。

## 5. ファイル構成

```
code_store/
  c/SD_<hash1>.c     ← 特化 C ソース
  c/SD_<hash2>.c
  o/SD_<hash1>.o     ← コンパイル済みオブジェクト
  o/SD_<hash2>.o
  all.so             ← 全 .o をまとめた共有オブジェクト (atomic mv で更新)
  all.<N>.so         ← reload 用の世代別ハードリンク
  Makefile           ← astro_cs_build が自動生成
  hopt_index.txt     ← PGC: (Horg, file, line) → Hopt のマップ
```

各 `.c` ファイルはサブツリーの特化コードを自己完結で含む。共通する部分木は複数ファイルに重複するが、`static` 関数のためリンク問題は起きない。LTO で重複を最適化することも可能。

dlopen / リンクまわりの罠と暫定対処は [`code_store_quirks.md`](./code_store_quirks.md) を参照。

## 6. 言語側の統合

calc など最小サンプルの REPL は、入力ごとにこう呼ぶ:

```c
NODE *ast = parse(line);
if (!ast->head.flags.is_specialized) {
    astro_cs_compile(ast, NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
    astro_cs_load(ast, NULL);
}
EVAL(c, ast);
```

JIT を持つサンプル (naruby) では、ホットノード検出時に L0 スレッド経由で
`astro_cs_compile` → `astro_cs_build` → `astro_cs_reload` をバックグラウンドで実行し、
完了後に `astro_cs_load` を呼ぶ。code store はハッシュ→dispatcher マップの管理と
`.so` のロードに専念し、「何をエントリとするか」「いつコンパイルするか」の
ポリシーは言語側に委ねる。

## 7. 穴 (hole) とインスタンス化 — dlopen 経路とローダ経路 (設計、2026-09-06)

### 7.1 動機

SD は木の形 (hash) ごとに 1 つを共有するので、サイト固有の値 — メソッド名 ID、行番号、
Symbol リテラル、inline cache のアドレス、子 NODE のアドレス — は SD の中で NODE から
読む (`n->u.node_call.mid`、`n->u.node_plus.lhs->u.node_ivar_get.ic` …)。この
**AST の pointer chase** が特化コードの残りコストの大きな部分になっている。
koruby_precise の optcarrot (AOT, sp4) で、ID / 行番号 / Symbol 値だけを即値に焼く実験
(trials の実験パッチ、ツリー外) は **+10.6%** (172.9 → 190.0 fps)、命令数 −6%、L1d miss −22%。
ただし ID を hash に含めるので SD が 1 プログラムの intern 順に鍵づけされ、別プログラムと
SD を共有できない (Reusable の原則に反する)。よって「共有テンプレートはそのまま、値は
インスタンスごとに供給する」機構が要る。詳細と数字は
`sample/koruby_precise/docs/copy_and_patch.md`。

### 7.2 穴の抽象

SD テンプレートの中で「実行時に NODE から読んでいた値」を **穴 k** として番号づけし、
SD は `HOLE_U32(k)` / `HOLE_PTR(k)` マクロで参照する。穴の値をどう供給するかは
2 通りあり、**どちらでも同じテンプレート・同じ hash・同じ穴番号**を使う:

| 経路 | 供給方法 | 速さ | 可搬性 |
|---|---|---|---|
| **ローダ** (`astro_cs_instantiate`) | `.o` の `.text` をインスタンスごとにコピーし、ELF 再配置 (穴 = 未定義シンボル) を実値で埋める。即値。 | 最速 (即値、pointer chase 無し) | x86-64 Linux。hot な body だけに限る |
| **dlopen + 初期化** (`astro_cs_load`) | `all.so` の共有 SD をそのまま使い、swap 時に初期化コード `SD_<h>_fill` がインスタンスの **穴の表 (pool)** を作って `n->head.pool` に置く。SD は `P = n->head.pool` を 1 回読み、`P[k]` で参照。 | pointer chase が「深さ d の依存ロード」から「1 + 独立ロード」に。命令数は変わらない | 全 arch / wasm / `--build` |

穴に入れるのは、今 `build_specializer` が「実行時参照」で出しているもの全部:
`@sym` オペランド、`line`、Symbol リテラル (VALUE)、`@ref` の cache アドレス
(`&n->u.X.ic`)、`void *` の記述子、`const char *` の配列、out-of-line 子の `NODE *`
(dispatcher と組で)、そして子を辿るための子 `NODE *` 自体。これで SD 本体は `n` を
穴表の取得以外に参照しなくなる。

### 7.3 生成器 (lib/astrogen.rb)

- `Operand#build_specializer` に 3 つ目の出し方 **hole** を足す。既存は「定数で焼く」
  「`n->u.X.f` を実行時に読む」。どのオペランドを穴にするかはサンプルの Operand
  サブクラスが `hole?` で決める (koruby: `sym?`、`line`、cache 系 `@ref`、`void *`、
  Symbol リテラル、子 NODE*)。hash からは今までどおり除外 (共有を保つ)。
- SPECIALIZE は SD ごとに穴の表を集め、次を `.c` に emit する:
  - `#define SD_<h>_HOLES <n>`
  - `static void SD_<h>_fill(const NODE *n, uintptr_t *pool)` — 木を辿って各穴の値を
    NODE から取り出す初期化コード (`pool[3] = (uintptr_t)&n->u.node_call.argv[0]->u.node_ivar_get.ic;`)。
    ローダ経路もこれで値を得る (パッチの元値)。
  - 穴の種類表 `static const uint8_t SD_<h>_hole_kind[]` (u32 即値 / 64bit ポインタ /
    dispatcher 関数)。ローダが再配置の種類と突き合わせる。
- テンプレート本体の穴参照は `HOLE_*` マクロで書き、ビルドモードで展開を変える:
  - pool モード (`.so`): `#define HOLE_U32(k) ((uint32_t)P[k])`、SD 先頭で
    `const uintptr_t *const P = n->head.pool;`。
  - patch モード (`.o`): `extern char _astro_hole_##k[];`
    `#define HOLE_U32(k) ((uint32_t)(uintptr_t)_astro_hole_##k)` → gcc が
    `mov $imm32` + `R_X86_64_32` を残す。`-fno-pic -fno-plt -fno-jump-tables`。
  同じ `.c` から両方をコンパイルする (`o/` に patch 版、`all.so` に pool 版)。
- インライン展開された子は親と同じ pool / 同じコードコピーに入るので穴番号は body
  単位で通し番号。out-of-line 子 (`@noinline`、block の node_entry) は自分の root を持つ。

### 7.4 ランタイム (runtime/astro_code_store.{h,c})

```c
// NodeHead に 1 語追加
const uintptr_t *pool;          // pool モード: この body インスタンスの穴の表

// dlopen 経路 (既存の astro_cs_load の中):
//   dlsym("SD_<h>") → dispatcher、dlsym("SD_<h>_fill") → pool を作って fill → n->head.pool
bool astro_cs_load(NODE *n, const char *file);

// ローダ経路 (任意、x86-64 Linux):
//   o/SD_<h>.o の .text/.rodata をコピー、.rela.text を SD_<h>_fill の値と
//   dlsym(RTLD_DEFAULT) で解決、n->head.dispatcher にコピーを据える。
//   失敗 (未対応 reloc / arch) は false → 呼び側は astro_cs_load のままでよい。
bool astro_cs_instantiate(NODE *n);
```

- ローダの中身: 最小 ELF リーダ (`.symtab` / `.rela.text` / `.rodata`)、扱う再配置は
  `R_X86_64_64 / 32 / 32S / PC32 / PLT32`。`.rodata` (文字列・定数) もコピーして PC32 を
  つなぐ。コード領域はテンプレート `.so` とホストバイナリの ±2 GB に `mmap` (hint 付き)
  して `call rel32` が届くようにする。`mprotect` で RX。
- hot 判定はサンプル側 (PG の count、または swap 後 N 回呼ばれたら) — code store は
  「呼ばれたら instantiate する」だけ。cold は共有 SD のまま。
- `.o` は `astro_cs_build` が既に `o/` に持っている。patch 版と pool 版で CFLAGS が
  違うので `o/` を 2 系統にする (`o/` = .so 用、`op/` = patch 用)。

### 7.5 サンプル側

- Operand サブクラスで `hole?` を返す (koruby_gen.rb)。
- `n->head.pool` を GC や dump が触る必要は無い (libc 確保、immortal)。
- それ以外は無変更: 呼び出し規約 `(CTX *, NODE *, VALUE *)` は据え置き。

### 7.6 見込みと段取り

- pool モード: 依存ロードが消える分だけ。ID 即値実験 (+10.6%) の一部、たぶん半分程度。
  全 arch で効き、bake 時間も伸びない (テンプレート共有のまま) ので**先にこれ**。
- ローダ: 即値化で残り全部 + ic/子ポインタも即値。I キャッシュは hot 限定で抑える。
- 段取り: (1) astrogen に hole 出力 + `_fill` 生成 (pool モード) → koruby で計測
  (2) `.o` の patch 版ビルド + `astro_cs_instantiate` の spike を SD 1 個で
  (3) hot 判定と optcarrot / microbench で判定。

### 7.7 実装状況 (2026-09-06): pool 経路 = 実装済み、ローダ経路 = 未実装

実装箇所: `lib/astrogen.rb` (`Node.pool_mode?` / `Operand#hole?` / `hole_arg` /
`child_call_emitter` / `sd_pool_*`)、`runtime/astro_hole.h` (`astro_hole_t`、`HOLE_*`、
`ASTRO_POOL_PARAM`)、`runtime/astro_node.c` (`astro_hole_alloc` / `astro_hole_sub` /
`astro_hole_emit_fill`、`--build` builder 末尾の pool attach)、`runtime/astro_code_store.c`
(dedup 表に穴数、`astro_cs_load` が `SD_<h>_pool` を dlsym して `astro_cs_pool_attach`、
store format salt)。koruby_precise: `node.h` の NodeHead に `pool` / `nholes` と
`ASTRO_NODEHEAD_POOL`、`koruby_gen.rb` は `pool_mode?` (`KORUBY_POOL=0` で従来出力) と
`hole?` の宣言、SD 出力の上書きを pool 対応。opt-in しないサンプルの生成物は不変
(node_alloc.c の `#ifdef ASTRO_NODEHEAD_POOL` 初期化だけ増える)。

上の設計 (7.2〜7.4) からの差分:

- **穴番号は SD (subtree) ごとに 0 起点の相対番号**。親は子 SD を `P + off` で呼び、
  `SD_<h>_fill` が子の `SD_<c>_fill(child, pool + off)` を連鎖する。同じ形の inline SD が
  1 ファイル内で dedup されても同じテキストで済む (絶対番号だと親ごとに別テキスト)。
  子の穴数は `n->head.nholes` (SPECIALIZE 時、dedup hit でも表から復元)。
- inline SD は `(CTX *, NODE *, VALUE *, ASTRO_POOL_PARAM)` の 4 引数。public SD は
  呼び出し規約据え置きで、先頭で `P = n->head.pool` を 1 回読む。
- lazy 子 (`NODE *` operand、`EVAL_ARG`) は SD TU (`ASTRO_SD_POOL`) では
  `ASTRO_LAZY_PARAM(x)` = (SD 関数ポインタ, `x_pool`) の対で受け、`EVAL_ARG` が
  `(c, n, slots, x_pool)` で呼ぶ。no_inline 子 (cycle break / `@noinline` kind) は
  `astro_sd_indirect` (runtime dispatcher へ橋渡し、pool 引数は NULL)。interp TU は従来どおり
  (`node_eval.c` はマクロで両 TU に対応)。
- `SD_<h>_HOLES` / 穴の種類表は出さず、exported `uint32_t SD_<h>_pool(const NODE *,
  astro_hole_t *)` (pool が NULL なら穴数だけ返す) に集約。ローダ経路の穴種別は `.o` の
  再配置種別 (R_X86_64_32 = u32 / R_X86_64_64 = ptr) で判る。
- pool 要素は `unsigned long long` (LP64 では VALUE = long と TBAA が別クラスなので slot
  store が `P[k]` の再ロードを強制しない。wasm32 でも 64 bit)。
- 旧 (pool 前) の code_store は hash 一致で再利用されるので、store の version に format
  salt を混ぜて自動で消す (koruby の program store は version 0 だったため)。
- `line` operand: koruby の多くの body は `(void)line` して slow path で `n->u.X.line` を
  読む (fast path に値を抱えない既存方針) ので、その穴は埋まるが読まれない (1 語の無駄、
  実行コスト無し)。
- 罠: SD を `-O3` で焼くと SLP ベクトル化が staged slot への scalar store 2 本を 16B
  load にまとめ、store-forwarding が失敗して 3× 遅くなる形が pool 経路で顕在化した
  (nested_loop)。koruby は SD CFLAGS に `-fno-tree-slp-vectorize` (`docs/perf.md` §4.10)。

結果 (sp4, optcarrot 180 frames AOT, 3 round 交互, YJIT 対照 295〜300 で不変):
**172 → 206 fps (+19.7%)**。ローカル `perf stat -r 3`: 命令数 −1.1%、cycles −13.5%、
**L1d miss 396M → 148M (−63%)**、L1i miss 2.0M → 1.7M。即値 bake 実験 (+10.6%) を超えたのは
ic / 子 NODE* まで密な表に載り、SD の複製 (I キャッシュ) が無いため。microbench 53 本は
15 本 ≥3% 速く (send 0.56 / fannkuch 0.73 / hashiter 0.89 / while 0.89)、4 本 ≥3% 遅い
(casewhen 1.09 / aryidx 1.05 / fib 1.04 / ivar 1.04)。詳細は
`sample/koruby_precise/docs/copy_and_patch.md` と
`~/ruby/src/trials/2026-09-06-koruby-precise-perf/` (経路 P)。
