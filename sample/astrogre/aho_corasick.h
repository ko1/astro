/*
 * astrogre — Aho-Corasick multi-literal prefilter.
 *
 * Used by `node_grep_search_ac` when the regex pattern's leading edge
 * is an alternation of two or more literal byte strings whose first
 * bytes don't fit in the 8-entry `byteset` SIMD scanner (~10+ distinct
 * first bytes), or whose literals share prefixes.  The AC automaton
 * scans the haystack in a single pass and reports the start position
 * of each literal occurrence; the regex matcher then verifies from
 * that position.
 *
 * Build: O(total_pattern_length × 256) — done once per pattern at
 * `astrogre_parse` time.
 *
 * Scan: branchless inner loop, one indirect load per byte.  Failure
 * links are folded into the children table at build time so the scan
 * doesn't need an inner `while` to walk fail links.
 *
 * Memory: ~1 KiB per state.  A 12-way alt of 6-char literals builds
 * ~70 states (~70 KiB), comfortable per pattern.
 *
 * AOT: ASTro's operand encoding is scalar-only; we can't bake a trie
 * table into a code-store SD.  The `ac_t *` rides as an opaque void *
 * operand on the node and the table itself is heap-allocated, owned
 * by the pattern struct.  This is fine — the AC inner loop is data-
 * driven, so a static-const table and a heap table run identically;
 * the body chain that fires after an AC hit is what specialises.
 */

#ifndef ASTROGRE_AHO_CORASICK_H
#define ASTROGRE_AHO_CORASICK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct ac_t ac_t;

/* Build an AC automaton over `n` literal needles.  Each needle is
 * `needles[i]` of length `lens[i]` bytes.  Caller retains ownership
 * of the needle strings; the automaton copies what it needs into
 * the trie. */
ac_t *astrogre_ac_build(const char *const *needles, const uint32_t *lens, int n);

void  astrogre_ac_free(ac_t *ac);

/* Advance the scan from `*io_pos` until a literal output fires, or
 * `end` is reached.  On output, returns true and writes the
 * literal's START position to `*out_match_start`, with `*io_pos` and
 * `*io_state` updated so the next call resumes immediately after
 * the matched literal's last byte.  Returns false at end-of-haystack.
 *
 * `ac_handle` is `(ac_t *)`; the void* is what the search node
 * carries as its operand. */
bool  astrogre_ac_scan(void *ac_handle, const uint8_t *hay, size_t end,
                       size_t *io_pos, int32_t *io_state,
                       size_t *out_match_start);

#endif  /* ASTROGRE_AHO_CORASICK_H */
