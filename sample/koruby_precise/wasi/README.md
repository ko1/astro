# WASI (wasm32) ビルド

```sh
WASI_SDK=$HOME/wasi-sdk
$WASI_SDK/bin/clang --target=wasm32-wasip1 -O2 -w \
  -I. -Iwasi -Iprism/include -I../../runtime -include wasi/wasi_decls.h \
  -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_PROCESS_CLOCKS \
  -DKORB_WASI=1 -DKORB_BIGNUM=2 -DBARUBY_GC=5 -DASTRO_DEBUG=0 \
  -DKORUBY_SRC_DIR='"/koruby"' -DASTRO_RUNTIME_DIR='"/koruby"' -DASTRO_PRISM_INC_DIR='"/koruby"' \
  main.c node.c korb_runtime.c parse.c wasi/wasi_missing.c \
  ../../runtime/precise_gc/gc_common.c ../../runtime/precise_gc/gc_copy.c \
  libprism-wasm.a -lwasi-emulated-mman -lwasi-emulated-signal -lwasi-emulated-process-clocks -lm \
  -Wl,-z,stack-size=16777216 -o koruby.wasm

wasmtime --dir .::/koruby koruby.wasm -e 'p 1+2'
```

libprism-wasm.a は prism/src/*.c を同じ target でコンパイルして llvm-ar でまとめる
(prism は wasm32 でそのまま通る)。

## 32bit で踏むもの

- **WASI の mmap は MAP_NORESERVE も PROT_NONE のガードページも扱えない**。
  値スタック (korb_ctx_new) と GC のアリーナ (gc_copy.c) は calloc に落ちる。
  値スタックの溢れがガードページで捕まらなくなる。
- **copy GC の 64 GiB 仮想予約が成立しない**。size_t が 32bit なので 64 GiB が
  0 に切り詰まり「OOM (have 0)」になる。gc.h が SIZE_MAX を見て 32bit では
  64 MiB の実メモリを取る。遅延ページングが無いのでここは実メモリである。

  **これが実用上いちばんきつい制約**。semispace が 2 面なので起動時に calloc で
  128 MiB を実確保し、しかも**伸びない**。2 要素配列を 1.5M 個保持しただけで
  `aro_gc: OOM (need 24, have 16)` になる (64bit では 64 GiB 仮想 + 遅延ページング
  なので事実上上限が無い)。定数を上げるのではなく、必要に応じて semispace を
  伸ばす形にするのが本筋。生存データを積むベンチが測れないのもこれが理由。
- **wasm の既定スタックは 64KB**。koruby の深さ検査が即発火するので
  `-Wl,-z,stack-size` で広げる。

## この構成で動かないもの

- Fiber / Thread (ucontext が無い) と Enumerator#next
- Process / Kernel#system / Socket (wasi/wasi_stubs.c が NotImplementedError)
- **正規表現** — koruby_regex.so を dlopen しているため。静的リンクが要る。
- AOT code store — 実行時コンパイルができない。静的 code store が要る。
- 多倍長は BIGNUM=wrap (64bit で wrap、Ruby とは意味論が違う)。GMP を
  クロスビルドすれば BIGNUM=gmp も使えるはず。

prelude はまだ埋め込んでおらず、`--dir` で渡している。

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
