/**
 * @name Raw borrow escapes a non-accessor function
 * @description A raw interior pointer from an ARO_BORROW accessor (korb_str_data
 *              / korb_strbuf_data / korb_items_data) is returned from, or stored
 *              into long-lived memory by, a function that is NOT itself marked
 *              ARO_BORROW.  The caller then holds a borrow without knowing its
 *              (next-alloc) lifetime.  Fix: mark the function ARO_BORROW (if it
 *              is deliberately an accessor) or copy the bytes out.
 * @kind problem
 * @problem.severity warning
 * @id koruby/borrow-escape
 */
import cpp
import semmle.code.cpp.dataflow.new.DataFlow

predicate isBorrowAccessor(Function f) {
  exists(MacroInvocation mi, FunctionDeclarationEntry fde |
    mi.getMacroName() = "ARO_BORROW" and fde = f.getADeclarationEntry() and
    mi.getFile() = fde.getFile() and
    mi.getLocation().getStartLine() = fde.getLocation().getStartLine())
}

class BorrowCall extends FunctionCall { BorrowCall() { isBorrowAccessor(this.getTarget()) } }

from BorrowCall b, DataFlow::Node esc, Function f
where
  f = b.getEnclosingFunction() and
  not isBorrowAccessor(f) and
  b.getFile().getAbsolutePath().matches("%/astro/%") and
  DataFlow::localFlow(DataFlow::exprNode(b), esc) and
  (
    esc.asExpr() = any(ReturnStmt r).getExpr()
    or
    exists(AssignExpr a | a.getRValue() = esc.asExpr() and
      (a.getLValue() instanceof FieldAccess or
       a.getLValue().(VariableAccess).getTarget() instanceof GlobalOrNamespaceVariable))
  )
select esc,
  "raw borrow (from $@) escapes non-accessor '" + f.getName() +
    "' (returned/stored) — mark it ARO_BORROW or copy out",
  b, b.getTarget().getName()
