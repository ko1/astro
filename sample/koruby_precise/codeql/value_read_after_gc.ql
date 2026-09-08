/**
 * @name Any VALUE held in a local across a may-GC call
 * @description value_after_gc.ql only follows a VALUE that a may-GC call
 *              PRODUCED.  The far more common shape is a VALUE merely TAKEN OUT
 *              of somewhere — a slot, a parameter, a struct field — and then
 *              used after a collection.  Moving GC updates the slot, not the
 *              copy in the local, so the local is stale.  This widens the source
 *              to any non-constant VALUE expression.
 * @kind problem
 * @problem.severity error
 * @id koruby/value-read-after-gc
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

/** A VALUE-typed local whose definition is not a compile-time immediate. */
/* `v = KORB_NOT_REF(v)` states the value is an immediate from here on.  An
 * immediate never moves, so this definition is not movable and the chain the
 * query is looking for stops at it.  korb_not_ref is a real function in the
 * release build the database is extracted from, which is what makes the
 * annotation visible here at all. */
predicate notRefCall(Expr e) {
  exists(FunctionCall fc | fc = e.getAChild*() and fc.getTarget().hasName("korb_not_ref"))
}

predicate movableDef(SsaDefinition def, StackVariable v) {
  v.getType().getName() = "VALUE" and
  exists(Expr src |
    src = def.getDefiningValue(v) and not src.isConstant() and not notRefCall(src)
  )
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
  movableDef(def, v) and
  use.getFile().getAbsolutePath().matches("%/astro/%") and
  use = def.getAUse(v) and
  reach(def, v, use, true)
select use,
  use.getFile().getBaseName() + ":" + use.getLocation().getStartLine() +
  "  VALUE '" + v.getName() + "' read into a local, a may-GC call ran, used here — stale under moving GC"
