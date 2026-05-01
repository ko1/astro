// Redux/React-style state reducer with object + array spread.
// Spread sites can't be IC-stabilised by V8 (each call site
// produces a fresh shape) so TurboFan keeps deoptimising.  jstro's
// object literal IC + flat 8-byte JsValue layout pays a constant
// per-iteration cost.  jstro AOT ~2.3 s vs node v18 ~6 s — 2.5×.
function reducer(state, action) {
  switch (action.type) {
    case "ADD":   return { ...state, count: state.count + 1, items: [...state.items, action.item] };
    case "RESET": return { ...state, count: 0 };
    default:      return state;
  }
}
var state = { count: 0, items: [], meta: { version: 1, ts: 0 } };
var start = Date.now();
for (var i = 0; i < 30000; i++) {
  state = reducer(state, { type: "ADD", item: i });
  if (i % 100 === 0) state = reducer(state, { type: "RESET" });
}
var elapsed = (Date.now() - start) / 1000;
console.log("state.count = " + state.count + " items.length = " + state.items.length);
console.log("elapsed: " + elapsed + "s");
