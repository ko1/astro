/* aro_gc_effect.h — GC-effect / borrow annotations for ASTro samples.
 *
 * Opt-in markers used by the CodeQL static checks (see a sample's codeql/ dir
 * and sample/koruby_precise/docs/c_ext_api_design.md §4.1).  They expand to
 * nothing under GCC (which has no `annotate` attribute — an unknown-attribute
 * warning would violate -Werror), and to Clang's `annotate` under Clang so a
 * clang-based extraction carries a richer signal.  The CodeQL queries key on
 * the *macro invocation* (via MacroInvocation), so detection works even under
 * the GCC build where the macros are empty.
 *
 *   ARO_MAYGC   this function may trigger a GC (allocates / runs managed code)
 *   ARO_NOGC    this function must not trigger a GC (verified contract)
 *   ARO_BORROW  this function's return value points INTO / AT a movable GC
 *               object; only ARO_BORROW-marked (inline) accessors may reach
 *               into a GC object's raw layout, so the representation can change
 *               by editing the accessors alone.  Place the marker on the same
 *               source line as the function declarator.
 */
#ifndef ARO_GC_EFFECT_H
#define ARO_GC_EFFECT_H 1

#if defined(__clang__)
#  define ARO_MAYGC  __attribute__((annotate("aro_maygc")))
#  define ARO_NOGC   __attribute__((annotate("aro_nogc")))
#  define ARO_BORROW __attribute__((annotate("aro_borrow")))
#else
#  define ARO_MAYGC
#  define ARO_NOGC
#  define ARO_BORROW
#endif

#endif /* ARO_GC_EFFECT_H */
