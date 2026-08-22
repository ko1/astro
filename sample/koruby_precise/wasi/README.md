# WASI (wasm32) ビルド

```sh
make -C wasi                                # build/koruby.wasm (インタプリタ)
wasmtime --dir .::/koruby wasi/build/koruby.wasm -e 'p 1+2'

make -C wasi aot PROG=foo.rb                # build/foo.wasm (AOT 全埋め込み)
wasmtime wasi/build/foo.wasm                # mount 不要 (prelude+program 埋め込み)
```

libprism-wasm.a は prism/src/*.c を同じ target でコンパイルして llvm-ar でまとめる
(prism は wasm32 でそのまま通る)。wasi/Makefile が全部やる。

## AOT (`--build` 全埋め込み)

wasm には dlopen も実行時 C コンパイラも無いので、AOT は**ネイティブの
koruby_precise が `--build` でクロスコンパイル**する (KORUBY_BUILD_TARGET=wasi):

1. prelude + program + 全 method body を code_store_wasi/ に bake
   (SD .c 生成はファイル出力だけなのでネイティブ側でやれる。hash は構造のみで
   symbol ID 非依存 — node_lit の symbol は SD が runtime-ref する)
2. `_embed.c` を emit — AST を DAG のまま ALLOC 列で再構築する C コード。
   dispatcher は SD_<hash> に直接パッチ済み、symbol 名は起動時に re-intern
3. wasi-sdk clang で SD .o (並列, code_store_wasi/o/ にキャッシュ) +
   ホスト .o (code_store_wasi/host/ にキャッシュ) をビルドしてリンク

exe は起動時にパースを一切しない (プログラムも prelude も埋め込み AST を
再構築するだけ)。warm ビルドは ~3.5s (native は ~4.5s)。

計測 (2026-08-22, wasmtime compile 済み .cwasm, 3 回の生値は
`~/ruby/src/trials/20260822_koruby_wasm_aot/`):

| bench | AOT wasm | interp wasm | ruby.wasm (記録値) |
|---|---|---|---|
| 起動 (`p 1`) | **0.014s** | 0.30s | 0.09s |
| while 80M | **0.43s** | 7.0s | 15.9s |
| fib(32) | **0.15s** | 0.93s | 0.45s |
| 8M.times | **0.19s** | 0.82s | 1.0s |
| object 3M | **0.39s** | 1.33s | 1.96s |
| String 2M | **0.70s** | 1.12s | 1.08s |
| Hash 4M | **0.58s** | 1.63s | 2.49s |
| 配列 1.5M 保持 | **0.28s** | 0.75s | 1.18s |

起動が ruby.wasm の 6 倍速くなり (パース消滅)、インタプリタで唯一負けていた
再帰 (fib) も 3 倍速い側に回った。

## 32bit で踏むもの

- **WASI の mmap は MAP_NORESERVE も PROT_NONE のガードページも扱えない**。
  値スタック (korb_ctx_new) と GC のアリーナ (gc_copy.c) は calloc に落ちる。
  値スタックの溢れがガードページで捕まらなくなる。
- **copy GC の 64 GiB 仮想予約が成立しない**。size_t が 32bit なので 64 GiB が
  0 に切り詰まり「OOM (have 0)」になる。gc.h が SIZE_MAX を見て 32bit では
  64 MiB の実メモリを取る。遅延ページングが無いのでここは実メモリである。

  semispace 2 面ぶんを起動時に確保し、しかも**伸びない**。512 MiB * 2 = 1 GiB
  (アドレス空間 4 GiB の 1/4) を取っている。64 MiB では 2 要素配列 1.5M 個で
  `aro_gc: OOM (need 24, have 16)` になった。本来は必要に応じて伸ばすべきで、
  この定数はその暫定。

  **確保に calloc を使ってはいけない**。wasm の線形メモリは memory.grow で
  増えた分が仕様上ゼロなので (mmap(MAP_ANONYMOUS) のゼロ保証と同じ)、calloc の
  memset はそのまま RSS になる。512 MiB * 2 で実測:

  | | 起動 | `-e ''` の最大 RSS |
  |---|---|---|
  | calloc | 0.87 s | 1,274 MB |
  | malloc | 0.76 s | **223 MB** |

  「大きく取るとメモリを食う」というトレードオフは calloc が作っていた偽物だった。

## Linux の前提をそのまま移していないか (見直しリスト)

上の calloc はこの類の見落としだった。同種のものが残っている:

- `madvise` を no-op にしている。wasm にページを OS へ返す概念が無いので正しい
  はずだが、GC が「使い終わった semispace を返す」意図で呼んでいるなら、
  wasm では返せないぶんメモリが張り付く。
- guard page を落としている。mprotect(PROT_NONE) が使えないので値スタックの
  溢れが検出できない。wasm は線形メモリの外に出れば trap するので、末尾を
  敢えて未確保にする手はある。
- **wasm の既定スタックは 64KB**。koruby の深さ検査が即発火するので
  `-Wl,-z,stack-size` で広げる。

## この構成で動かないもの

- Fiber / Thread (ucontext が無い) と Enumerator#next
- Process / Kernel#system / Socket (wasi/wasi_stubs.c が NotImplementedError)

正規表現は**静的リンクで動く** (native の koruby_regex.so dlopen の代替)。
astrogre + regex_bridge を wasi clang でビルドし、ASTro の汎用シンボル
(ALLOC_node_* / kind_* / OPTION / astro_* — koruby と衝突する) を
プリプロセッサで `astrogre_hidden_` に一括リネームして
libkoruby-regex-wasm.a にまとめる (wasi/Makefile。llvm-objcopy は wasm obj の
シンボル操作未対応なので nm→rename.h の 2 pass)。
- 実行時 AOT (`--aot-compile`) — 実行時コンパイル不可。かわりに上記の
  `--build` クロスコンパイルを使う (AOT 済み全埋め込み .wasm が出る)。
- `BARUBY_GC_PURGE` — mprotect(PROT_NONE) ベースなので wasm では
  `mmap PURGE arena: Invalid argument`。STRESS は使える。
- 多倍長は BIGNUM=wrap (64bit で wrap、Ruby とは意味論が違う)。GMP を
  クロスビルドすれば BIGNUM=gmp も使えるはず。

インタプリタ .wasm は prelude を `--dir` mount から起動時に読む。
AOT .wasm は prelude 埋め込み済みで mount 不要
(require する場合だけ `--dir` が要る)。

## ブラウザで動く (wasi/demo/)

import は標準の `wasi_snapshot_preview1` 33 個だけなので、ブラウザは JS の
WASI shim があれば動く。`@bjorn3/browser_wasi_shim` 0.4.2 (wasi/demo/shim/ に
vendor、MIT/Apache-2.0) で **interp + AOT の両方が動くことを Node (V8 = ブラウザと
同じエンジン + 同じ shim コード) で確認済み**。ブロック・regex・`sleep` も動く。

```sh
# サンプルルートで:
python3 -m http.server
# → http://localhost:8000/wasi/demo/
```

- **インタプリタ .wasm は prelude ソース埋め込み** (`-DKORUBY_PRELUDE_BLOB`,
  tools/gen_prelude_blob.rb が main.c の KORUBY_PRELUDE_FILES から生成) なので
  mount も fetch も不要。native は開発性 (prelude 編集→即実行) のため
  ファイル読みのまま。AOT .wasm は AST ごと埋め込みで元から不要。
- **ブラウザ (main thread) は実行中止まる**: wasm は同期実行なので、走って
  いる間 rAF / setInterval / DOM は全部停止する (CSS アニメーションだけは
  compositor 駆動なので動き続けることが多い)。demo に JS 時計 + CSS スピナーを
  置いて見えるようにしてある。**Worker 実行なら UI は止まらない** (demo の
  Run (Worker) ボタン。compile 済み Module を postMessage で渡す)。
- **`sleep` は動くが busy-wait**: shim の `poll_oneoff` は
  `while (endTime > getNow()) {}` で回す (JS は本当にはブロックできない)。
  時間は正確だが CPU 1 コアを食う。Worker なら UI には影響しない。真面目に
  やるなら SharedArrayBuffer + Atomics.wait ベースの shim (COOP/COEP 必須)。
  wasmtime / Node native WASI は本物のブロック。
- shim の `poll_oneoff` は clock 購読 1 本のみ。fd を混ぜる IO 待ちは NOTSUP
  になるので、IO.select 系はブラウザでは不可。
- **vendored shim は 1 箇所パッチ済み** (wasi.js 冒頭コメント参照):
  args_sizes_get が JS 文字列の `.length` (UTF-16 単位) でサイズを申告するのに
  args_get は UTF-8 バイトを書くため、**非 ASCII を含む argv (-e の日本語
  コメント等) でバッファを溢れて "memory access out of bounds"** になる。
  エンコード後バイト長を数えるよう修正 (environ 側は元から正しい)。
  上流 (main, 0.4.2 時点) は未修正。
- ヒープは起動時 malloc の 512 MiB×2 だが、wasm の memory.grow は OS 側で
  遅延コミットされるので実 RSS は数百 MB 弱 (native の観測と同じ理屈)。
  モバイル Safari 等ではタブのメモリ上限に注意。

## ruby.wasm との比較 (2026-08-22, wasmtime 44)

**測り方**: `wasmtime compile` で .cwasm にしてから測る。**そうしないとモジュールの
コンパイル時間を測ることになる** (27 MB の ruby.wasm で毎回 7.5 秒)。wasmtime は
既定でモジュール全体を事前コンパイルし、キャッシュは `directory` を書いた TOML を
`-C cache-config=` で渡さない限り効かない (`-C cache=y` だけでは置き場が無く効かない)。

事前コンパイル済みでの起動: **koruby 0.30s / ruby 0.09s** — ruby のほうが 3 倍速い。
koruby は起動のたびに 5,187 行の prelude をパースしているため (埋め込みの動機)。

実行時間 (起動を引いた値、3 回の最小)。koruby は**純インタプリタ (AOT 無し)**、
CRuby は wasm なので YJIT 無し:

| bench | koruby | ruby | 比 |
|---|---|---|---|
| while 80M | 6.40s | 15.47s | **0.41x** |
| 8M.times | 0.49s | 1.00s | **0.49x** |
| 3M object 確保 (使い捨て) | 0.61s | 1.89s | **0.32x** |
| 2M String 確保 | 0.84s | 1.04s | **0.81x** |
| fib(32) | 0.56s | 0.43s | **1.30x** (koruby が遅い) |

ループ・ブロック・短命オブジェクトの確保で速く、再帰で負ける。ネイティブでの
YJIT 比の傾向 (再帰と object だけ負ける) と一致している。

実行時間が 0.3 秒を下回るものは起動の引き算誤差に埋もれるので載せていない
(Hash・Array・生存データ保持)。生存データ系は上記のヒープ上限で規模を上げられない。
