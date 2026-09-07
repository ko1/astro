// Worker-side DOOM runner.
//
// The guest is koruby_precise compiled to wasm32-wasip1 with the whole DOOM
// engine baked in (--build: prelude + program + every method body as an SD).
// It talks one byte in / one frame out over stdin and stdout, so all this side
// has to do is be those two file descriptors.
//
// Pacing is the host's: the guest's stdin read blocks on Atomics.wait until the
// page's requestAnimationFrame bumps the tick.  A worker may block; the main
// thread may not, which is the whole reason the guest runs over here.
import { WASI, File, PreopenDirectory, Fd, ConsoleStdout, WASIProcExit, wasi } from '../shim/index.js';

const FRAME_BYTES = 320 * 240;
const PAL_BYTES = 768;

// stdin: one command byte per host tick.
class TickStdin extends Fd {
  constructor(ctl) { super(); this.ctl = ctl; this.seen = 0; }
  fd_fdstat_get() { return { ret: 0, fdstat: new wasi.Fdstat(wasi.FILETYPE_CHARACTER_DEVICE, 0) }; }
  fd_read(size) {
    if (size === 0) return { ret: 0, data: new Uint8Array(0) };
    // Wait for a tick we have not consumed yet.
    while (Atomics.load(this.ctl, 0) === this.seen) Atomics.wait(this.ctl, 0, this.seen);
    this.seen = Atomics.load(this.ctl, 0);
    return { ret: 0, data: new Uint8Array([Atomics.load(this.ctl, 1)]) };
  }
}

// stdout: "PAL0" + 768 palette bytes once, then raw frames forever.
class FrameStdout extends Fd {
  constructor(ctl, fb, pal) {
    super();
    this.ctl = ctl; this.fb = fb; this.pal = pal;
    this.stage = 'magic'; this.buf = new Uint8Array(FRAME_BYTES); this.n = 0;
  }
  fd_fdstat_get() { return { ret: 0, fdstat: new wasi.Fdstat(wasi.FILETYPE_CHARACTER_DEVICE, 0) }; }
  fd_write(data) {
    let i = 0;
    while (i < data.length) {
      if (this.stage === 'magic') {
        const need = 4 - this.n, take = Math.min(need, data.length - i);
        this.buf.set(data.subarray(i, i + take), this.n); this.n += take; i += take;
        if (this.n === 4) { this.stage = 'pal'; this.n = 0; }
      } else if (this.stage === 'pal') {
        const need = PAL_BYTES - this.n, take = Math.min(need, data.length - i);
        this.pal.set(data.subarray(i, i + take), this.n); this.n += take; i += take;
        if (this.n === PAL_BYTES) { this.stage = 'frame'; this.n = 0; Atomics.store(this.ctl, 3, 1); }
      } else {
        const need = FRAME_BYTES - this.n, take = Math.min(need, data.length - i);
        this.buf.set(data.subarray(i, i + take), this.n); this.n += take; i += take;
        if (this.n === FRAME_BYTES) { this.fb.set(this.buf); this.n = 0; Atomics.add(this.ctl, 2, 1); }
      }
    }
    return { ret: 0, nwritten: data.byteLength };
  }
}


// One entry point for both hosts: the browser Worker and the Node test.
// `src` is the Ruby program when the module is the plain interpreter (it is
// then run as `koruby --plain /doom/doom_web.rb`); the AOT modules carry the
// program inside and ignore it.
export async function runDoom({ mod, wad, src, argv, ctlBuf, fbBuf, palBuf, log, ready }) {
  const ctl = new Int32Array(ctlBuf);
  const fb = new Uint8Array(fbBuf);
  const pal = new Uint8Array(palBuf);
  const fds = [
    new TickStdin(ctl),
    new FrameStdout(ctl, fb, pal),
    ConsoleStdout.lineBuffered(l => log && log(l)),
    new PreopenDirectory('/doom', new Map([
      ['doom1.wad', new File(wad, { readonly: true })],
      ...(src ? [['doom_web.rb', new File(src, { readonly: true })]] : []),
    ])),
  ];
  const wasi = new WASI(argv || ['doom'], [], fds, { debug: false });
  const inst = await WebAssembly.instantiate(mod, { wasi_snapshot_preview1: wasi.wasiImport });
  ready && ready();
  try { return wasi.start(inst); }
  catch (e) { if (e instanceof WASIProcExit) return e.code; throw e; }
}
