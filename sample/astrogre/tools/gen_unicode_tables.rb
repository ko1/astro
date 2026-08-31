#!/usr/bin/env ruby
# frozen_string_literal: true
#
# Unicode table generator.
#
#   ruby tools/gen_unicode_tables.rb       > unicode_tables.c
#   ruby tools/gen_unicode_tables.rb --gcb > unicode_gcb.h
#
# The data source is *CRuby itself*: for every property we scan the whole
# codepoint space with the host ruby's own Regexp and record the maximal
# runs.  No Unicode data file is needed and the result is guaranteed to
# agree with the CRuby we are trying to be a drop-in for (astrogre's
# `\p{...}` and `[[:...:]]` must answer exactly what `ruby` answers).
# Takes ~15 s.
#
# Surrogates (D800..DFFF) can't live in a UTF-8 String, so they are absent
# from the scan; they can never occur in a valid subject either.

require "rbconfig"

# All scannable codepoints, in order, as one String.  A maximal run of
# property members in this String is a maximal codepoint range (modulo the
# surrogate hole, which only ever splits a range in two).
CPS = (0..0x10FFFF).reject { |c| c.between?(0xD800, 0xDFFF) }.pack("U*")

def ranges_for(src)
  out = []
  CPS.scan(Regexp.new("(?:#{src})+")) { out << [$~[0][0].ord, $~[0][-1].ord] }
  # Re-join ranges that the surrogate hole split.
  out.each_with_object([]) do |(lo, hi), acc|
    if acc.last && acc.last[1] == 0xD7FF && lo == 0xE000
      acc.last[1] = hi
    else
      acc << [lo, hi]
    end
  end
end

def emit_ranges(out, name, ranges)
  out.puts "static const agre_cprange_t #{name}[] = {"
  ranges.each_slice(4) { |sl| out.puts "    " + sl.map { |lo, hi| format("{0x%X,0x%X},", lo, hi) }.join(" ") }
  out.puts "};"
  out.puts "#define #{name}_N #{ranges.size}"
end

# --gcb: grapheme-cluster-break tables for node_re_grapheme (`\X`).
#
# These live in their own header as *function-local* `static const` tables
# rather than in agre_uni_pool: a code-store SD is a standalone .so that
# cannot resolve libastrogre.so's unexported data, so it has to be able to
# compile its own copy.  Unused, the whole header costs nothing.
def emit_gcb(out)
  sets = {
    # GCB=Extend is Grapheme_Extend plus the emoji skin-tone modifiers.
    "agre_gcb_extend"  => '\p{Grapheme_Extend}|\p{Emoji_Modifier}',
    # Onigmo's \p{SpacingMark} is Mc; GCB=SpacingMark differs in ~20 chars.
    "agre_gcb_spacing" => '\p{SpacingMark}',
    "agre_gcb_ri"      => '\p{Regional_Indicator}',
    # GCB=Control minus CR / LF / ZWJ, which agre_gcb_class tests first.
    "agre_gcb_control" => '\p{Cc}|\p{Cf}|\p{Zl}|\p{Zp}',
    "agre_gcb_extpict" => '\p{Extended_Pictographic}',
  }
  out.puts <<~HDR
    /* Grapheme-cluster-break tables (UAX #29) — GENERATED, DO NOT EDIT.
     *
     * Regenerate with:  ruby tools/gen_unicode_tables.rb --gcb > unicode_gcb.h
     *
     * Derived from #{RUBY_DESCRIPTION.sub(/ \\+PRISM.*/, "")}
     * (Unicode #{RbConfig::CONFIG["UNICODE_VERSION"] || "?"}) — see tools/gen_unicode_tables.rb.
     */
    #ifndef ASTROGRE_UNICODE_GCB_H
    #define ASTROGRE_UNICODE_GCB_H 1

    #include "unicode_prop.h"

    /* Grapheme_Cluster_Break values, plus AGRE_GCB_EXTPICT — really the
     * separate Extended_Pictographic property, OR'd in for GB11. */
    enum agre_gcb {
        AGRE_GCB_OTHER = 0, AGRE_GCB_CR, AGRE_GCB_LF, AGRE_GCB_CONTROL,
        AGRE_GCB_EXTEND, AGRE_GCB_ZWJ, AGRE_GCB_RI, AGRE_GCB_PREPEND,
        AGRE_GCB_SPACINGMARK, AGRE_GCB_L, AGRE_GCB_V, AGRE_GCB_T,
        AGRE_GCB_LV, AGRE_GCB_LVT,
    };
    #define AGRE_GCB_EXTPICT 0x80
    #define AGRE_GCB_CLASS(x) ((x) & 0x7F)

    static inline int
    agre_gcb_class(uint32_t cp)
    {
  HDR
  sets.each { |name, src| emit_ranges(out, name, ranges_for(src)) }
  out.puts <<~'BODY'
    /* Prepend is not exposed by Onigmo, so this one list is hand-maintained
     * (Unicode 17.0 Grapheme_Cluster_Break=Prepend). */
    static const agre_cprange_t agre_gcb_prepend[] = {
        {0x600,0x605}, {0x6DD,0x6DD}, {0x70F,0x70F}, {0x890,0x891},
        {0x8E2,0x8E2}, {0xD4E,0xD4E}, {0x110BD,0x110BD}, {0x110CD,0x110CD},
        {0x111C2,0x111C3}, {0x1193F,0x1193F}, {0x11941,0x11941},
        {0x11A3A,0x11A3A}, {0x11A84,0x11A89}, {0x11D46,0x11D46},
        {0x11F02,0x11F02},
    };
    #define agre_gcb_prepend_N 15

    #define GCB_IN(tab, cp) \
        agre_cpset_contains(&(const agre_cpset_t){ tab, tab##_N }, (cp))

    const int ep = GCB_IN(agre_gcb_extpict, cp) ? AGRE_GCB_EXTPICT : 0;
    /* Latin-1 has no Extend / Prepend / jamo; only CR, LF and Cc matter. */
    if (cp < 0xA0 && ep == 0) {
        if (cp == 0x0D) return AGRE_GCB_CR;
        if (cp == 0x0A) return AGRE_GCB_LF;
        return cp < 0x20 || cp == 0x7F ? AGRE_GCB_CONTROL : AGRE_GCB_OTHER;
    }
    if (cp == 0x0D) return AGRE_GCB_CR | ep;
    if (cp == 0x0A) return AGRE_GCB_LF | ep;
    if (cp == 0x200D) return AGRE_GCB_ZWJ | ep;
    if (GCB_IN(agre_gcb_extend, cp)) return AGRE_GCB_EXTEND | ep;
    if (GCB_IN(agre_gcb_prepend, cp)) return AGRE_GCB_PREPEND | ep;
    if (GCB_IN(agre_gcb_spacing, cp)) return AGRE_GCB_SPACINGMARK | ep;
    if (GCB_IN(agre_gcb_ri, cp)) return AGRE_GCB_RI | ep;
    /* Hangul jamo and precomposed syllables live in fixed blocks. */
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0xA960 && cp <= 0xA97C)) return AGRE_GCB_L | ep;
    if ((cp >= 0x1160 && cp <= 0x11A7) || (cp >= 0xD7B0 && cp <= 0xD7C6)) return AGRE_GCB_V | ep;
    if ((cp >= 0x11A8 && cp <= 0x11FF) || (cp >= 0xD7CB && cp <= 0xD7FB)) return AGRE_GCB_T | ep;
    if (cp >= 0xAC00 && cp <= 0xD7A3)
        return (((cp - 0xAC00) % 28) == 0 ? AGRE_GCB_LV : AGRE_GCB_LVT) | ep;
    if (GCB_IN(agre_gcb_control, cp)) return AGRE_GCB_CONTROL | ep;
    return AGRE_GCB_OTHER | ep;
    #undef GCB_IN
    }

    #endif /* ASTROGRE_UNICODE_GCB_H */
  BODY
end

if ARGV.include?("--gcb")
  emit_gcb($stdout)
  exit
end

# `\p{...}` names.  General categories (one- and two-letter), the long
# names Ruby accepts, and the scripts.  Cs is empty by construction (no
# surrogate can appear in the scan) so it is hard-coded.
CATEGORIES = %w[L Lu Ll Lt Lm Lo M Mn Mc Me N Nd Nl No
                P Pc Pd Ps Pe Pi Pf Po S Sm Sc Sk So
                Z Zs Zl Zp C Cc Cf Co Cn]
LONG_NAMES = %w[Alpha Word Space Upper Lower Alnum Graph Print Punct
                Digit Cntrl Blank XDigit ASCII Any Assigned
                Alphabetic Uppercase Lowercase White_Space Newline]
SCRIPTS    = %w[Latin Greek Cyrillic Armenian Hebrew Arabic Syriac Thaana
                Devanagari Bengali Gurmukhi Gujarati Oriya Tamil Telugu
                Kannada Malayalam Sinhala Thai Lao Tibetan Myanmar Georgian
                Hangul Ethiopic Cherokee Ogham Runic Khmer Mongolian
                Hiragana Katakana Bopomofo Han Yi Common Inherited
                Greek_And_Coptic Coptic Deseret Gothic Old_Italic
                Braille Cypriot Limbu Osmanya Shavian Linear_B Tai_Le
                Ugaritic Buginese Glagolitic Kharoshthi Syloti_Nagri
                New_Tai_Lue Tifinagh Old_Persian Balinese Cuneiform
                Nko Phags_Pa Phoenician]
# POSIX bracket classes — `[[:name:]]`.  Not always the same set as the
# same-named `\p{}` property (e.g. [[:punct:]] also takes the ASCII
# symbol characters that \p{Punct} rejects), so they get their own list.
POSIX = %w[alpha digit alnum upper lower xdigit space blank
           print graph cntrl punct ascii word]

pool = []          # flat [lo, hi] pool
pool_index = {}    # range-list -> offset (dedup identical lists)

def normalize(name) = name.downcase.delete("-_ ")

intern = lambda do |ranges|
  key = ranges
  off = pool_index[key]
  unless off
    off = pool.size
    pool_index[key] = off
    pool.concat(ranges)
  end
  [off, ranges.size]
end

props = []
(CATEGORIES + LONG_NAMES + SCRIPTS).each do |name|
  ranges = ranges_for("\\p{#{name}}")
  next if ranges.empty?
  off, len = intern.call(ranges)
  props << [normalize(name), off, len]
rescue RegexpError
  warn "skip \\p{#{name}}: not supported by #{RUBY_DESCRIPTION}"
end
props << [normalize("Cs"), *intern.call([[0xD800, 0xDFFF]])]
props.uniq! { |p| p[0] }
props.sort_by! { |p| p[0] }

posix = POSIX.map do |name|
  [name, *intern.call(ranges_for("[[:#{name}:]]"))]
end.sort_by { |p| p[0] }

out = $stdout
out.puts <<~HDR
  /* Unicode property tables — GENERATED, DO NOT EDIT.
   *
   * Regenerate with:  ruby tools/gen_unicode_tables.rb > unicode_tables.c
   *
   * Derived from #{RUBY_DESCRIPTION.sub(/ \+PRISM.*/, "")}'s own Regexp engine
   * (Unicode #{RbConfig::CONFIG["UNICODE_VERSION"] || "?"}) by scanning the whole
   * codepoint space per property — see tools/gen_unicode_tables.rb.
   */
  #include "unicode_prop.h"

  const agre_cprange_t agre_uni_pool[] = {
HDR
pool.each_slice(4) do |slice|
  out.puts "    " + slice.map { |lo, hi| format("{0x%X,0x%X},", lo, hi) }.join(" ")
end
out.puts "};"
out.puts "const unsigned agre_uni_pool_n = #{pool.size};"
out.puts
out.puts "/* `\\p{...}` names, normalized (lowercase, no `-`/`_`/space), sorted. */"
out.puts "const agre_uni_prop_t agre_uni_props[] = {"
props.each { |n, off, len| out.puts format("    {%-18s %6d, %4d},", "\"#{n}\",", off, len) }
out.puts "};"
out.puts "const unsigned agre_uni_props_n = #{props.size};"
out.puts
out.puts "/* POSIX bracket classes `[[:name:]]`, sorted. */"
out.puts "const agre_uni_prop_t agre_posix_props[] = {"
posix.each { |n, off, len| out.puts format("    {%-18s %6d, %4d},", "\"#{n}\",", off, len) }
out.puts "};"
out.puts "const unsigned agre_posix_props_n = #{posix.size};"

warn "pool=#{pool.size} ranges, props=#{props.size}, posix=#{posix.size}"
