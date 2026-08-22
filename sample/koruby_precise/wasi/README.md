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

## ベンチマーク (全 53 本, 起動込み総実行時間)

rubyharness の全 53 bench、各 3 回の min、**起動込み wall clock** (wasmtime
compile 済み .cwasm)。生値と方法は
`~/ruby/src/trials/20260822_koruby_wasm_full_bench/`。出力は native CRuby を
oracle に diff 検証済み。3 系比較できる 50 bench で **AOT は 50/50 全勝**
(vs ruby.wasm)、interp は 28勝22敗。

ファイルサイズ: **AOT .wasm ~7.8 MB/本** (プログラム毎の増分は ~0.1 MB;
中身はインタプリタ+prelude+SD の固定部) vs **ruby.wasm 27.0 MB**、interp
koruby.wasm 3.0 MB。.cwasm 化後は AOT ~27.7 MB / interp 9.4 MB / ruby 40.1 MB。

「—」= 除外: bignum/render_span_kernel は BIGNUM=wrap の意味論差 (koruby 側)、
nbody は ruby.wasm が native CRuby と libm 最終桁不一致 (koruby は一致)。

| bench | ruby.wasm | interp | AOT | AOT .wasm |
|---|---:|---:|---:|---:|
| ackermann | 2.90s | 3.79s | 0.78s | 7.76 MB |
| array_access | 2.91s | 3.28s | 0.86s | 7.76 MB |
| ary | 1.72s | 1.99s | 0.24s | 7.75 MB |
| aryidx | 0.17s | 0.07s | 0.02s | 7.76 MB |
| bignum | 0.40s | — | — | 7.75 MB |
| binary_trees | 0.94s | 0.84s | 0.39s | 7.77 MB |
| bitops | 15.92s | 6.06s | 0.30s | 7.76 MB |
| block | 5.08s | 0.74s | 0.30s | 7.75 MB |
| block_yield_kernel | 2.11s | 0.80s | 0.24s | 7.76 MB |
| casewhen | 1.30s | 2.29s | 0.31s | 7.76 MB |
| closures | 2.47s | 1.30s | 0.47s | 7.76 MB |
| cmpsort | 2.19s | 0.79s | 0.53s | 7.77 MB |
| collatz | 2.14s | 4.80s | 0.74s | 7.76 MB |
| exception | 1.23s | 0.65s | 0.27s | 7.76 MB |
| fannkuch | 2.21s | 3.71s | 0.31s | 7.78 MB |
| fib | 1.58s | 1.98s | 0.42s | 7.76 MB |
| floatcalc | 4.12s | 2.53s | 0.29s | 7.76 MB |
| gc_bigobj | 2.57s | 0.58s | 0.32s | 7.76 MB |
| gc_wb | 0.68s | 0.67s | 0.15s | 7.76 MB |
| gcchurn | 1.55s | 2.66s | 0.36s | 7.76 MB |
| gcd | 3.10s | 3.58s | 0.68s | 7.76 MB |
| gen_gc | 1.23s | 1.35s | 0.77s | 7.77 MB |
| hash | 2.61s | 3.82s | 1.53s | 7.76 MB |
| hashiter | 2.41s | 0.64s | 0.25s | 7.76 MB |
| intdiv | 2.03s | 2.16s | 0.18s | 7.76 MB |
| iterators | 8.34s | 2.10s | 0.85s | 7.76 MB |
| ivar | 3.27s | 3.60s | 0.76s | 7.76 MB |
| kwargs | 1.48s | 2.44s | 0.50s | 7.76 MB |
| mandelbrot | 6.71s | 3.30s | 0.55s | 7.77 MB |
| mapreduce | 5.84s | 1.19s | 0.49s | 7.76 MB |
| mathfn | 2.48s | 1.61s | 0.75s | 7.76 MB |
| method_call | 3.41s | 3.83s | 0.77s | 7.76 MB |
| methodchain | 4.65s | 1.07s | 0.68s | 7.76 MB |
| nbody | — | 1.50s | 0.67s | 7.83 MB |
| nested_loop | 2.61s | 3.95s | 0.47s | 7.76 MB |
| nesteddata | 0.40s | 0.32s | 0.14s | 7.76 MB |
| object | 1.54s | 1.35s | 0.43s | 7.76 MB |
| poly | 1.50s | 2.28s | 0.50s | 7.77 MB |
| rangeeach | 1.70s | 0.41s | 0.17s | 7.75 MB |
| render_span_kernel | 3.69s | — | — | 7.79 MB |
| send | 2.60s | 3.39s | 0.55s | 7.76 MB |
| sieve | 1.07s | 1.34s | 0.18s | 7.76 MB |
| sort | 4.68s | 0.76s | 0.18s | 7.76 MB |
| sprintfb | 1.68s | 1.18s | 0.85s | 7.76 MB |
| str | 2.35s | 2.82s | 1.64s | 7.76 MB |
| strcmp | 0.43s | 0.29s | 0.23s | 7.77 MB |
| strfmt | 2.16s | 2.79s | 1.95s | 7.76 MB |
| strops | 1.65s | 0.79s | 0.56s | 7.77 MB |
| strscan | 1.39s | 0.52s | 0.51s | 7.76 MB |
| structacc | 2.78s | 0.99s | 0.40s | 7.77 MB |
| tak | 3.24s | 3.92s | 0.85s | 7.76 MB |
| while | 10.23s | 4.23s | 0.29s | 7.75 MB |
| while2 | 2.08s | 1.88s | 0.13s | 7.75 MB |

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

GitHub Pages でもそのまま動く: `wasi/build/` は gitignore なので、ページは
コミット済みの `wasi/demo/koruby.wasm` (strip 済み 2.3 MB) に fallback する。
この copy の更新は意図的な手順 (`make -C wasi demo-wasm`) — ビルドのたびに
2.3 MB を git に積まないため。

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

## 測り方の注意 (wasmtime)

`wasmtime compile` で .cwasm にしてから測ること。そうしないとモジュールの
コンパイル時間を測ることになる (27 MB の ruby.wasm で毎回 7.5 秒)。wasmtime の
キャッシュは `directory` を書いた TOML を `-C cache-config=` で渡さない限り
効かない (`-C cache=y` だけでは置き場が無く効かない)。

(朝時点の interp-only 比較とその撤回の経緯は
`~/ruby/src/trials/20260822_koruby_wasm_vs_rubywasm/` と
`~/ruby/src/trials/20260822_koruby_wasm_aot/` に残してある。)
