/**
 * @name may-gc functions (transitive callers of korb_alloc)
 * @description Sanity query: infer the GC-effect of every function by the
 *              transitive closure of direct calls to the single GC seed
 *              korb_alloc.  Validates the DB + call-graph before the real
 *              borrow-lifetime check.
 * @kind problem
 * @problem.severity recommendation
 * @id koruby/may-gc
 */
import cpp

predicate calls(Function f, Function g) {
  exists(FunctionCall c | c.getEnclosingFunction() = f and c.getTarget() = g)
}

predicate mayGc(Function f) {
  f.hasName("korb_alloc")
  or
  exists(Function g | mayGc(g) and calls(f, g))
}

from Function f
where mayGc(f)
select f, "may-gc: transitively reaches korb_alloc"
