#!/usr/bin/env ruby
# frozen_string_literal: true
#
# unicode_tables.c generator.
#
#   ruby tools/gen_unicode_tables.rb > unicode_tables.c
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
