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
