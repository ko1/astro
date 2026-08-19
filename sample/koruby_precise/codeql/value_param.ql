/**
 * @name Heap VALUE held in a local across a may-GC call
 * @description A VALUE produced by a may-GC call (potentially a movable heap
 *              object) is held in a plain C local, another may-GC call runs, then
 *              the SAME local is used.  Moving GC updates rooted slots (slots[],
 *              VALUE_REF cells) but NOT a bare local, so the local is stale.  The
 *              safe idiom stages into slots[] (an array element, not a
 *              StackVariable) and re-reads it, which this query does not flag.
 *              SSA-precise: a re-read (v = slots[i] again) is a new definition.
 * @kind problem
 * @problem.severity error
 * @id koruby/value-param-after-gc
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

/** A call returning a fresh, potentially-movable heap VALUE (a may-GC fn → VALUE). */
class HeapValueExpr extends FunctionCall {
  HeapValueExpr() {
    mayGcFn(this.getTarget()) and
    this.getType().getName() = "VALUE"
  }
}

/** The VALUE of `b` flows to `e` via value-preserving steps (conversions, local alias). */
predicate valueFlowsTo(Expr e, HeapValueExpr b) {
  e = b
  or
  valueFlowsTo(e.(Conversion).getExpr(), b)
  or
  exists(SsaDefinition d2, StackVariable v2 |
    e = d2.getAUse(v2) and
    v2.getType().getName() = "VALUE" and
    valueFlowsTo(d2.getDefiningValue(v2), b)
  )
}

/** SSA def `v = <heap VALUE>` — a movable VALUE held in local `v`. */
predicate valueDef(SsaDefinition def, StackVariable v, HeapValueExpr b) {
  v.getType().getName() = "VALUE" and
  valueFlowsTo(def.getDefiningValue(v), b)
}

/** EXPERIMENT: a VALUE that arrived as a parameter is just as movable. */
predicate paramDef(SsaDefinition def, StackVariable v) {
  v instanceof Parameter and
  v.getType().getName() = "VALUE" and
  def.getAUse(v) = v.getAnAccess()
}

predicate redefOf(SsaDefinition def, StackVariable v, ControlFlowNode n) {
  exists(SsaDefinition d2 |
    d2 != def and d2.getDefinition() = n and exists(d2.getDefiningValue(v))
  )
}

predicate reach(SsaDefinition def, StackVariable v, ControlFlowNode n, boolean sawGc) {
  n = def.getDefinition().getASuccessor() and
  (if mayGcNode(n) then sawGc = true else sawGc = false)
  or
  exists(ControlFlowNode prev, boolean s0 |
    reach(def, v, prev, s0) and
    not prev = def.getDefinition() and
    not redefOf(def, v, prev) and
    n = prev.getASuccessor() and
    (if mayGcNode(n) then sawGc = true else sawGc = s0)
  )
}

from SsaDefinition def, StackVariable v, VariableAccess use
where
  paramDef(def, v) and
  v.getFile().getAbsolutePath().matches("%/astro/%") and
  use = def.getAUse(v) and
  reach(def, v, use, true)
select use,
  "VALUE parameter '" + v.getName() + "' used after a may-GC call — stale under moving GC"
