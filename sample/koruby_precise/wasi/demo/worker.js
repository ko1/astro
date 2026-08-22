// Worker-side runner: receives the compiled WebAssembly.Module + Ruby source,
// runs it with the WASI shim, streams stdout lines back.  The main thread
// (and its clock) never blocks; sleep's busy-wait burns this worker only.
import { WASI, File, OpenFile, ConsoleStdout, WASIProcExit } from './shim/index.js';

onmessage = async (ev) => {
  const { mod, code } = ev.data;
  try {
    const fds = [
      new OpenFile(new File([])),
      ConsoleStdout.lineBuffered(l => postMessage({ line: l })),
      ConsoleStdout.lineBuffered(l => postMessage({ line: l })),
    ];
    const wasi = new WASI(['koruby', '-e', code], [], fds, { debug: false });
    const inst = await WebAssembly.instantiate(mod, { wasi_snapshot_preview1: wasi.wasiImport });
    let rc = 0;
    try { rc = wasi.start(inst); }
    catch (e) { if (e instanceof WASIProcExit) rc = e.code; else throw e; }
    postMessage({ exit: rc });
  } catch (e) {
    postMessage({ error: String(e) });
  }
};
