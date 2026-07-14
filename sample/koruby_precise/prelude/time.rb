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

  # koruby has no DST tracking (UTC/localtime only) — report standard time.
  def isdst; false; end
  alias dst? isdst

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
