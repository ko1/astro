# Time — methods layered on the C core (strftime/subsec/to_i/Time.at/utc?).
class Time
  # Truncate/round the sub-second part to ndigits decimal places (ns precision).
  def floor(ndigits = 0)
    return self if ndigits >= 9
    whole = to_i
    if ndigits <= 0
      scale = 10 ** (-ndigits)
      whole = (whole / scale) * scale
      _time_at_ns(whole, 0)
    else
      ns = ((subsec * (10 ** ndigits)).floor * (10 ** (9 - ndigits)))
      _time_at_ns(whole, ns)
    end
  end

  def ceil(ndigits = 0)
    return self if ndigits >= 9
    whole = to_i
    sub = subsec
    if ndigits <= 0
      scale = 10 ** (-ndigits)
      total = whole + (sub > 0 ? 1 : 0)
      total = ((total + scale - 1) / scale) * scale
      _time_at_ns(total, 0)
    else
      scaled = (sub * (10 ** ndigits)).ceil
      _time_at_ns(whole, scaled * (10 ** (9 - ndigits)))
    end
  end

  # ISO-8601 / W3C XML Schema timestamp.  "Z" for UTC, else "+HH:MM".
  def iso8601(fraction_digits = 0)
    s = format("%04d-%02d-%02dT%02d:%02d:%02d", year, month, day, hour, min, sec)
    if fraction_digits && fraction_digits > 0
      frac = (subsec * (10 ** fraction_digits)).floor
      s += "." + frac.to_s.rjust(fraction_digits, "0")
    end
    s + (utc? ? "Z" : strftime("%:z"))
  end
  alias xmlschema iso8601

  def deconstruct_keys(keys)
    all = { year: year, month: month, day: day, yday: yday, wday: wday,
            hour: hour, min: min, sec: sec, subsec: subsec, dst: dst?, zone: zone }
    return all if keys.nil?
    h = {}
    keys.each { |k| h[k] = all[k] if all.key?(k) }
    h
  end

  private
  def _time_at_ns(whole, ns)
    t = Time.at(whole, ns, :nanosecond)
    utc? ? t.utc : t
  end
end

class Time
  # Marshal format: two little-endian 32-bit words.  The high bit of the first
  # marks the "broken-down" form — utc flag, year-1900, month-1, mday and hour
  # in the first word; min, sec and usec in the second.  A zone offset rides on
  # the payload String as @offset / @zone, which Marshal attaches before _load.
  def _dump(limit = -1)
    g = utc? ? self : getutc          # the broken-down fields are always UTC
    y = g.year
    raise ArgumentError, "year too big to marshal: #{y}" if y - 1900 > 0xffff || y - 1900 < 0
    p = (1 << 31) | ((utc? ? 1 : 0) << 30) | ((y - 1900) << 14) |
        ((g.mon - 1) << 10) | (g.mday << 5) | g.hour
    s = (g.min << 26) | (g.sec << 20) | g.usec
    str = [p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff, (p >> 24) & 0xff,
           s & 0xff, (s >> 8) & 0xff, (s >> 16) & 0xff, (s >> 24) & 0xff].pack("C*")
    str
  end

  def self._load(str)
    b = str.unpack("C*")
    raise TypeError, "marshaled time format differ" unless b.length >= 8
    p = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)
    s = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24)
    return Time.at(p, s) if (p & (1 << 31)).zero?      # plain epoch seconds + usec
    utc = !(p & (1 << 30)).zero?
    t = Time.utc(((p >> 14) & 0xffff) + 1900, ((p >> 10) & 0xf) + 1,
                 (p >> 5) & 0x1f, p & 0x1f,
                 (s >> 26) & 0x3f, (s >> 20) & 0x3f, s & 0xfffff)
    return t if utc
    t   # Marshal applies the :offset pseudo-ivar; a bare _load sees UTC fields
  end

  # The seconds/nanoseconds/zone live in these ivars; CRuby keeps them out of
  # reach in a native struct, so they must not show up as user ivars.  Named
  # exactly, so a user's own @__foo on a Time is still reported.
  TIME_INTERNAL_IVARS__ = %i[@__t @__ns @__utc @__off @__offx @__tz].freeze
  private_constant :TIME_INTERNAL_IVARS__
  def instance_variables = super - TIME_INTERNAL_IVARS__
end
