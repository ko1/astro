# DOOM in the browser — pure Ruby, AOT-compiled, wasm32-wasip1

Pure-Ruby DOOM (the renderer from `sample/rubyharness/apps/doom`) compiled by
koruby_precise into a single `.wasm` with the prelude, the program and every
method body baked in, running in a Worker and drawing to a canvas.

```sh
python3 serve.py            # http://127.0.0.1:8000/  (COOP/COEP for SharedArrayBuffer)
```

`W`/`S` move, `A`/`D` strafe, `←`/`→` turn.

## How it fits together

```
page (main thread)                         Worker
  requestAnimationFrame                      koruby_precise.wasm
    ├─ read frame from SAB → canvas            stdin  ← 1 byte per tick
    └─ tick: key byte + Atomics.notify   ──▶   stdout → 320x240 palette indices
```

The guest is an ordinary WASI program: one byte of input per frame on stdin,
one raw frame on stdout. Nothing in it knows about a browser. The Worker is
those two file descriptors and nothing else (`doom_run.js`), which is why a
plain Node script can drive the identical code path.

Pacing is the host's. The guest's `stdin` read blocks on `Atomics.wait` until
the page's `requestAnimationFrame` bumps the tick, so the page never runs ahead
of the renderer and the renderer never floods the page. A worker may block; the
main thread may not, which is the reason the guest lives over here.

`Atomics.wait` needs a `SharedArrayBuffer`, which needs the document to be
cross-origin isolated — hence `serve.py` rather than `python3 -m http.server`.

## Files

| | |
|---|---|
| `index.html` | page: canvas, keys, palette → RGB, the tick loop |
| `doom_run.js` | the two file descriptors + WASI setup (shared) |
| `worker.js` | browser glue around `doom_run.js` |
| `test_node.mjs` | headless check: same `doom_run.js`, Node `worker_threads` |
| `doom.wasm` | built by `make -C .. aot PROG=<bundle>` |
| `doom1.wad` | shareware WAD (from `apps/doom`) |

## Building the wasm

```sh
cd sample/koruby_precise
WAD_PATH=/doom/doom1.wad OUT=/tmp/doom_web.rb sh ../rubyharness/tools/doom_web.sh
make -C wasi aot PROG=/tmp/doom_web.rb
cp wasi/build/doom_web.wasm wasi/demo/doom/doom.wasm
cp ../rubyharness/apps/doom/doom1.wad wasi/demo/doom/
```

`doom_web.sh` bundles the same engine files as `doom.sh` (koruby has no
`require`) but ends in a frame loop instead of a fixed render plus checksum.

## 3 つの構成を切り替える

ページ上のラジオボタンで、同じ DOOM を 3 つの koruby ビルドで動かせます。

| 選択肢 | モジュール | 中身 |
|---|---|---|
| AOT 新（穴 + pool） | `doom.wasm` | 現行。site 固有値は穴の表 `n->head.pool` から |
| AOT 旧（穴なし） | `doom-old.wasm` | master `0ffc4c0f` + wasm 移植性の修正のみ |
| インタプリタ | `koruby-interp.wasm` | 木を辿るだけ。`--plain /doom/doom_web.rb` |

切り替えると Worker を作り直して最初から走ります。インタプリタだけは
プログラムを外から渡すので、`doom_web.rb` も一緒に配信する必要があります。

## 4 構成の実測 (headless, 固定視点 30 フレーム, wasmtime, best of 3)

| 構成 | 実行時間 | モジュール |
|---|---|---|
| ruby.wasm 3.4.1 | 7.455 s | 23.8 MB |
| koruby インタプリタ | 2.695 s | 4.3 MB |
| koruby AOT 旧 | 0.916 s | 15.2 MB |
| koruby AOT 新 | 0.918 s | 16.0 MB |

ruby.wasm 比 8.1 倍、koruby インタプリタ比 2.9 倍。新旧 AOT の差はばらつきの中で、
wasm には L (ローダ) が無く両方 pool 経路なので当然の結果。再現は
`trials/2026-09-06-koruby-precise-perf/src/wasm_doom_bench.sh`。

**既知の不具合 (この作業とは無関係)**: wasm32 では 64 ビットの符号なしリテラルが
負になる (`0xffff_ffff_ffff_ffff` → `-1`)。シフトも乗算も正しいので描画は一致するが、
DOOM の checksum だけが native と食い違う。koruby のインタプリタでも旧 AOT でも
同じなので既存バグ。

## Checking it without a browser

```sh
node test_node.mjs 120          # AOT 新
node test_node.mjs 30 old       # AOT 旧
node test_node.mjs 30 interp    # インタプリタ
```

It ticks the guest, waits on the frame counter and asserts the frames are not
blank and that the palette arrived. Measured here: **120 frames in 3.16 s (~38
fps)**, WAD load included; 3 frames in 0.61 s from cold. The browser is capped
by `requestAnimationFrame` anyway.

## Why this is the pool path

wasm has no `dlopen` and no runtime C compiler, so the loader (copy-and-patch)
does not exist there: `ASTRO_LOADER_SUPPORTED` is 0 and every hole is read from
the per-instance table, `n->head.pool`, filled at startup by replaying each
shape's descriptor. See `docs/idea_code_store.md` §7.

The descriptors are emitted as `offsetof`/`sizeof` expressions, not as numbers,
precisely so this cross build works: the store is baked by a native x86-64
binary, but `struct Node` on wasm32 has 4-byte pointers and a different layout,
so the offsets have to be the *target* compiler's.
