/**
 * @name Interior payload access outside an ARO_BORROW accessor
 * @description Reaching into a movable GC object's raw layout
 *              (KorbStrBuf::data / KorbArrayItems::data) should happen ONLY in
 *              ARO_BORROW-marked (inline) accessor functions, so the internal
 *              representation can change by editing the accessors alone (and so
 *              the borrow-after-gc check has a single, complete source).  Any
 *              other function touching the raw payload is a violation.
 *
 *              This is a RATCHET: koruby currently has a large baseline of
 *              direct accesses (route them through ARO_BORROW inline accessors
 *              over time); the intent is to forbid NEW violations.
 * @kind problem
 * @problem.severity warning
 * @id koruby/interior-encapsulation
 */
import cpp

/** A raw pointer into a movable GC payload buffer. */
class PayloadField extends FieldAccess {
  PayloadField() {
    this.getTarget().getName() = ["data", "data_priv"] and
    this.getTarget().getDeclaringType().getName() = ["KorbStrBuf", "KorbArrayItems"]
  }
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

from PayloadField fa, Function f
where
  f = fa.getEnclosingFunction() and
  fa.getFile().getAbsolutePath().matches("%/astro/%") and
  not isBorrowAccessor(f)
select fa,
  "interior payload access outside an ARO_BORROW accessor (in " + f.getName() +
    ") — route through a marked inline accessor so the layout stays changeable"
