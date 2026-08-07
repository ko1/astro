/**
 * @name Borrowed string bytes held across a may-GC call
 * @description A raw pointer into a KorbString's byte buffer (str->buf->data) is
 *              stored in a local, a may-GC call runs, then the SAME stored
 *              pointer is used.  Under koruby's moving GC the buffer may have
 *              moved, so the pointer is stale.  SSA-precise: a re-derive
 *              (p = str->buf->data again) is a new SSA definition, so the
 *              re-derive-each-iteration idiom is correctly treated as safe.
 * @kind problem
 * @problem.severity error
 * @id koruby/borrow-after-gc
 */
import cpp
import semmle.code.cpp.controlflow.SSA

predicate calls(Function f, Function g) {
  exists(FunctionCall c | c.getEnclosingFunction() = f and c.getTarget() = g)
}

predicate mayGcFn(Function f) {
  f.hasName("korb_alloc")
  or
  exists(Function g | mayGcFn(g) and calls(f, g))
}

predicate mayGcNode(ControlFlowNode n) {
  exists(Call c | c = n | mayGcFn(c.getTarget()) or not exists(c.getTarget()))
}

/** `f` is declared with the ARO_BORROW marker on its declarator line. */
predicate isBorrowAccessor(Function f) {
  exists(MacroInvocation mi, FunctionDeclarationEntry fde |
    mi.getMacroName() = "ARO_BORROW" and
    fde = f.getADeclarationEntry() and
    mi.getFile() = fde.getFile() and
    mi.getLocation().getStartLine() = fde.getLocation().getStartLine()
  )
}

/**
 * A raw pointer into a movable GC buffer, from either:
 *   - an inline `->data` field access (KorbStrBuf::data / KorbArrayItems::data,
 *     separately-allocated buffers that move on GC), or
 *   - a call to an ARO_BORROW accessor (e.g. korb_str_data): as code is routed
 *     through accessors the `->data` moves inside them, so the temporal check
 *     must follow the accessor's return value to stay effective.
 */
class BorrowExpr extends Expr {
  BorrowExpr() {
    this.(FieldAccess).getTarget().getName() = "data" and
    this.(FieldAccess).getTarget().getDeclaringType().getName() = ["KorbStrBuf", "KorbArrayItems"]
    or
    isBorrowAccessor(this.(FunctionCall).getTarget())
  }
}

/**
 * The pointer value of `b` flows to `e` through value-preserving steps only —
 * conversions (incl. array-to-pointer decay) and pointer arithmetic.  This does
 * NOT cross a function call (`n = utf8len(s->buf->data)` → n is a count) or an
 * array index / dereference (`ch = data[i]` → ch is one byte), which merely
 * *mention* the borrow without holding the pointer.
 */
predicate borrowFlowsTo(Expr e, BorrowExpr b) {
  e = b
  or
  borrowFlowsTo(e.(Conversion).getExpr(), b)
  or
  borrowFlowsTo(e.(PointerArithmeticOperation).getAnOperand(), b)
  or
  // `&base[i]` — the address of an element is still an interior pointer.
  borrowFlowsTo(e.(AddressOfExpr).getOperand().(ArrayExpr).getArrayBase(), b)
  or
  // alias: `q = p` where the pointer local `p` already holds the borrow.
  exists(SsaDefinition d2, StackVariable v2 |
    e = d2.getAUse(v2) and
    v2.getUnspecifiedType() instanceof PointerType and
    borrowFlowsTo(d2.getDefiningValue(v2), b)
  )
}

/** SSA def `p = <pointer derived from str->buf->data>` — the borrow is held in `v`. */
predicate borrowDef(SsaDefinition def, StackVariable v, BorrowExpr b) {
  v.getUnspecifiedType() instanceof PointerType and
  borrowFlowsTo(def.getDefiningValue(v), b)
}

/** A node that (re)defines `v` other than `def` — crossing it resets staleness. */
predicate redefOf(SsaDefinition def, StackVariable v, ControlFlowNode n) {
  exists(SsaDefinition d2 |
    d2 != def and d2.getDefinition() = n and exists(d2.getDefiningValue(v))
  )
}

/**
 * `n` is reachable from `def` on a path that does not cross another definition
 * of `v`; `sawGc` records whether a may-GC call has occurred on the way.
 */
predicate reach(SsaDefinition def, StackVariable v, ControlFlowNode n, boolean sawGc) {
  n = def.getDefinition().getASuccessor() and
  (if mayGcNode(n) then sawGc = true else sawGc = false)
  or
  exists(ControlFlowNode prev, boolean s0 |
    reach(def, v, prev, s0) and
    // Don't step through a (re)definition of v — including THIS def re-reached
    // via a loop back-edge, which is a re-derive (a fresh, non-stale value).
    not prev = def.getDefinition() and
    not redefOf(def, v, prev) and
    n = prev.getASuccessor() and
    (if mayGcNode(n) then sawGc = true else sawGc = s0)
  )
}

from SsaDefinition def, StackVariable v, BorrowExpr b, VariableAccess use
where
  borrowDef(def, v, b) and
  b.getFile().getAbsolutePath().matches("%/astro/%") and
  use = def.getAUse(v) and
  reach(def, v, use, true)
select use,
  "borrowed string bytes (held in '" + v.getName() +
    "', from $@) used after a may-GC call — stale under moving GC",
  b, "str->buf->data"
