// Headless check for the browser demo: drives doom_run.js exactly the way the
// page does — a worker blocked on Atomics.wait, the host ticking it once per
// "frame" — and asserts that real frames come out.
//
//   node test_node.mjs [frames]
//
// A browser is not needed to see whether the file descriptors, the palette
// hand-off and the tick protocol work; only to see the pixels.
import { Worker, isMainThread, workerData } from 'node:worker_threads';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const W = 320, H = 240, FRAME = W * H;
const HERE = fileURLToPath(new URL('.', import.meta.url));

if (!isMainThread) {
  const { mod, wad, src, argv, ctlBuf, fbBuf, palBuf } = workerData;
  const { runDoom } = await import('./doom_run.js');
  await runDoom({ mod, wad, src, argv, ctlBuf, fbBuf, palBuf, log: l => console.log('[guest]', l) });
} else {
  const frames = Number(process.argv[2] || 3);
  const kind = process.argv[3] || 'new';
  const BUILDS = {
    new:    { wasm: 'doom.wasm' },
    old:    { wasm: 'doom-old.wasm' },
    interp: { wasm: 'koruby-interp.wasm', src: 'doom_web.rb',
              argv: ['koruby', '--plain', '/doom/doom_web.rb'] },
  };
  const b = BUILDS[kind];
  if (!b) { console.error('unknown build:', kind); process.exit(1); }
  const wasm = readFileSync(HERE + b.wasm);
  const src = b.src ? readFileSync(HERE + b.src) : null;
  const wad = readFileSync(HERE + 'doom1.wad');
  const mod = await WebAssembly.compile(wasm);

  const ctlBuf = new SharedArrayBuffer(16);
  const fbBuf = new SharedArrayBuffer(FRAME);
  const palBuf = new SharedArrayBuffer(768);
  const ctl = new Int32Array(ctlBuf), fb = new Uint8Array(fbBuf), pal = new Uint8Array(palBuf);

  const w = new Worker(new URL(import.meta.url), {
    workerData: { mod, wad: wad.buffer.slice(wad.byteOffset, wad.byteOffset + wad.byteLength),
                  src: src ? src.buffer.slice(src.byteOffset, src.byteOffset + src.byteLength) : null,
                  argv: b.argv, ctlBuf, fbBuf, palBuf },
  });
  w.on('error', e => { console.error('worker error:', e); process.exit(1); });

  const t0 = Date.now();
  const tick = (key) => { Atomics.store(ctl, 1, key.charCodeAt(0)); Atomics.add(ctl, 0, 1); Atomics.notify(ctl, 0); };
  const waitFor = async (n, whatFor) => {
    for (let i = 0; i < 2000000; i++) {
      if (Atomics.load(ctl, 2) >= n) return true;
      await new Promise(r => setImmediate(r));
    }
    console.error(`timeout waiting for ${whatFor}`); process.exit(1);
  };

  const keys = ['.', 'w', 'j', 'd', 's', 'l'];
  for (let i = 0; i < frames; i++) {
    tick(keys[i % keys.length]);
    await waitFor(i + 1, `frame ${i + 1}`);
    const nonzero = fb.reduce((a, b) => a + (b !== 0 ? 1 : 0), 0);
    console.log(`frame ${i + 1}: ${nonzero} / ${FRAME} non-zero pixels`
              + `, first row ${Array.from(fb.subarray(0, 8)).join(',')}`);
    if (nonzero < FRAME / 10) { console.error('frame looks empty'); process.exit(1); }
  }
  const palSum = pal.reduce((a, b) => a + b, 0);
  console.log(`palette ready=${Atomics.load(ctl, 3)} sum=${palSum}`);
  if (Atomics.load(ctl, 3) !== 1 || palSum === 0) { console.error('no palette'); process.exit(1); }
  console.log(`${kind}: ${frames} frames in ${((Date.now() - t0) / 1000).toFixed(2)}s`);
  tick('q');
  await new Promise(r => setTimeout(r, 500));
  process.exit(0);
}
