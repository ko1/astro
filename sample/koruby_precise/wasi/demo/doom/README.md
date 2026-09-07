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

## Checking it without a browser

```sh
node test_node.mjs 120
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
