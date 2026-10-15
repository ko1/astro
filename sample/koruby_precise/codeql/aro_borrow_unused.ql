/**
 * @name ARO_BORROW function that does not touch the interior
 * @description A function is marked ARO_BORROW (it claims to hand out a raw
 *              pointer into a movable GC object) but its body neither accesses a
 *              raw payload field (KorbStrBuf/KorbArrayItems ::data/::data_priv)
 *              nor calls another ARO_BORROW accessor.  The annotation is then a
 *              lie: it needlessly exempts the function from interior-encapsulation
 *              and makes borrow_after_gc treat its return as a borrow, producing
 *              false positives on its callers.  Remove ARO_BORROW.
 * @kind problem
 * @problem.severity warning
 * @id koruby/aro-borrow-unused
 */
import cpp

predicate isBorrowAccessor(Function f) {
  exists(MacroInvocation mi, FunctionDeclarationEntry fde |
    mi.getMacroName() = "ARO_BORROW" and fde = f.getADeclarationEntry() and
    mi.getFile() = fde.getFile() and
    mi.getLocation().getStartLine() = fde.getLocation().getStartLine())
}

predicate touchesInterior(Function f) {
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and
    fa.getTarget().getName() = ["data", "data_priv"] and
    fa.getTarget().getDeclaringType().getName() = ["KorbStrBuf", "KorbArrayItems"])
  or
  // the elements past the header of the object the payload pointer names (an embedded Array / Hash /
  // String is its own payload, so the accessor offsets from items_priv / buf instead of reading data_priv)
  exists(FieldAccess fa |
    fa.getEnclosingFunction() = f and
    fa.getTarget().getName() = ["items_priv", "buf"] and
    fa.getTarget().getDeclaringType().getName() = ["KorbArray", "KorbHash", "KorbString"])
  or
  exists(FunctionCall c | c.getEnclosingFunction() = f and isBorrowAccessor(c.getTarget()))
}

from Function f
where
  isBorrowAccessor(f) and
  f.hasDefinition() and
  not f.getFile().getAbsolutePath().matches("/usr/%") and
  not touchesInterior(f)
select f,
  "function '" + f.getName() +
    "' is marked ARO_BORROW but touches no raw payload / accessor — remove the annotation"
