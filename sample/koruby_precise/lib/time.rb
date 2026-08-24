# 'time' — Time.parse and the RFC/ISO parsers, on top of Date._parse.
require 'date'

class Time
  # Named zones the RFC-822 grammar allows (RFC 2822 declares them obsolete but
  # still parseable); everything else must be a numeric offset.
  RFC2822_ZONES__ = {
    'UT' => 0, 'GMT' => 0, 'UTC' => 0, 'Z' => 0,
    'EST' => -5 * 3600, 'EDT' => -4 * 3600, 'CST' => -6 * 3600, 'CDT' => -5 * 3600,
    'MST' => -7 * 3600, 'MDT' => -6 * 3600, 'PST' => -8 * 3600, 'PDT' => -7 * 3600,
  }.freeze
  private_constant :RFC2822_ZONES__

  class << self
    # A parsed-fields Hash → Time.  A known offset gives a UTC-based time shifted
    # back by that offset; without one the fields are local.
    private def __from_parsed(h, now)
      year = h[:year] || now.year
      mon  = h[:mon]  || (h[:year] ? 1 : now.month)
      mday = h[:mday] || ((h[:year] || h[:mon]) ? 1 : now.day)
      hour = h[:hour] || 0
      min  = h[:min]  || 0
      sec  = h[:sec]  || 0
      sec += h[:sec_fraction].to_r if h[:sec_fraction]
      off = h[:offset]
      if off
        utc(year, mon, mday, hour, min, sec) - off
      else
        local(year, mon, mday, hour, min, sec)
      end
    end

    def parse(date, now = Time.now)
      raise TypeError, "no implicit conversion of #{date.class} into String" unless date.is_a?(String)
      h = Date._parse(date, true)
      raise ArgumentError, "no time information in #{date.inspect}" if h.empty?
      __from_parsed(h, now)
    end

    def strptime(date, format, now = Time.now)
      h = Date._strptime(date, format)
      raise ArgumentError, "invalid date or strptime format - '#{date}' '#{format}'" unless h
      __from_parsed(h, now)
    end

    def xmlschema(time)
      raise TypeError, "no implicit conversion of #{time.class} into String" unless time.is_a?(String)
      m = /\A\s*(-?\d{4,})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})(\.\d+)?(Z|[-+]\d{2}:?\d{2})?\s*\z/.match(time)
      raise ArgumentError, "invalid xmlschema format: #{time.inspect}" unless m
      sec = m[6].to_i
      sec += Rational(m[7][1..].to_i, 10**(m[7].length - 1)) if m[7]
      if m[8].nil?
        local(m[1].to_i, m[2].to_i, m[3].to_i, m[4].to_i, m[5].to_i, sec)
      else
        off = m[8] == 'Z' ? 0 : __zone_offset_str(m[8])
        utc(m[1].to_i, m[2].to_i, m[3].to_i, m[4].to_i, m[5].to_i, sec) - off
      end
    end
    alias_method :iso8601, :xmlschema

    private def __zone_offset_str(s)
      sign = s[0] == '-' ? -1 : 1
      digits = s[1..].delete(':')
      sign * (digits[0, 2].to_i * 3600 + digits[2, 2].to_i * 60)
    end

    # RFC 2822: "[Day, ]DD Mon YYYY HH:MM[:SS] ZONE"; a 2-digit year follows the
    # RFC's own rule (00..49 → 20xx, 50..99 → 19xx), not Date's 69-rule.
    def rfc2822(date)
      raise TypeError, "no implicit conversion of #{date.class} into String" unless date.is_a?(String)
      # folding whitespace is allowed around the ':' of the time-of-day
      m = /\A\s*(?:[A-Za-z]{3},\s*)?(\d{1,2})\s+([A-Za-z]{3})\s+(-?\d{2,})\s+
           (\d{1,2})\s*:\s*(\d{2})(?:\s*:\s*(\d{2}))?\s*
           ([-+]\d{4}|[A-Za-z]{1,3})?\s*(?:\(.*\))?\s*\z/x.match(date)
      raise ArgumentError, "not RFC 2822 compliant date: #{date.inspect}" unless m
      year = m[3].to_i
      year += year < 50 ? 2000 : 1900 if m[3].length == 2
      mon = Date::ABBR_MONTHNAMES.index { |x| x && x.casecmp?(m[2]) }
      raise ArgumentError, "not RFC 2822 compliant date: #{date.inspect}" unless mon
      off = if m[7].nil? then 0
            elsif m[7].start_with?('-', '+') then __zone_offset_str(m[7])
            else RFC2822_ZONES__[m[7].upcase] or
                 raise ArgumentError, "not RFC 2822 compliant date: #{date.inspect}"
            end
      utc(year, mon, m[1].to_i, m[4].to_i, m[5].to_i, m[6].to_i) - off
    end
    alias_method :rfc822, :rfc2822

    # RFC 2616 allows three shapes; the asctime one has no zone and is UTC.
    def httpdate(date)
      raise TypeError, "no implicit conversion of #{date.class} into String" unless date.is_a?(String)
      if (m = /\A\s*[A-Za-z]{3},\s(\d{2})\s([A-Za-z]{3})\s(\d{4})\s
               (\d{2}):(\d{2}):(\d{2})\sGMT\s*\z/x.match(date))
        mon = Date::ABBR_MONTHNAMES.index { |x| x && x.casecmp?(m[2]) }
        return utc(m[3].to_i, mon, m[1].to_i, m[4].to_i, m[5].to_i, m[6].to_i)
      end
      if (m = /\A\s*[A-Za-z]{3}\s([A-Za-z]{3})\s+(\d{1,2})\s
               (\d{2}):(\d{2}):(\d{2})\s(\d{4})\s*\z/x.match(date))
        mon = Date::ABBR_MONTHNAMES.index { |x| x && x.casecmp?(m[1]) }
        return utc(m[6].to_i, mon, m[2].to_i, m[3].to_i, m[4].to_i, m[5].to_i)
      end
      if (m = /\A\s*[A-Za-z]+,\s(\d{2})-([A-Za-z]{3})-(\d{2})\s
               (\d{2}):(\d{2}):(\d{2})\sGMT\s*\z/x.match(date))
        mon = Date::ABBR_MONTHNAMES.index { |x| x && x.casecmp?(m[2]) }
        yy = m[3].to_i
        return utc(yy < 70 ? 2000 + yy : 1900 + yy, mon, m[1].to_i,
                   m[4].to_i, m[5].to_i, m[6].to_i)
      end
      raise ArgumentError, "not RFC 2616 compliant date: #{date.inspect}"
    end
  end

  def iso8601(fraction_digits = 0)
    frac = fraction_digits > 0 ? ".#{'%0*d' % [fraction_digits, (subsec * 10**fraction_digits).to_i]}" : ""
    strftime("%Y-%m-%dT%H:%M:%S") + frac + (utc? ? "Z" : strftime("%z").insert(3, ":"))
  end
  alias xmlschema iso8601

  def rfc2822 = strftime("%a, %-d %b %Y %H:%M:%S ") + (utc? ? "-0000" : strftime("%z"))
  alias rfc822 rfc2822

  def httpdate = getutc.strftime("%a, %d %b %Y %H:%M:%S GMT")
end
