# Pure-Ruby Date / DateTime.
#
# A date is stored as a civil Julian Day Number plus a local day offset, which
# keeps every calendar operation integer arithmetic; the Rational-valued
# astronomical JD (#ajd) is derived on demand.  The calendar-reform day (@sg,
# Date::ITALY by default) selects Julian vs Gregorian per instance, exactly as
# the C implementation does.
class Date
  include Comparable

  ITALY     = 2299161          # 1582-10-15
  ENGLAND   = 2361222          # 1752-09-14
  JULIAN    = Float::INFINITY
  GREGORIAN = -Float::INFINITY

  # Both the arrays and the names in them are frozen, as in the C library.
  MONTHNAMES = [nil, "January", "February", "March", "April", "May", "June",
                "July", "August", "September", "October", "November", "December"]
               .map { |s| s&.freeze }.freeze
  ABBR_MONTHNAMES = [nil, "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
                    .map { |s| s&.freeze }.freeze
  DAYNAMES = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
             .map(&:freeze).freeze
  ABBR_DAYNAMES = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].map(&:freeze).freeze

  DAYS_IN_MONTH = [nil, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31].freeze

  class Error < ArgumentError; end

  # Date::Infinity — the sentinel the C library uses for an open-ended calendar
  # reform day.  `d` is +1 / -1 / 0 (the last one behaving as NaN).
  class Infinity < Numeric
    include Comparable

    def initialize(d = 1)
      @d = d <=> 0
    end

    def d = @d
    def zero? = false
    def finite? = false
    def infinite? = @d.nonzero?
    def nan? = @d.zero?
    def abs = self.class.new
    def -@ = self.class.new(-@d)
    def +@ = self.class.new(@d)
    def to_f = @d.zero? ? Float::NAN : (@d > 0 ? Float::INFINITY : -Float::INFINITY)

    def <=>(other)
      case other
      when Infinity then @d <=> other.d
      when Numeric  then to_f <=> other
      else nil
      end
    end

    def coerce(other)
      return [-@d, @d] if other.is_a?(Numeric)
      super
    end
  end

  # ---- calendar conversions (module functions, as in the C version) -------

  def self.gregorian_leap?(y) = (y % 4).zero? && (!(y % 100).zero? || (y % 400).zero?)
  def self.julian_leap?(y) = (y % 4).zero?
  def self.leap?(y) = gregorian_leap?(y)

  # Julian-vs-Gregorian tests compare a jd against the reform day; a
  # Date::Infinity start has to become a Float first.
  def self.__sgnum(sg) = sg.is_a?(Infinity) ? sg.to_f : sg

  def self.civil_to_jd(y, m, d, sg = GREGORIAN)
    sg = __sgnum(sg)
    if m <= 2
      y -= 1
      m += 12
    end
    a = (y / 100.0).floor
    b = 2 - a + (a / 4.0).floor
    jd = (365.25 * (y + 4716)).floor + (30.6001 * (m + 1)).floor + d + b - 1524
    jd < sg ? jd - b : jd
  end

  def self.jd_to_civil(jd, sg = GREGORIAN)
    sg = __sgnum(sg)
    if jd < sg
      a = jd
    else
      x = ((jd - 1867216.25) / 36524.25).floor
      a = jd + 1 + x - (x / 4.0).floor
    end
    b = a + 1524
    c = ((b - 122.1) / 365.25).floor
    d = (365.25 * c).floor
    e = ((b - d) / 30.6001).floor
    dom = b - d - (30.6001 * e).floor
    if e <= 13
      [c - 4716, e - 1, dom]
    else
      [c - 4715, e - 13, dom]
    end
  end

  def self.ordinal_to_jd(y, d, sg = GREGORIAN) = civil_to_jd(y, 1, 1, sg) + d - 1

  def self.jd_to_ordinal(jd, sg = GREGORIAN)
    y = jd_to_civil(jd, sg)[0]
    [y, jd - civil_to_jd(y, 1, 1, sg) + 1]
  end

  def self.commercial_to_jd(y, w, d, sg = GREGORIAN)
    # January 4th always falls in ISO week 1, so its Monday anchors the year.
    j = civil_to_jd(y, 1, 4, sg)
    (j - (j % 7)) + 7 * (w - 1) + (d - 1)
  end

  def self.jd_to_commercial(jd, sg = GREGORIAN)
    a = jd_to_civil(jd - 3, sg)[0]
    y = commercial_to_jd(a + 1, 1, 1, sg) <= jd ? a + 1 : a
    w = 1 + (jd - commercial_to_jd(y, 1, 1, sg)) / 7
    d = (jd + 1) % 7
    d = 7 if d.zero?
    [y, w, d]
  end

  def self.jd_to_wday(jd) = (jd + 1) % 7
  def self.jd_to_mjd(jd) = jd - 2400001
  def self.mjd_to_jd(mjd) = mjd + 2400001
  def self.jd_to_ld(jd) = jd - 2299160
  def self.ld_to_jd(ld) = ld + 2299160
  def self.jd_to_ajd(jd, fr, of = 0) = jd + fr - of - Rational(1, 2)

  def self.ajd_to_jd(ajd, of = 0)
    r = ajd + of + Rational(1, 2)
    jd = r.floor
    [jd, r - jd]
  end

  def self.amjd_to_ajd(amjd) = amjd + Rational(4800001, 2)
  def self.ajd_to_amjd(ajd) = ajd - Rational(4800001, 2)
  def self.day_fraction_to_time(fr)
    ss = fr * 86400
    h = (ss / 3600).floor
    ss -= h * 3600
    mi = (ss / 60).floor
    ss -= mi * 60
    s = ss.floor
    [h, mi, s, ss - s]
  end

  def self.time_to_day_fraction(h, min, s) = Rational(h * 3600 + min * 60 + s, 86400)

  def self.valid_civil?(y, m, d, sg = ITALY)
    jd = _valid_civil_jd(y, m, d, sg)
    !jd.nil?
  end
  class << self; alias_method :valid_date?, :valid_civil?; end

  def self.valid_ordinal?(y, d, sg = ITALY) = !_valid_ordinal_jd(y, d, sg).nil?
  def self.valid_commercial?(y, w, d, sg = ITALY) = !_valid_commercial_jd(y, w, d, sg).nil?
  def self.valid_jd?(jd, sg = ITALY) = jd.is_a?(Numeric)

  def self._valid_civil_jd(y, m, d, sg)
    raise Date::Error, "invalid date" unless y.is_a?(Integer) && m.is_a?(Integer) && d.is_a?(Integer)
    return nil if m.zero? || d.zero?
    m += 13 if m < 0
    return nil if m < 1 || m > 12
    last = _month_days(y, m, sg)
    d = last + d + 1 if d < 0
    return nil if d < 1 || d > last
    jd = civil_to_jd(y, m, d, sg)
    jd_to_civil(jd, sg) == [y, m, d] ? jd : nil
  end

  def self._valid_ordinal_jd(y, d, sg)
    return nil if d.zero?
    ylen = (_leap?(y, sg) ? 366 : 365)
    d = ylen + d + 1 if d < 0
    return nil if d < 1 || d > ylen
    ordinal_to_jd(y, d, sg)
  end

  def self._valid_commercial_jd(y, w, d, sg)
    d = 8 + d if d < 0
    return nil if d < 1 || d > 7
    if w < 0
      w = jd_to_commercial(commercial_to_jd(y + 1, 1, 1, sg) - 1, sg)[1] + w + 1
    end
    return nil if w < 1 || w > 53
    jd = commercial_to_jd(y, w, d, sg)
    jd_to_commercial(jd, sg)[0, 2] == [y, w] ? jd : nil
  end

  def self._leap?(y, sg)
    # A year is Julian-leap before the reform day, Gregorian-leap after it.
    civil_to_jd(y, 3, 1, sg) - civil_to_jd(y, 2, 1, sg) == 29
  end

  def self._month_days(y, m, sg)
    return 29 if m == 2 && _leap?(y, sg)
    DAYS_IN_MONTH[m]
  end

  # ---- constructors ------------------------------------------------------

  def self.jd(jd = 0, sg = ITALY)
    jd = jd.to_int if jd.respond_to?(:to_int) && !jd.is_a?(Integer)
    new!(jd, 0, 0, 0, sg)
  end

  def self.civil(y = -4712, m = 1, d = 1, sg = ITALY)
    jd = _valid_civil_jd(y, m, d, sg) or raise Date::Error, "invalid date"
    new!(jd, 0, 0, 0, sg)
  end
  class << self; alias_method :new, :civil; end

  def self.ordinal(y = -4712, d = 1, sg = ITALY)
    jd = _valid_ordinal_jd(y, d, sg) or raise Date::Error, "invalid date"
    new!(jd, 0, 0, 0, sg)
  end

  def self.commercial(y = -4712, w = 1, d = 1, sg = ITALY)
    jd = _valid_commercial_jd(y, w, d, sg) or raise Date::Error, "invalid date"
    new!(jd, 0, 0, 0, sg)
  end

  def self.today(sg = ITALY)
    t = Time.now
    civil(t.year, t.month, t.day, sg)
  end

  # Internal all-fields constructor: jd + seconds/nanoseconds within the local
  # day + the local offset in seconds.
  def self.new!(jd, df = 0, sf = 0, of = 0, sg = ITALY)
    d = allocate
    d.__setup(jd, df, sf, of, sg)
    d
  end

  def __setup(jd, df, sf, of, sg)
    @jd = jd
    @df = df
    @sf = sf
    @of = of
    @sg = sg
    self
  end

  protected def __df = @df
  protected def __sf = @sf

  # ---- field readers -----------------------------------------------------

  def jd = @jd
  def mjd = Date.jd_to_mjd(@jd)
  def ld = Date.jd_to_ld(@jd)

  def ajd = @jd + day_fraction - Rational(@of, 86400) - Rational(1, 2)
  def amjd = Date.ajd_to_amjd(ajd)

  def day_fraction = Rational(@df, 86400) + Rational(@sf, 86400 * 1_000_000_000)

  def year = __civil[0]
  def mon = __civil[1]
  alias_method :month, :mon
  def mday = __civil[2]
  alias_method :day, :mday
  def yday = Date.jd_to_ordinal(@jd, @sg)[1]
  def wday = Date.jd_to_wday(@jd)
  def cwyear = Date.jd_to_commercial(@jd, @sg)[0]
  def cweek = Date.jd_to_commercial(@jd, @sg)[1]
  def cwday = Date.jd_to_commercial(@jd, @sg)[2]

  def sunday? = wday == 0
  def monday? = wday == 1
  def tuesday? = wday == 2
  def wednesday? = wday == 3
  def thursday? = wday == 4
  def friday? = wday == 5
  def saturday? = wday == 6

  def leap? = Date._leap?(year, @sg)
  def start = @sg.is_a?(Numeric) ? @sg.to_f : @sg   # a Float even for the integral reform days
  def julian? = @jd < Date.__sgnum(@sg)
  def gregorian? = !julian?
  def fix_style = self

  def new_start(sg = Date::ITALY) = self.class.new!(@jd, @df, @sf, @of, sg)
  def italy = new_start(Date::ITALY)
  def england = new_start(Date::ENGLAND)
  def julian = new_start(Date::JULIAN)
  def gregorian = new_start(Date::GREGORIAN)

  def hour = 0
  def min = 0
  def sec = 0
  def sec_fraction = 0
  def offset = Rational(@of, 86400)
  def zone = Date.__zone_str(@of)

  private def __civil = (@civil ||= Date.jd_to_civil(@jd, @sg))

  def self.__zone_str(of)
    sign = of < 0 ? "-" : "+"
    a = of.abs
    format("%s%02d:%02d", sign, a / 3600, (a % 3600) / 60)
  end

  # ---- arithmetic --------------------------------------------------------

  def +(other)
    case other
    when Integer  then self.class.new!(@jd + other, @df, @sf, @of, @sg)
    when Numeric
      __plus_fraction(other)
    else raise TypeError, "expected numeric"
    end
  end

  def -(other)
    case other
    when Integer then self.class.new!(@jd - other, @df, @sf, @of, @sg)
    when Date    then ajd - other.ajd
    when Numeric then __plus_fraction(-other)
    else raise TypeError, "expected numeric or date"
    end
  end

  private def __plus_fraction(fr)
    total = Rational(@jd) + day_fraction + fr
    jd = total.floor
    rest = total - jd
    ns = (rest * 86400 * 1_000_000_000).round
    df, sf = ns.divmod(1_000_000_000)
    self.class.new!(jd, df, sf, @of, @sg)
  end

  # Month arithmetic clamps to the end of the target month, as Ruby does.
  def >>(n)
    y, m, d = __civil
    m += n
    y += (m - 1).div(12)
    m = (m - 1) % 12 + 1
    last = Date._month_days(y, m, @sg)
    d = last if d > last
    self.class.new!(Date.civil_to_jd(y, m, d, @sg), @df, @sf, @of, @sg)
  end

  def <<(n) = self >> -n

  def next_day(n = 1) = self + n
  def prev_day(n = 1) = self - n
  def next_month(n = 1) = self >> n
  def prev_month(n = 1) = self << n
  def next_year(n = 1) = self >> (n * 12)
  def prev_year(n = 1) = self << (n * 12)
  def succ = next_day
  alias_method :next, :succ

  def upto(maximum, &block)
    return to_enum(:upto, maximum) unless block
    d = self
    while d <= maximum
      block.call(d)
      d = d.next_day
    end
    self
  end

  def downto(minimum, &block)
    return to_enum(:downto, minimum) unless block
    d = self
    while d >= minimum
      block.call(d)
      d = d.prev_day
    end
    self
  end

  def step(limit, by = 1, &block)
    return to_enum(:step, limit, by) unless block
    raise ArgumentError, "step can't be 0" if by.zero?
    d = self
    if by > 0
      while d <= limit
        block.call(d)
        d += by
      end
    else
      while d >= limit
        block.call(d)
        d += by
      end
    end
    self
  end

  # ---- comparison --------------------------------------------------------

  def <=>(other)
    case other
    when Date    then ajd <=> other.ajd
    when Numeric then ajd <=> other
    else
      return nil unless other.respond_to?(:ajd)
      ajd <=> other.ajd
    end
  end

  def ==(other) = other.is_a?(Date) ? (ajd == other.ajd) : false
  def eql?(other) = other.is_a?(Date) && self.class == other.class && ajd == other.ajd
  def hash = ajd.hash

  def ===(other)
    case other
    when Date    then jd == other.jd
    when Numeric then jd == other
    else false
    end
  end

  def deconstruct_keys(keys)
    all = { year: year, month: month, day: day, yday: yday, wday: wday }
    return all if keys.nil?
    h = {}
    keys.each { |k| h[k] = all[k] if all.key?(k) }
    h
  end

  # ---- conversion --------------------------------------------------------

  def to_date = self

  def to_datetime
    DateTime.new!(@jd, @df, @sf, @of, @sg)
  end

  def to_time
    y, m, d = __civil
    Time.local(y, m, d)
  end

  def to_s = format("%04d-%02d-%02d", year, mon, mday)

  def inspect
    format("#<%s: %s ((%sj,%ds,%dn),%+ds,%sj)>",
           self.class.name, to_s, jd.to_s, @df, @sf, @of, __sg_str)
  end

  private def __sg_str
    return "-Infinity" if @sg == GREGORIAN
    return "Infinity" if @sg == JULIAN
    @sg.to_s
  end

  def iso8601 = to_s
  alias_method :xmlschema, :iso8601
  def rfc3339 = format("%04d-%02d-%02dT%02d:%02d:%02d%s", year, mon, mday, hour, min, sec, zone)
  def rfc2822 = strftime("%a, %-d %b %Y %T %z")
  alias_method :rfc822, :rfc2822
  def httpdate = strftime("%a, %d %b %Y %T GMT")
  def jisx0301 = __jisx0301_date
  def ctime = strftime("%a %b %e %T %Y")
  alias_method :asctime, :ctime

  private def __jisx0301_date
    y, m, d = __civil
    if @jd < 2405160          # before Meiji 6-01-01 → plain Gregorian
      to_s
    else
      era, base = if @jd >= 2458605 then ["R", 2018]
                  elsif @jd >= 2447535 then ["H", 1988]
                  elsif @jd >= 2424875 then ["S", 1925]
                  else ["M", 1867]
                  end
      format("%s%02d.%02d.%02d", era, y - base, m, d)
    end
  end

  # ---- strftime ----------------------------------------------------------

  def strftime(fmt = "%F")
    out = +""
    i = 0
    n = fmt.length
    while i < n
      ch = fmt[i]
      if ch != "%"
        out << ch
        i += 1
        next
      end
      i += 1
      break if i >= n
      # flags and width
      flags = +""
      while i < n && "-_0^#:".include?(fmt[i])
        flags << fmt[i]
        i += 1
      end
      width = +""
      while i < n && fmt[i] >= "0" && fmt[i] <= "9"
        width << fmt[i]
        i += 1
      end
      break if i >= n
      out << __strftime_conv(fmt[i], flags, width.empty? ? nil : width.to_i)
      i += 1
    end
    out
  end

  private def __pad(v, w, flags, zero = true)
    s = v.abs.to_s
    neg = v.is_a?(Numeric) && v < 0
    if flags.include?("-")
      # no padding
    else
      pad = flags.include?("_") ? " " : (zero ? "0" : " ")
      s = pad * (w - s.length) + s if w && s.length < w
    end
    (neg ? "-" : "") + s
  end

  private def __strftime_conv(c, flags, width)
    y, m, d = __civil
    case c
    when "Y" then __pad(y, width || 4, flags)
    when "C" then __pad(y / 100, width || 2, flags)
    when "y" then __pad(y % 100, width || 2, flags)
    when "m" then __pad(m, width || 2, flags)
    when "d" then __pad(d, width || 2, flags)
    when "e" then __pad(d, width || 2, flags.include?("-") ? flags : flags + "_")
    when "j" then __pad(yday, width || 3, flags)
    when "H" then __pad(hour, width || 2, flags)
    when "k" then __pad(hour, width || 2, flags + "_")
    when "I" then __pad((hour % 12).zero? ? 12 : hour % 12, width || 2, flags)
    when "l" then __pad((hour % 12).zero? ? 12 : hour % 12, width || 2, flags + "_")
    when "M" then __pad(min, width || 2, flags)
    when "S" then __pad(sec, width || 2, flags)
    when "L" then __pad((sec_fraction * 1000).to_i, width || 3, flags)
    when "N" then __pad((sec_fraction * 1_000_000_000).to_i, width || 9, flags)
    when "P" then hour < 12 ? "am" : "pm"
    when "p" then hour < 12 ? "AM" : "PM"
    when "A" then __case(DAYNAMES[wday], flags)
    when "a" then __case(ABBR_DAYNAMES[wday], flags)
    when "B" then __case(MONTHNAMES[m], flags)
    when "b", "h" then __case(ABBR_MONTHNAMES[m], flags)
    when "u" then (wday.zero? ? 7 : wday).to_s
    when "w" then wday.to_s
    when "G" then __pad(cwyear, width || 4, flags)
    when "g" then __pad(cwyear % 100, width || 2, flags)
    when "V" then __pad(cweek, width || 2, flags)
    when "U" then __pad((yday + 6 - wday) / 7, width || 2, flags)
    when "W" then __pad((yday + 6 - (wday.zero? ? 6 : wday - 1)) / 7, width || 2, flags)
    when "Z" then zone
    when "z" then __zone_offset(flags)
    when "s" then __epoch_seconds.to_s
    when "n" then "\n"
    when "t" then "\t"
    when "%" then "%"
    when "F" then format("%04d-%02d-%02d", y, m, d)
    when "D", "x" then format("%02d/%02d/%02d", m, d, y % 100)
    when "T", "X" then format("%02d:%02d:%02d", hour, min, sec)
    when "R" then format("%02d:%02d", hour, min)
    when "r" then format("%02d:%02d:%02d %s", (hour % 12).zero? ? 12 : hour % 12, min, sec, hour < 12 ? "AM" : "PM")
    when "c" then strftime("%a %b %e %H:%M:%S %Y")
    when "+" then strftime("%a %b %e %H:%M:%S %Z %Y")
    else "%" + flags + (width ? width.to_s : "") + c
    end
  end

  private def __epoch_seconds = ((ajd - Rational(4881175, 2)) * 86400).to_i

  private def __case(s, flags)
    return s.upcase if flags.include?("^")
    s
  end

  private def __zone_offset(flags)
    sign = @of < 0 ? "-" : "+"
    a = @of.abs
    if flags.include?(":")
      format("%s%02d:%02d", sign, a / 3600, (a % 3600) / 60)
    else
      format("%s%02d%02d", sign, a / 3600, (a % 3600) / 60)
    end
  end

  # ---- parsing -----------------------------------------------------------

  def self._parse(str, comp = true)
    str = str.to_str if !str.is_a?(String) && str.respond_to?(:to_str)
    raise TypeError, "no implicit conversion into String" unless str.is_a?(String)
    h = {}
    s = str.dup

    if (m = /([-+]?\d{4,})-(\d{1,2})-(\d{1,2})/.match(s))
      h[:year], h[:mon], h[:mday] = m[1].to_i, m[2].to_i, m[3].to_i
      s = m.pre_match + m.post_match
    elsif (m = /(\d{4})(\d{2})(\d{2})/.match(s))
      h[:year], h[:mon], h[:mday] = m[1].to_i, m[2].to_i, m[3].to_i
      s = m.pre_match + m.post_match
    elsif (m = /(\d{1,2})\/(\d{1,2})\/(\d{2,4})/.match(s))
      h[:mon], h[:mday], h[:year] = m[1].to_i, m[2].to_i, m[3].to_i
      s = m.pre_match + m.post_match
    elsif (m = /(-?\d{4})[.\s]+([A-Za-z]{3,})\.?[.\s]+(\d{1,2})(?:st|nd|rd|th)?/.match(s)) && __month_index(m[2])
      h[:year], h[:mon], h[:mday] = m[1].to_i, __month_index(m[2]), m[3].to_i   # "YYYY mmm DD"
      s = m.pre_match + m.post_match
    elsif (m = /(\d{1,2})(?:st|nd|rd|th)?[.\s]+([A-Za-z]{3,})\.?(?:[.\s,]*(-?\d{1,4}))?/.match(s)) && __month_index(m[2])
      h[:mday], h[:mon] = m[1].to_i, __month_index(m[2])                        # "DD mmm[ YYYY]"
      h[:year] = m[3].to_i if m[3]
      s = m.pre_match + m.post_match
    elsif (m = /([A-Za-z]{3,})\.?[.\s]*(-?\d{4})(?![\d])/.match(s)) && __month_index(m[1])
      h[:mon], h[:year] = __month_index(m[1]), m[2].to_i                        # "mmm[.]YYYY" (4 digits = year)
      s = m.pre_match + m.post_match
    elsif (m = /([A-Za-z]{3,})\.?[.\s]*(\d{1,2})(?!\d)(?:st|nd|rd|th)?(?:\s*,?\s*(-?\d{1,4}))?/.match(s)) && __month_index(m[1])
      h[:mon], h[:mday] = __month_index(m[1]), m[2].to_i                        # "mmm[.]DD[, YYYY]"
      h[:year] = m[3].to_i if m[3]
      s = m.pre_match + m.post_match
    elsif (m = /(-?\d{4})[.](\d{1,2})[.](\d{1,2})/.match(s))
      h[:year], h[:mon], h[:mday] = m[1].to_i, m[2].to_i, m[3].to_i             # "YYYY.MM.DD" (4-digit head = year)
      s = m.pre_match + m.post_match
    elsif (m = /(\d{1,2})[.](\d{1,2})[.](-?\d{2,4})/.match(s))
      h[:mday], h[:mon], h[:year] = m[1].to_i, m[2].to_i, m[3].to_i             # "DD.MM.YYYY"
      s = m.pre_match + m.post_match
    elsif (m = /\b([A-Za-z]{3,})\.?\b/.match(s)) && __month_index(m[1]) && !__wday_index(m[1])
      h[:mon] = __month_index(m[1])                                            # bare month name
      s = m.pre_match + m.post_match
    elsif (m = /\A\s*(\d+)\s*\z/.match(s))
      # Bare digit strings, disambiguated by length (CRuby's ddd rules):
      # DD → day / DDD → year-day / MMDD → month+day / YYMMDD / YYYYMMDD is
      # handled above / YYDDD → 2-digit year + year-day / YYYYDDD.
      d = m[1]
      case d.length
      when 1, 2 then h[:mday] = d.to_i
      when 3    then h[:yday] = d.to_i
      when 4    then h[:mon], h[:mday] = d[0, 2].to_i, d[2, 2].to_i
      when 5    then h[:year], h[:yday] = __complete_year(d[0, 2].to_i), d[2, 3].to_i
      when 6    then h[:year], h[:mon], h[:mday] = __complete_year(d[0, 2].to_i), d[2, 2].to_i, d[4, 2].to_i
      when 7    then h[:year], h[:yday] = d[0, 4].to_i, d[4, 3].to_i
      end
      s = m.pre_match + m.post_match
    end

    if h[:mday].nil? && h[:yday].nil? && (m = /\b(sun|mon|tue|wed|thu|fri|sat)[a-z]*\b/i.match(s))
      h[:wday] = %w[sun mon tue wed thu fri sat].index(m[1].downcase)
      s = m.pre_match + m.post_match
    end

    if (m = /(\d{1,2}):(\d{1,2})(?::(\d{1,2})(?:[.,](\d+))?)?/.match(s))
      h[:hour], h[:min] = m[1].to_i, m[2].to_i
      h[:sec] = m[3].to_i if m[3]
      h[:sec_fraction] = Rational(m[4].to_i, 10**m[4].length) if m[4]
      s = m.pre_match + m.post_match
    end

    if (m = /(?<![\d.])([-+]\d{2}):?(\d{2})?\b|\b(UTC|GMT|Z)\b/.match(s))
      if m[3]
        h[:zone] = m[3]
        h[:offset] = 0
      else
        h[:zone] = m[0]
        sign = m[0][0] == "-" ? -1 : 1
        h[:offset] = sign * (m[1].to_i.abs * 3600 + (m[2] ? m[2].to_i : 0) * 60)
      end
    end

    if (m = /\b([A-Za-z]{3,})\b/.match(s)) && (w = __wday_index(m[1]))
      h[:wday] = w
    end

    if comp && h[:year] && h[:year] >= 0 && h[:year] < 100
      h[:year] += h[:year] >= 69 ? 1900 : 2000       # 2-digit year → 19xx/20xx
    end
    h
  end

  def self.__month_index(name)
    n = name[0, 3].downcase
    ABBR_MONTHNAMES.index { |x| x && x.downcase == n }
  end

  def self.__wday_index(name)
    n = name[0, 3].downcase
    ABBR_DAYNAMES.index { |x| x.downcase == n }
  end

  # CRuby's 69-rule: 2-digit years 69..99 are 19xx, 00..68 are 20xx.
  def self.__complete_year(yy)
    yy >= 69 ? 1900 + yy : 2000 + yy
  end

  def self.parse(str = "-4712-01-01", comp = true, sg = ITALY)
    h = _parse(str, comp)
    raise Date::Error, "invalid date" unless h[:year] || h[:mon] || h[:mday] || h[:yday] || h[:wday]
    # Partial dates complete from today (Date.parse("10") → this month's 10th).
    if h[:year].nil? || h[:mon].nil? || h[:mday].nil?
      t = Time.now
      if h[:yday]
        return ordinal(h[:year] || t.year, h[:yday], sg)
      elsif h[:wday] && h[:mday].nil?
        base = civil(t.year, t.mon, t.day, sg)     # the named day of THIS week (Sun-start)
        return base - base.wday + h[:wday]
      end
      h[:year] ||= t.year
      if h[:mon].nil? && h[:mday]
        h[:mon] = t.mon
      end
    end
    civil(h[:year], h[:mon] || 1, h[:mday] || 1, sg)
  end

  def self.iso8601(str = "-4712-01-01", sg = ITALY) = parse(str, true, sg)
  def self.rfc3339(str = "-4712-01-01T00:00:00+00:00", sg = ITALY) = parse(str, true, sg)
  def self.xmlschema(str = "-4712-01-01", sg = ITALY) = parse(str, true, sg)
  def self.rfc2822(str = "Mon, 1 Jan -4712 00:00:00 +0000", sg = ITALY) = parse(str, true, sg)
  class << self; alias_method :rfc822, :rfc2822; end
  def self.httpdate(str = "Mon, 01 Jan -4712 00:00:00 GMT", sg = ITALY) = parse(str, true, sg)

  def self._strptime(str, fmt = "%F")
    h = {}
    si = 0
    fi = 0
    while fi < fmt.length
      c = fmt[fi]
      if c != "%"
        if c == " "
          si += 1 while si < str.length && str[si] == " "
        else
          return nil unless str[si] == c
          si += 1
        end
        fi += 1
        next
      end
      fi += 1
      spec = fmt[fi]
      fi += 1
      case spec
      when "Y" then si = __sp_int(str, si, h, :year, 10, true) or return nil
      when "y" then si = __sp_int(str, si, h, :year, 2) or return nil
      when "m" then si = __sp_int(str, si, h, :mon, 2) or return nil
      when "d" then si = __sp_int(str, si, h, :mday, 2) or return nil
      when "e"                                            # day with ONE optional leading blank
        si += 1 if str[si] == " "
        si = __sp_int(str, si, h, :mday, 2) or return nil
      when "H" then si = __sp_int(str, si, h, :hour, 2) or return nil
      when "M" then si = __sp_int(str, si, h, :min, 2) or return nil
      when "S" then si = __sp_int(str, si, h, :sec, 2) or return nil
      when "j" then si = __sp_int(str, si, h, :yday, 3) or return nil
      when "B", "b", "h"
        m = /\A([A-Za-z]+)/.match(str[si..-1]) or return nil
        idx = __month_index(m[1]) or return nil
        h[:mon] = idx
        si += m[1].length
      when "A", "a"
        m = /\A([A-Za-z]+)/.match(str[si..-1]) or return nil
        w = __wday_index(m[1]) or return nil
        h[:wday] = w
        si += m[1].length
      when "F"
        sub = _strptime(str[si..-1], "%Y-%m-%d") or return nil
        h.update(sub)
        si += (str.length - si) - sub[:leftover].to_s.length
      when "T"
        sub = _strptime(str[si..-1], "%H:%M:%S") or return nil
        h.update(sub)
        si += (str.length - si) - sub[:leftover].to_s.length
      when "C" then si = __sp_int(str, si, h, :__century, 2) or return nil        # century (with %y)
      when "G" then si = __sp_int(str, si, h, :cwyear, 10, true) or return nil     # commercial year
      when "g" then si = __sp_int(str, si, h, :__cwyear2, 2) or return nil
      when "V" then si = __sp_int(str, si, h, :cweek, 2) or return nil             # ISO week
      when "U" then si = __sp_int(str, si, h, :wnum0, 2) or return nil             # week (Sunday start)
      when "W" then si = __sp_int(str, si, h, :wnum1, 2) or return nil             # week (Monday start)
      when "u" then si = __sp_int(str, si, h, :cwday, 1) or return nil             # 1..7 (Mon..Sun)
      when "w" then si = __sp_int(str, si, h, :wday, 1) or return nil              # 0..6 (Sun..Sat)
      when "L" then si = __sp_int(str, si, h, :__msec, 3) or return nil
      when "N" then si = __sp_int(str, si, h, :__nsec, 9) or return nil
      when "%" then (return nil unless str[si] == "%"); si += 1
      else return nil
      end
    end
    h.delete(:leftover)
    h[:leftover] = str[si..-1] if si < str.length
    if (cen = h.delete(:__century))
      h[:year] = cen * 100 + (h[:year] || 0)              # %C[%y]
    elsif h[:year] && h[:year] < 100 && fmt.include?("%y")
      h[:year] += h[:year] >= 69 ? 1900 : 2000
    end
    if (cy2 = h.delete(:__cwyear2))                       # %g: 2-digit commercial year
      h[:cwyear] = cy2 + (cy2 >= 69 ? 1900 : 2000)
    end
    if (ms = h.delete(:__msec)) then h[:sec_fraction] = Rational(ms, 1000) end
    if (ns = h.delete(:__nsec)) then h[:sec_fraction] = Rational(ns, 1_000_000_000) end
    h
  end

  def self.__sp_int(str, si, h, key, maxlen, signed = false)
    j = si
    j += 1 if signed && j < str.length && (str[j] == "-" || str[j] == "+")
    k = j
    k += 1 while k < str.length && k - j < maxlen && str[k] >= "0" && str[k] <= "9"
    return nil if k == j
    h[key] = str[si...k].to_i
    k
  end

  def self.strptime(str = "-4712-01-01", fmt = "%F", sg = ITALY)
    h = _strptime(str, fmt) or raise Date::Error, "invalid date"
    __from_parsed(h, sg)
  end

  # Build a Date from a parsed-field Hash, completing what the format did not
  # give from today (CRuby: a lone %m is this year's month, %V/%U/%W pick the
  # week of the current year, %u/%w the weekday of the current week).
  def self.__from_parsed(h, sg = ITALY)
    t = nil
    today = -> { t ||= Date.today }
    if h[:yday] && !h[:mon]
      return ordinal(h[:year] || today.().year, h[:yday], sg)
    end
    if h[:cwday] && !h[:cwyear] && !h[:cweek]        # a lone %u → this week's day (Mon-start)
      base = today.()
      monday = base - ((base.wday + 6) % 7)
      return monday + (h[:cwday] - 1)
    end
    if h[:cwyear] || h[:cweek] || h[:cwday]
      cwy = h[:cwyear] || today.().year
      cwk = h[:cweek] || 1
      cwd = h[:cwday] || 1
      return commercial(cwy, cwk, cwd, sg)
    end
    if (wn = h[:wnum0] || h[:wnum1])                 # week of year (Sun/Mon start)
      y = h[:year] || today.().year
      jan1 = civil(y, 1, 1, sg)
      start = h[:wnum0] ? 0 : 1                      # weekday the week starts on
      first = jan1 - ((jan1.wday - start) % 7)       # first such weekday on/before Jan 1
      base = first + wn * 7
      base += (h[:wday] - start) % 7 if h[:wday]
      return base
    end
    if h[:wday] && h[:mday].nil? && h[:mon].nil?     # a lone weekday → this week's
      base = today.()
      return base - base.wday + h[:wday]
    end
    if h[:year].nil? || h[:mon].nil? || h[:mday].nil?
      y = h[:year] || today.().year
      m = h[:mon] || (h[:mday] ? today.().mon : 1)
      return civil(y, m, h[:mday] || 1, sg)
    end
    civil(h[:year], h[:mon], h[:mday], sg)
  end
end

# DateTime adds a time of day and a UTC offset on top of Date.
class DateTime < Date
  def self.civil(y = -4712, m = 1, d = 1, h = nil, min = nil, s = 0, of = 0, sg = ITALY)
    if h.nil? && d.is_a?(Numeric) && !d.is_a?(Integer)
      # `DateTime.new(y, m, d)` — with no hour given the day may carry a
      # fraction, which becomes the time of day (the C short form).
      h = (d - d.floor) * 24
      d = d.floor
    end
    jd = _valid_civil_jd(y, m, d, sg) or raise Date::Error, "invalid date"
    carry, df, sf = __time_parts(h || 0, min, s)
    new!(jd + carry, df, sf, __offset_seconds(of), sg)
  end
  class << self; alias_method :new, :civil; end

  def self.jd(jd = 0, h = 0, min = nil, s = 0, of = 0, sg = ITALY)
    carry, df, sf = __time_parts(h, min, s)
    new!(jd + carry, df, sf, __offset_seconds(of), sg)
  end

  def self.ordinal(y = -4712, d = 1, h = 0, min = nil, s = 0, of = 0, sg = ITALY)
    jd = _valid_ordinal_jd(y, d, sg) or raise Date::Error, "invalid date"
    carry, df, sf = __time_parts(h, min, s)
    new!(jd + carry, df, sf, __offset_seconds(of), sg)
  end

  def self.commercial(y = -4712, w = 1, d = 1, h = 0, min = nil, s = 0, of = 0, sg = ITALY)
    jd = _valid_commercial_jd(y, w, d, sg) or raise Date::Error, "invalid date"
    carry, df, sf = __time_parts(h, min, s)
    new!(jd + carry, df, sf, __offset_seconds(of), sg)
  end

  def self.now(sg = ITALY)
    t = Time.now
    new!(civil_to_jd(t.year, t.month, t.day, sg),
         t.hour * 3600 + t.min * 60 + t.sec, 0, t.utc_offset, sg)
  end

  # → [day carry, seconds in day, nanoseconds].  Hour and minute must be whole
  # (Ruby rejects a fractional hour outright); a fractional second becomes @sf.
  # Negative fields count back from the end of the unit, and 24:00:00 is the
  # start of the next day.
  def self.__time_parts(h, min, s)
    if min.nil?
      # `DateTime.new(y, m, d, h)` — with no minute given the hour may carry a
      # fraction, matching the C constructor's short form.
      min = 0
      if h.is_a?(Numeric) && !h.is_a?(Integer)
        whole = h.floor
        rest = (h - whole) * 3600
        min = (rest / 60).floor
        s = rest - min * 60
        h = whole
      end
    end
    raise Date::Error, "invalid date" unless h.is_a?(Integer) && min.is_a?(Integer)
    raise Date::Error, "invalid date" unless s.is_a?(Numeric)
    h = 24 + h if h < 0
    min = 60 + min if min < 0
    s = 60 + s if s < 0
    raise Date::Error, "invalid date" if h < 0 || h > 24 || min < 0 || min > 59 || s < 0 || s >= 60
    raise Date::Error, "invalid date" if h == 24 && (min > 0 || s > 0)
    return [1, 0, 0] if h == 24
    sec = s.floor
    frac = s - sec
    [0, h * 3600 + min * 60 + sec, (frac * 1_000_000_000).round]
  end

  # Accepts a seconds count, a day fraction, or a "+09:00" string.
  def self.__offset_seconds(of)
    case of
    when String
      m = /\A([-+])(\d{2}):?(\d{2})?\z/.match(of) or
        (return 0 if of == "Z" || of == "UTC" || of == "GMT")
      raise Date::Error, "invalid offset" unless m
      sign = m[1] == "-" ? -1 : 1
      sign * (m[2].to_i * 3600 + (m[3] ? m[3].to_i : 0) * 60)
    when Integer then of.zero? ? 0 : (of.abs < 24 ? of * 3600 : of)
    when Numeric then (of * 86400).round
    else 0
    end
  end

  def hour = @df / 3600
  def min = (@df % 3600) / 60
  alias_method :minute, :min
  def sec = @df % 60
  alias_method :second, :sec
  def sec_fraction = Rational(@sf, 1_000_000_000)
  alias_method :second_fraction, :sec_fraction

  def to_s = format("%04d-%02d-%02dT%02d:%02d:%02d%s", year, mon, mday, hour, min, sec, zone)
  def iso8601(n = 0) = to_s
  alias_method :xmlschema, :iso8601
  def rfc3339(n = 0) = to_s
  def to_date = Date.new!(@jd, 0, 0, 0, @sg)
  def to_datetime = self

  def to_time
    Time.at(__epoch_seconds)
  end

  def deconstruct_keys(keys)
    all = { year: year, month: month, day: day, yday: yday, wday: wday,
            hour: hour, min: min, sec: sec, sec_fraction: sec_fraction, zone: zone }
    return all if keys.nil?
    h = {}
    keys.each { |k| h[k] = all[k] if all.key?(k) }
    h
  end

  def self.parse(str = "-4712-01-01T00:00:00+00:00", comp = true, sg = ITALY)
    h = _parse(str, comp)
    raise Date::Error, "invalid date" unless h[:year] || h[:mon] || h[:mday]
    civil(h[:year] || -4712, h[:mon] || 1, h[:mday] || 1,
          h[:hour] || 0, h[:min] || 0, h[:sec] || 0, h[:offset] || 0, sg)
  end

  def self.strptime(str = "-4712-01-01T00:00:00+00:00", fmt = "%FT%T%z", sg = ITALY)
    h = _strptime(str, fmt) or raise Date::Error, "invalid date"
    civil(h[:year] || -4712, h[:mon] || 1, h[:mday] || 1,
          h[:hour] || 0, h[:min] || 0, h[:sec] || 0, h[:offset] || 0, sg)
  end
end

class Time
  def to_date = Date.civil(year, month, day)
  def to_datetime = DateTime.civil(year, month, day, hour, min, sec, utc_offset)
  def to_time = self
end
