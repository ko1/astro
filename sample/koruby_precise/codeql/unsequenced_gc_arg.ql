/**
 * @name may-GC call and movable VALUE in one argument list
 * @description C does not order the evaluation of a call's arguments.  When one
 *              argument can collect and another reads a VALUE the collector can
 *              move, the read may happen FIRST: the moved-from address is then
 *              passed in, and nothing will ever fix it up — the root scan
 *              updates the slot, not the copy already taken.  This is not a new
 *              rule, it is the codebase's existing "re-read after every
 *              allocation-capable call" seen from a place where no named local
 *              makes the lifetime visible, which is why value_after_gc.ql (it
 *              keys on StackVariable) cannot see it.  Order is exactly what the
 *              standard leaves open, so this query asks about CO-OCCURRENCE and
 *              never about control-flow order.
 *              Real instance: File::NULL's owner (builtins/file.c), fixed
 *              2026-09-07 — the stale owner was baked into vm->const_owners and
 *              later had a fake header stamped into the middle of Set's
 *              ancestor array.
 * @kind problem
 * @problem.severity error
 * @id koruby/unsequenced-gc-arg
 */
import cpp

predicate calls(Function f, Function g) {
  exists(FunctionCall c | c.getEnclosingFunction() = f and c.getTarget() = g)
}

predicate mayGcFn(Function f) {
  f.hasName("korb_alloc")
  or
  exists(Function g | mayGcFn(g) and calls(f, g))
}

/** `e` performs, at any depth, a call that can collect. */
predicate containsGc(Expr e) {
  exists(FunctionCall c | c = e.getAChild*() and mayGcFn(c.getTarget()))
}

/** A VALUE the collector can relocate: constants (KORB_NIL, LONG2FIX of a
 *  literal) are immediates and never move, so they are not interesting. */
predicate movableValueRead(Expr e) {
  e.getType().getName() = "VALUE" and
  not e.isConstant() and
  not containsGc(e)
}

from Call fc, Expr gcArg, Expr other
where
  fc.getFile().getAbsolutePath().matches("%/astro/%") and
  gcArg = fc.getAnArgument() and
  other = fc.getAnArgument() and
  gcArg != other and
  containsGc(gcArg) and
  (
    movableValueRead(other)
    or
    // two producers in one list: the first result is an unrooted temporary
    // while the second one collects.
    (containsGc(other) and other.getType().getName() = "VALUE" and
     gcArg.getFile().getLocation().getStartLine() >= 0 and
     exists(int i, int j |
       gcArg = fc.getArgument(i) and other = fc.getArgument(j) and i < j))
  )
select fc,
  "unsequenced: this argument list mixes a may-GC call ($@) with a movable VALUE ($@) — " +
  "C leaves their order open, so the VALUE may be read before the collection",
  gcArg, "may-gc argument", other, "movable VALUE"
