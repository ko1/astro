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
               .map { |s| s&.dup&.force_encoding(Encoding::US_ASCII)&.freeze }.freeze
  ABBR_MONTHNAMES = [nil, "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
                    .map { |s| s&.dup&.force_encoding(Encoding::US_ASCII)&.freeze }.freeze
  DAYNAMES = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"]
             .map { |s| s.dup.force_encoding(Encoding::US_ASCII).freeze }.freeze
  ABBR_DAYNAMES = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"].map { |s| s.dup.force_encoding(Encoding::US_ASCII).freeze }.freeze

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
    if d < 0
      # Count back over the days that really exist: the calendar-reform gap is
      # not a run of dates, so the arithmetic has to happen in JD (CRuby).
      ljd = __last_dom_jd(y, m, sg) or return nil
      jd = ljd + d + 1
      ry, rm, = jd_to_civil(jd, sg)
      return (ry == y && rm == m) ? jd : nil
    end
    return nil if d > _month_days(y, m, sg)
    jd = civil_to_jd(y, m, d, sg)
    jd_to_civil(jd, sg) == [y, m, d] ? jd : nil
  end

  # JD of the last existing day of (y, m), or nil if the month has none.
  def self.__last_dom_jd(y, m, sg)
    last = _month_days(y, m, sg)
    last.downto(28) do |i|
      jd = civil_to_jd(y, m, i, sg)
      return jd if jd_to_civil(jd, sg) == [y, m, i]
    end
    nil
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
  # time-of-day and zone are DateTime's; a Date keeps them for its own use
  # (i18n tells a Time-like value by respond_to?(:sec))
  private :hour, :min, :sec, :sec_fraction, :offset, :zone

  # a frozen Date cannot memoize (faker freezes what it returns)
  private def __civil = frozen? ? Date.jd_to_civil(@jd, @sg) : (@civil ||= Date.jd_to_civil(@jd, @sg))

  def self.__zone_str(of)
    sign = of < 0 ? "-" : "+"
    a = of.abs
    format("%s%02d:%02d", sign, a / 3600, (a % 3600) / 60).force_encoding(Encoding::US_ASCII)
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

  NS_PER_DAY__ = 86400 * 1_000_000_000
  private_constant :NS_PER_DAY__

  # Keep @jd out of the sum: folding a small fraction into a ~2.4e6 Float jd
  # loses ~10 µs of the time of day.  A Float operand is taken at the
  # nanosecond resolution the receiver stores anyway.
  private def __plus_fraction(fr)
    fr = Rational((fr * NS_PER_DAY__).round, NS_PER_DAY__) if fr.is_a?(Float)
    total = day_fraction + fr
    carry = total.floor
    rest = total - carry
    ns = (rest * 86400 * 1_000_000_000).round
    df, sf = ns.divmod(1_000_000_000)
    self.class.new!(@jd + carry, df, sf, @of, @sg)
  end

  # Month arithmetic clamps to the end of the target month, as Ruby does.
  def >>(n)
    y, m, d = __civil
    m += n
    y += (m - 1).div(12)
    m = (m - 1) % 12 + 1
    last = Date._month_days(y, m, @sg)
    d = last if d > last
    # The target day can also land in the calendar-reform gap; step back until
    # it names a day that exists (CRuby).
    jd = nil
    while d >= 1 && (jd = Date._valid_civil_jd(y, m, d, @sg)).nil?
      d -= 1
    end
    raise Date::Error, "invalid date" if jd.nil?
    self.class.new!(jd, @df, @sf, @of, @sg)
  end

  # `0 - n` rather than `-n`: a non-numeric argument must raise TypeError, as
  # CRuby's `year*12 + mon-1 + n` does.
  def <<(n) = self >> (0 - n)

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
    raise TypeError, "wrong argument type #{keys.class} (expected Array or nil)" unless keys.is_a?(Array)
    h = {}
    keys.each { |k| h[k] = all[k] if all.key?(k) }
    h
  end

  # ---- conversion --------------------------------------------------------

  def to_date = self

  # d_lite_to_datetime: the day fraction is dropped
  def to_datetime
    DateTime.new!(@jd, 0, 0, @of, @sg)
  end

  # d_lite_to_time: a Julian date converts through the (proleptic) Gregorian one
  def to_time
    d = julian? ? gregorian : self
    Time.local(d.year, d.mon, d.mday)
  end

  def to_s = __strftimev("%Y-%m-%d")

  # mk_inspect shows the UTC jd / day seconds
  def inspect
    jd = @jd
    df = @df - @of
    if df < 0
      jd -= 1
      df += 86400
    elsif df >= 86400
      jd += 1
      df -= 86400
    end
    sf = @sf.is_a?(Rational) && @sf.denominator == 1 ? @sf.to_i : @sf
    format("#<%s: %s ((%sj,%ds,%sn),%+ds,%sj)>",
           self.class.name, to_s, jd.to_s, df, sf.to_s, @of, __sg_str).force_encoding(Encoding::US_ASCII)
  end

  private def __sg_str
    # CRuby's Date#inspect abbreviates the calendar-reform point
    return "-Inf" if @sg == GREGORIAN
    return "Inf" if @sg == JULIAN
    @sg.to_s
  end

  # ---- formatting (date_core.c d_lite_* / dt_lite_*) ----------------------

  def iso8601 = __strftimev("%Y-%m-%d")
  alias_method :xmlschema, :iso8601
  def rfc3339 = __strftimev("%Y-%m-%dT%H:%M:%S%:z")
  def rfc2822 = __strftimev("%a, %-d %b %Y %T %z")
  alias_method :rfc822, :rfc2822
  def httpdate = (@of == 0 ? self : new_offset(0)).__send__(:__strftimev, "%a, %d %b %Y %T GMT")
  def jisx0301 = __strftimev(__jisx0301_date_format)
  def asctime = __strftimev("%a %b %e %H:%M:%S %Y")
  alias_method :ctime, :asctime

  # jisx0301_date_format
  private def __jisx0301_date_format
    d = @jd
    return "%Y-%m-%d" if d < 2405160
    c, s = if d < 2419614 then ["M", 1867]
           elsif d < 2424875 then ["T", 1911]
           elsif d < 2447535 then ["S", 1925]
           elsif d < 2458605 then ["H", 1988]
           else ["R", 2018]
           end
    format("%s%02d.%%m.%%d", c, year - s)
  end

  # ---- strftime (a port of ext/date/date_strftime.c) ----------------------

  def strftime(fmt = NO_FMT__)
    if fmt.equal?(NO_FMT__)
      fmt = __default_strftime_format.dup.force_encoding(Encoding::US_ASCII)
    else
      fmt = Date.__string_value(fmt)
      raise ArgumentError, "format should have ASCII compatible encoding" unless fmt.encoding.ascii_compatible?
    end
    __strftime_alloc(fmt).force_encoding(fmt.encoding)
  end

  private def __default_strftime_format = "%F"

  private def __strftimev(fmt) = __strftime_alloc(fmt).force_encoding(Encoding::US_ASCII)

  # date_strftime_internal / date_strftime_alloc: "\0" splits the format, and
  # the output may grow to 1024 * 2**k >= 1024 * flen bytes before ERANGE.
  private def __strftime_alloc(fmt)
    return +"" if fmt.empty?
    if fmt.include?("\0")
      out = +""
      fmt.split(/(\0+)/).each do |part|
        next if part.empty?
        out << (part[0] == "\0" ? part : __strftime_alloc(part))
      end
      return out
    end
    limit = 1024
    limit *= 2 while limit < 1024 * fmt.bytesize
    out = __strftime_tmx(fmt, limit)
    raise Errno::ERANGE, fmt if out.nil? || out.bytesize >= limit
    out
  end

  STRFTIME_DAYS__ = %w[Sunday Monday Tuesday Wednesday Thursday Friday Saturday].freeze
  STRFTIME_MONTHS__ = %w[January February March April May June July August
                         September October November December].freeze
  private_constant :STRFTIME_DAYS__, :STRFTIME_MONTHS__

  SF_LEFT__ = 1; SF_CHCASE__ = 2; SF_LOWER__ = 4; SF_UPPER__ = 8
  SF_LOCALE_O__ = 16; SF_LOCALE_E__ = 32; SF_COLONS__ = 64
  FIXNUM_MAX__ = 2**62 - 1
  private_constant :SF_LEFT__, :SF_CHCASE__, :SF_LOWER__, :SF_UPPER__,
                   :SF_LOCALE_O__, :SF_LOCALE_E__, :SF_COLONS__, :FIXNUM_MAX__

  # whole UTC seconds since the epoch (the old ajd-based .to_i rounded toward 0)
  private def __epoch_seconds = __tmx_secs

  # tmx accessors (local civil fields; secs are UTC seconds since the epoch)
  private def __tmx_secs = (@jd - 2440588) * 86400 + @df - @of
  private def __tmx_msecs
    ms = __tmx_secs * 1000
    ms += @sf / 1_000_000 unless @sf == 0
    ms
  end
  private def __tmx_wnum(f) = Date.__jd_to_weeknum(@jd, f, @sg)[1]

  # date_strftime_with_tmx; nil when the result cannot fit in `limit`.
  private def __strftime_tmx(format, limit)
    out = +""
    fi = 0
    flen = format.length
    while fi < flen
      ch = format[fi]
      if ch != "%"
        out << ch
        fi += 1
        next
      end
      sp = fi
      precision = -1
      flags = 0
      padding = nil
      colons = 0
      tp = nil
      emit = false # "break" to the common copy-out code
      while true # again
        fi += 1
        c = format[fi]
        unknown = false
        if !c.nil? && "_-^#0".include?(c) &&
           (precision > 0 || flags & (SF_LOCALE_E__ | SF_LOCALE_O__ | SF_COLONS__) != 0)
          unknown = true # FLAG_FOUND
        else
          case c
          when nil
            fi -= 1
            unknown = true
          when "A", "a"
            if flags & SF_CHCASE__ != 0
              flags &= ~(SF_LOWER__ | SF_CHCASE__)
              flags |= SF_UPPER__
            end
            wd = wday
            tp = c == "A" ? STRFTIME_DAYS__[wd] : STRFTIME_DAYS__[wd][0, 3]
            emit = true
          when "B", "b", "h"
            if flags & SF_CHCASE__ != 0
              flags &= ~(SF_LOWER__ | SF_CHCASE__)
              flags |= SF_UPPER__
            end
            mo = mon
            tp = c == "B" ? STRFTIME_MONTHS__[mo - 1] : STRFTIME_MONTHS__[mo - 1][0, 3]
            emit = true
          when "C"
            __sf_fmt(out, flags, padding, precision, "0", 2, year.div(100))
          when "c"
            __sf_sub(out, "%a %b %e %H:%M:%S %Y", flags, padding, precision, limit) or return nil
          when "D", "x"
            __sf_sub(out, "%m/%d/%y", flags, padding, precision, limit) or return nil
          when "d", "e"
            __sf_fmt(out, flags, padding, precision, c == "d" ? "0" : " ", 2, mday)
          when "F"
            __sf_sub(out, "%Y-%m-%d", flags, padding, precision, limit) or return nil
          when "G", "Y"
            y = c == "G" ? cwyear : year
            if y >= -FIXNUM_MAX__ - 1 && y <= FIXNUM_MAX__
              __sf_fmt(out, flags, padding, precision, "0", y >= 0 ? 4 : 5, y)
            else
              __sf_fmt(out, flags, padding, precision, "0", 4, y)
            end
          when "g", "y"
            __sf_fmt(out, flags, padding, precision, "0", 2, (c == "g" ? cwyear : year) % 100)
          when "H", "k"
            __sf_fmt(out, flags, padding, precision, c == "H" ? "0" : " ", 2, hour)
          when "I", "l"
            v = hour
            if v == 0
              v = 12
            elsif v > 12
              v -= 12
            end
            __sf_fmt(out, flags, padding, precision, c == "I" ? "0" : " ", 2, v)
          when "j"
            __sf_fmt(out, flags, padding, precision, "0", 3, yday)
          when "L", "N"
            w = c == "L" ? 3 : 9
            precision = w if precision <= 0
            return nil if precision >= limit
            subsec = sec_fraction
            ww = precision
            while ww >= 9
              subsec *= 1_000_000_000
              ww -= 9
            end
            subsec *= 10**ww if ww > 0
            out << format("%0*d", precision, subsec.div(1))
          when "M"
            __sf_fmt(out, flags, padding, precision, "0", 2, min)
          when "m"
            __sf_fmt(out, flags, padding, precision, "0", 2, mon)
          when "n", "t"
            __sf_fill(out, flags, padding, precision, 1)
            out << (c == "n" ? "\n" : "\t")
          when "P", "p"
            if (c == "p" && flags & SF_CHCASE__ != 0) ||
               (c == "P" && flags & (SF_CHCASE__ | SF_UPPER__) == 0)
              flags &= ~(SF_UPPER__ | SF_CHCASE__)
              flags |= SF_LOWER__
            end
            tp = hour < 12 ? "AM" : "PM"
            emit = true
          when "Q"
            __sf_fmt(out, flags, padding, precision, "0", 1, __tmx_msecs.floor)
          when "R"
            __sf_sub(out, "%H:%M", flags, padding, precision, limit) or return nil
          when "r"
            __sf_sub(out, "%I:%M:%S %p", flags, padding, precision, limit) or return nil
          when "S"
            __sf_fmt(out, flags, padding, precision, "0", 2, sec)
          when "s"
            __sf_fmt(out, flags, padding, precision, "0", 1, __tmx_secs)
          when "T", "X"
            __sf_sub(out, "%H:%M:%S", flags, padding, precision, limit) or return nil
          when "U", "W"
            __sf_fmt(out, flags, padding, precision, "0", 2, __tmx_wnum(c == "U" ? 0 : 1))
          when "u"
            __sf_fmt(out, flags, padding, precision, "0", 1, cwday)
          when "V"
            __sf_fmt(out, flags, padding, precision, "0", 2, cweek)
          when "v"
            __sf_sub(out, "%e-%^b-%Y", flags, padding, precision, limit) or return nil
          when "w"
            __sf_fmt(out, flags, padding, precision, "0", 1, wday)
          when "Z"
            if flags & SF_CHCASE__ != 0
              flags &= ~(SF_UPPER__ | SF_CHCASE__)
              flags |= SF_LOWER__
            end
            tp = zone
            emit = true
          when "z"
            if colons > 3
              fi -= 1
              unknown = true
            else
              __sf_z(out, flags, padding, precision, colons)
            end
          when "+"
            __sf_sub(out, "%a %b %e %H:%M:%S %Z %Y", flags, padding, precision, limit) or return nil
          when "E"
            flags |= SF_LOCALE_E__
            nc = format[fi + 1]
            next if !nc.nil? && "cCxXyY".include?(nc)
            unknown = true
          when "O"
            flags |= SF_LOCALE_O__
            nc = format[fi + 1]
            next if !nc.nil? && "deHkIlmMSuUVwWy".include?(nc)
            unknown = true
          when ":"
            flags |= SF_COLONS__
            l = 0
            l += 1 while format[fi + l] == ":"
            if format[fi + l] == "z"
              colons = l
              fi += l - 1
              next
            end
            unknown = true
          when "_"
            padding = " "
            next
          when "-"
            flags |= SF_LEFT__
            next
          when "^"
            flags |= SF_UPPER__
            next
          when "#"
            flags |= SF_CHCASE__
            next
          when "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"
            padding = "0" if c == "0"
            e = fi
            e += 1 while (d = format[e]) && d >= "0" && d <= "9"
            prec = format[fi...e].to_i
            return nil if prec > 2147483647 || prec > limit
            precision = prec
            fi = e - 1
            next
          when "%"
            __sf_fill(out, flags, padding, precision, 1)
            out << "%"
          else
            unknown = true
          end
        end
        if unknown
          tp = format[sp..fi]
          precision = -1
          flags = 0
          padding = nil
          colons = 0
          emit = true
        end
        break
      end
      if emit && !tp.empty?
        __sf_fill(out, flags, padding, precision, tp.length)
        case flags & (SF_UPPER__ | SF_LOWER__)
        when SF_UPPER__ then out << tp.upcase
        when SF_LOWER__ then out << tp.downcase
        else out << tp
        end
      end
      return nil if out.bytesize >= limit
      fi += 1
    end
    out
  end

  # FILL_PADDING
  private def __sf_fill(out, flags, padding, precision, i)
    if flags & SF_LEFT__ == 0 && precision > i
      out << (padding || " ") * (precision - i)
    end
  end

  # FMT / FMTV
  private def __sf_fmt(out, flags, padding, precision, def_pad, def_prec, val)
    precision = def_prec if precision <= 0
    precision = 1 if flags & SF_LEFT__ != 0
    zero = padding == "0" || (padding.nil? && def_pad == "0")
    out << format(zero ? "%0*d" : "%*d", precision, val)
  end

  # STRFTIME(fmt): a recursive expansion, padded as one unit.
  private def __sf_sub(out, fmt, flags, padding, precision, limit)
    s = __strftime_tmx(fmt, limit - out.bytesize) or return nil
    return nil if s.empty?
    s = s.upcase if flags & SF_UPPER__ != 0
    if flags & SF_LEFT__ == 0 && precision > s.length
      return nil if out.bytesize + precision > limit
      out << (padding || " ") * (precision - s.length)
    end
    out << s
  end

  # %z, %:z, %::z, %:::z
  private def __sf_z(out, flags, padding, precision, colons)
    off = @of
    aoff = off.abs
    hl = aoff / 3600 < 10 ? 1 : 2
    hw = 2
    hw = 1 if flags & SF_LEFT__ != 0 && hl == 1
    case colons
    when 0 then precision = precision <= 3 + hw ? hw : precision - 3
    when 1 then precision = precision <= 4 + hw ? hw : precision - 4
    when 2 then precision = precision <= 7 + hw ? hw : precision - 7
    when 3
      precision = if aoff % 3600 == 0 then precision <= 1 + hw ? hw : precision - 1
                  elsif aoff % 60 == 0 then precision <= 4 + hw ? hw : precision - 4
                  else precision <= 7 + hw ? hw : precision - 7
                  end
    end
    if padding == " " && precision > hl
      out << " " * (precision - hl)
      precision = hl
    end
    if off < 0
      off = -off
      out << "-"
    else
      out << "+"
    end
    out << (off / 3600).to_s.rjust(precision, "0")
    off %= 3600
    return if colons == 3 && off == 0
    out << ":" if colons >= 1
    out << format("%02d", off / 60)
    off %= 60
    return if colons == 3 && off == 0
    out << ":" << format("%02d", off) if colons >= 2
  end

  # ---- parsing -----------------------------------------------------------
  #
  # A 1:1 port of the date gem's ext/date/date_parse.c (Date._parse and the
  # _iso8601 / _rfc3339 / _xmlschema / _rfc2822 / _httpdate / _jisx0301
  # family), date_strptime.c (Date._strptime) and the frag completion of
  # date_core.c (d_new_by_frags / dt_new_by_frags).  The regexps are the C
  # sources verbatim (TIGHT_PARSER is off in CRuby).

  module Parse__
    ABBR_DAYS   = "sun|mon|tue|wed|thu|fri|sat"
    ABBR_MONTHS = "jan|feb|mar|apr|may|jun|jul|aug|sep|oct|nov|dec"
    DAYS = "sunday|monday|tuesday|wednesday|thursday|friday|saturday"
    ABBR_DAYS_A   = %w[sun mon tue wed thu fri sat].freeze
    ABBR_MONTHS_A = %w[jan feb mar apr may jun jul aug sep oct nov dec].freeze
    DAY_NAMES   = %w[Sunday Monday Tuesday Wednesday Thursday Friday Saturday].freeze
    MONTH_NAMES = %w[January February March April May June July August
                     September October November December].freeze
    NUMBER = '(?<!\d)\d'
    BEGIN_ERA = '\b'
    END_ERA = '(?!(?<!\.)[a-z])'
    I = Regexp::IGNORECASE

    # ext/date/zonetab.list (gperf, case-insensitive exact match)
    ZONETAB = {
    "ut" => 0, "gmt" => 0, "est" => -18000, "edt" => -14400,
    "cst" => -21600, "cdt" => -18000, "mst" => -25200, "mdt" => -21600,
    "pst" => -28800, "pdt" => -25200, "a" => 3600, "b" => 7200,
    "c" => 10800, "d" => 14400, "e" => 18000, "f" => 21600,
    "g" => 25200, "h" => 28800, "i" => 32400, "k" => 36000,
    "l" => 39600, "m" => 43200, "n" => -3600, "o" => -7200,
    "p" => -10800, "q" => -14400, "r" => -18000, "s" => -21600,
    "t" => -25200, "u" => -28800, "v" => -32400, "w" => -36000,
    "x" => -39600, "y" => -43200, "z" => 0, "utc" => 0,
    "wet" => 0, "at" => -7200, "brst" => -7200, "ndt" => -5400,
    "art" => -10800, "adt" => -10800, "brt" => -10800, "clst" => -10800,
    "nst" => -9000, "ast" => -14400, "clt" => -14400, "akdt" => -28800,
    "ydt" => -28800, "akst" => -32400, "hadt" => -32400, "hdt" => -32400,
    "yst" => -32400, "ahst" => -36000, "cat" => 7200, "hast" => -36000,
    "hst" => -36000, "nt" => -39600, "idlw" => -43200, "bst" => 3600,
    "cet" => 3600, "fwt" => 3600, "met" => 3600, "mewt" => 3600,
    "mez" => 3600, "swt" => 3600, "wat" => 3600, "west" => 3600,
    "cest" => 7200, "eet" => 7200, "fst" => 7200, "mest" => 7200,
    "mesz" => 7200, "sast" => 7200, "sst" => -39600, "bt" => 10800,
    "eat" => 10800, "eest" => 10800, "msk" => 10800, "msd" => 14400,
    "zp4" => 14400, "zp5" => 18000, "ist" => 19800, "zp6" => 21600,
    "wast" => 7200, "cct" => 23400, "sgt" => 28800, "wadt" => 28800,
    "jst" => 32400, "kst" => 32400, "east" => -21600, "gst" => 36000,
    "eadt" => 39600, "idle" => 43200, "nzst" => 43200, "nzt" => 43200,
    "nzdt" => 46800, "afghanistan" => 16200, "alaskan" => -32400, "arab" => 10800,
    "arabian" => 14400, "arabic" => 10800, "atlantic" => -14400, "aus central" => 34200,
    "aus eastern" => 36000, "azores" => -3600, "canada central" => -21600, "cape verde" => -3600,
    "caucasus" => 14400, "cen. australia" => 34200, "central america" => -21600, "central asia" => 21600,
    "central europe" => 3600, "central european" => 3600, "central pacific" => 39600, "central" => -21600,
    "china" => 28800, "dateline" => -43200, "e. africa" => 10800, "e. australia" => 36000,
    "e. europe" => 7200, "e. south america" => -10800, "eastern" => -18000, "egypt" => 7200,
    "ekaterinburg" => 18000, "fiji" => 43200, "fle" => 7200, "greenland" => -10800,
    "greenwich" => 0, "gtb" => 7200, "hawaiian" => -36000, "india" => 19800,
    "iran" => 12600, "jerusalem" => 7200, "korea" => 32400, "mexico" => -21600,
    "mid-atlantic" => -7200, "mountain" => -25200, "myanmar" => 23400, "n. central asia" => 21600,
    "nepal" => 20700, "new zealand" => 43200, "newfoundland" => -12600, "north asia east" => 28800,
    "north asia" => 25200, "pacific sa" => -14400, "pacific" => -28800, "romance" => 3600,
    "russian" => 10800, "sa eastern" => -10800, "sa pacific" => -18000, "sa western" => -14400,
    "samoa" => -39600, "se asia" => 25200, "malay peninsula" => 28800, "south africa" => 7200,
    "sri lanka" => 21600, "taipei" => 28800, "tasmania" => 36000, "tokyo" => 32400,
    "tonga" => 46800, "us eastern" => -18000, "us mountain" => -25200, "vladivostok" => 36000,
    "w. australia" => 28800, "w. central africa" => 3600, "w. europe" => 3600, "west asia" => 18000,
    "west pacific" => 36000, "yakutsk" => 32400, "acdt" => 37800, "acst" => 34200,
    "act" => -18000, "acwst" => 31500, "aedt" => 39600, "aest" => 36000,
    "aft" => 16200, "almt" => 21600, "anast" => 43200, "anat" => 43200,
    "aoe" => -43200, "aqtt" => 18000, "awdt" => 32400, "awst" => 28800,
    "azost" => 0, "azot" => -3600, "azst" => 18000, "azt" => 14400,
    "bnt" => 28800, "bot" => -14400, "btt" => 21600, "cast" => 28800,
    "chadt" => 49500, "chast" => 45900, "chost" => 32400, "chot" => 28800,
    "chst" => 36000, "chut" => 36000, "cidst" => -14400, "cist" => -18000,
    "ckt" => -36000, "cot" => -18000, "cvt" => -3600, "cxt" => 25200,
    "davt" => 25200, "ddut" => 36000, "easst" => -18000, "ect" => -18000,
    "egst" => 0, "egt" => -3600, "fet" => 10800, "fjst" => 46800,
    "fjt" => 43200, "fkst" => -10800, "fkt" => -14400, "fnt" => -7200,
    "galt" => -21600, "gamt" => -32400, "get" => 14400, "gft" => -10800,
    "gilt" => 43200, "gyt" => -14400, "hkt" => 28800, "hovst" => 28800,
    "hovt" => 25200, "ict" => 25200, "idt" => 10800, "iot" => 21600,
    "irdt" => 16200, "irkst" => 32400, "irkt" => 28800, "irst" => 12600,
    "kgt" => 21600, "kost" => 39600, "krast" => 28800, "krat" => 25200,
    "kuyt" => 14400, "lhdt" => 39600, "lhst" => 37800, "lint" => 50400,
    "magst" => 43200, "magt" => 39600, "mart" => -30600, "mawt" => 18000,
    "mht" => 43200, "mmt" => 23400, "mut" => 14400, "mvt" => 18000,
    "myt" => 28800, "nct" => 39600, "nfdt" => 43200, "nft" => 39600,
    "novst" => 25200, "novt" => 25200, "npt" => 20700, "nrt" => 43200,
    "nut" => -39600, "omsst" => 25200, "omst" => 21600, "orat" => 18000,
    "pet" => -18000, "petst" => 43200, "pett" => 43200, "pgt" => 36000,
    "phot" => 46800, "pht" => 28800, "pkt" => 18000, "pmdt" => -7200,
    "pmst" => -10800, "pont" => 39600, "pwt" => 32400, "pyst" => -10800,
    "qyzt" => 21600, "ret" => 14400, "rott" => -10800, "sakt" => 39600,
    "samt" => 14400, "sbt" => 39600, "sct" => 14400, "sret" => 39600,
    "srt" => -10800, "syot" => 10800, "taht" => -36000, "tft" => 18000,
    "tjt" => 18000, "tkt" => 46800, "tlt" => 32400, "tmt" => 18000,
    "tost" => 50400, "tot" => 46800, "trt" => 10800, "tvt" => 43200,
    "ulast" => 32400, "ulat" => 28800, "uyst" => -7200, "uyt" => -10800,
    "uzt" => 18000, "vet" => -14400, "vlast" => 39600, "vlat" => 36000,
    "vost" => 21600, "vut" => 39600, "wakt" => 43200, "warst" => -10800,
    "wft" => 43200, "wgst" => -3600, "wgt" => -7200, "wib" => 25200,
    "wit" => 32400, "wita" => 28800, "wt" => 0, "yakst" => 36000,
    "yakt" => 32400, "yapt" => 36000, "yekst" => 21600, "yekt" => 18000,
    }.freeze
    MAX_WORD_LENGTH = 17

    PAT_DAY = Regexp.new('\b(' + ABBR_DAYS + ')[^-/\d\s]*', I)
    PAT_TIME = Regexp.new(
      '(' \
        '' + NUMBER + '+\s*' \
        '(?:' \
          '(?:' \
            ':\s*\d+' \
            '(?:' \
              '\s*:\s*\d+(?:[,.]\d*)?' \
            ')?' \
          '|' \
            'h(?:\s*\d+m?(?:\s*\d+s?)?)?' \
          ')' \
          '(?:' \
            '\s*' \
            '[ap](?:m\b|\.m\.)' \
          ')?' \
        '|' \
          '[ap](?:m\b|\.m\.)' \
        ')' \
      ')' \
      '(?:' \
        '\s*' \
        '(' \
          '(?:gmt|utc?)?[-+]\d+(?:[,.:]\d+(?::\d+)?)?' \
        '|' \
          '(?-i:[[:alpha:].\s]+)(?:standard|daylight)\stime\b' \
        '|' \
          '(?-i:[[:alpha:]]+)(?:\sdst)?\b' \
        ')' \
      ')?', I)
    PAT_TIME2 = Regexp.new(
      '\A(\d+)h?' \
        '(?:\s*:?\s*(\d+)m?' \
          '(?:' \
            '\s*:?\s*(\d+)(?:[,.](\d+))?s?' \
          ')?' \
        ')?' \
      '(?:\s*([ap])(?:m\b|\.m\.))?', I)
    PAT_EU = Regexp.new(
      '(\'?' + NUMBER + '+)[^-\d\s]*' \
      '\s*' \
      '(' + ABBR_MONTHS + ')[^-\d\s\']*' \
      '(?:' \
        '\s*' \
        '(?:' \
          + BEGIN_ERA +
          '(c(?:e|\.e\.)|b(?:ce|\.c\.e\.)|a(?:d|\.d\.)|b(?:c|\.c\.))' \
          + END_ERA +
        ')?' \
        '\s*' \
        '(\'?-?\d+(?:(?:st|nd|rd|th)\b)?)' \
      ')?', I)
    PAT_US = Regexp.new(
      '\b(' + ABBR_MONTHS + ')[^-\d\s\']*' \
      '\s*' \
      '(\'?\d+)[^-\d\s\']*' \
      '(?:' \
        '\s*+,?' \
        '\s*+' \
        '(c(?:e|\.e\.)|b(?:ce|\.c\.e\.)|a(?:d|\.d\.)|b(?:c|\.c\.))?' \
        '\s*' \
        '(\'?-?\d+)' \
      ')?', I)
    PAT_ISO = Regexp.new('(\'?[-+]?' + NUMBER + '+)-(\d+)-(\'?-?\d+)')
    PAT_ISO21 = Regexp.new('\b(\d{2}|\d{4})?-?w(\d{2})(?:-?(\d))?\b', I)
    PAT_ISO22 = Regexp.new('-w-(\d)\b', I)
    PAT_ISO23 = Regexp.new('--(\d{2})?-(\d{2})\b')
    PAT_ISO24 = Regexp.new('--(\d{2})(\d{2})?\b')
    PAT_ISO25_0 = Regexp.new('[,.](\d{2}|\d{4})-\d{3}\b')
    PAT_ISO25 = Regexp.new('\b(\d{2}|\d{4})-(\d{3})\b')
    PAT_ISO26_0 = Regexp.new('\d-\d{3}\b')
    PAT_ISO26 = Regexp.new('\b-(\d{3})\b')
    PAT_JIS = Regexp.new('\b([mtshr])(\d+)\.(\d+)\.(\d+)', I)
    PAT_VMS11 = Regexp.new('(\'?-?' + NUMBER + '+)-(' + ABBR_MONTHS + ')[^-/.]*' \
                           '-(\'?-?\d+)', I)
    PAT_VMS12 = Regexp.new('\b(' + ABBR_MONTHS + ')[^-/.]*' \
                           '-(\'?-?\d+)(?:-(\'?-?\d+))?', I)
    PAT_SLA = Regexp.new('(\'?-?' + NUMBER + '+)/\s*(\'?\d+)(?:\D\s*(\'?-?\d+))?', I)
    PAT_DOT = Regexp.new('(\'?-?' + NUMBER + '+)\.\s*(\'?\d+)\.\s*(\'?-?\d+)', I)
    PAT_YEAR = Regexp.new('\'(\d+)\b')
    PAT_MON = Regexp.new('\b(' + ABBR_MONTHS + ')\S*', I)
    PAT_MDAY = Regexp.new('(' + NUMBER + '+)(st|nd|rd|th)\b', I)
    PAT_DDD = Regexp.new(
      '([-+]?)(' + NUMBER + '{2,14})' \
        '(?:' \
          '\s*' \
          't?' \
          '\s*' \
          '(\d{2,6})?(?:[,.](\d*))?' \
        ')?' \
        '(?:' \
          '\s*' \
          '(' \
            'z\b' \
          '|' \
            '[-+]\d{1,4}\b' \
          '|' \
            '\[[-+]?\d[^\]]*\]' \
          ')' \
        ')?', I)
    PAT_ANY_MONTH = Regexp.new(ABBR_MONTHS, I)
    PAT_BC = Regexp.new('\b(bc\b|bce\b|b\.c\.|b\.c\.e\.)', I)
    PAT_FRAG = Regexp.new('\A\s*(\d{1,2})\s*\z', I)
    PAT_CLEAN = Regexp.new('[^-+\',./:@[:alnum:]\[\]]+')

    PAT_ISO8601_EXT_DATETIME = Regexp.new(
      '\A\s*(?:([-+]?\d{2,}|-)-(\d{2})?(?:-(\d{2}))?|' \
        '([-+]?\d{2,})?-(\d{3})|' \
        '(\d{4}|\d{2})?-w(\d{2})-(\d)|' \
        '-w-(\d))' \
      '(?:t' \
      '(\d{2}):(\d{2})(?::(\d{2})(?:[,.](\d+))?)?' \
      '(z|[-+]\d{2}(?::?\d{2})?)?)?\s*\z', I)
    PAT_ISO8601_BAS_DATETIME = Regexp.new(
      '\A\s*(?:([-+]?(?:\d{4}|\d{2})|--)(\d{2}|-)(\d{2})|' \
        '([-+]?(?:\d{4}|\d{2}))(\d{3})|' \
        '-(\d{3})|' \
        '(\d{4}|\d{2})w(\d{2})(\d)|' \
        '-w(\d{2})(\d)|' \
        '-w-(\d))' \
      '(?:t?' \
      '(\d{2})(\d{2})(?:(\d{2})(?:[,.](\d+))?)?' \
      '(z|[-+]\d{2}(?:\d{2})?)?)?\s*\z', I)
    PAT_ISO8601_EXT_TIME = Regexp.new(
      '\A\s*(\d{2}):(\d{2})(?::(\d{2})(?:[,.](\d+))?' \
      '(z|[-+]\d{2}(:?\d{2})?)?)?\s*\z', I)
    PAT_ISO8601_BAS_TIME = Regexp.new(
      '\A\s*(\d{2})(\d{2})(?:(\d{2})(?:[,.](\d+))?' \
      '(z|[-+]\d{2}(\d{2})?)?)?\s*\z', I)
    PAT_RFC3339 = Regexp.new(
      '\A\s*(-?\d{4})-(\d{2})-(\d{2})' \
      '(?:t|\s)' \
      '(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?' \
      '(z|[-+]\d{2}:\d{2})\s*\z', I)
    PAT_XMLSCHEMA_DATETIME = Regexp.new(
      '\A\s*(-?\d{4,})(?:-(\d{2})(?:-(\d{2}))?)?' \
      '(?:t' \
        '(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?)?' \
      '(z|[-+]\d{2}:\d{2})?\s*\z', I)
    PAT_XMLSCHEMA_TIME = Regexp.new(
      '\A\s*(\d{2}):(\d{2}):(\d{2})(?:\.(\d+))?' \
      '(z|[-+]\d{2}:\d{2})?\s*\z', I)
    PAT_XMLSCHEMA_TRUNC = Regexp.new(
      '\A\s*(?:--(\d{2})(?:-(\d{2}))?|---(\d{2}))' \
      '(z|[-+]\d{2}:\d{2})?\s*\z', I)
    PAT_RFC2822 = Regexp.new(
      '\A\s*(?:(' + ABBR_DAYS + ')\s*,\s+)?' \
      '(\d{1,2})\s+' \
      '(' + ABBR_MONTHS + ')\s+' \
      '(-?\d{2,})\s+' \
      '(\d{2}):(\d{2})(?::(\d{2}))?\s*' \
      '([-+]\d{4}|ut|gmt|e[sd]t|c[sd]t|m[sd]t|p[sd]t|[a-ik-z])\s*\z', I)
    PAT_HTTPDATE1 = Regexp.new(
      '\A\s*(' + ABBR_DAYS + ')\s*,\s+' \
      '(\d{2})\s+' \
      '(' + ABBR_MONTHS + ')\s+' \
      '(-?\d{4})\s+' \
      '(\d{2}):(\d{2}):(\d{2})\s+' \
      '(gmt)\s*\z', I)
    PAT_HTTPDATE2 = Regexp.new(
      '\A\s*(' + DAYS + ')\s*,\s+' \
      '(\d{2})\s*-\s*' \
      '(' + ABBR_MONTHS + ')\s*-\s*' \
      '(\d{2})\s+' \
      '(\d{2}):(\d{2}):(\d{2})\s+' \
      '(gmt)\s*\z', I)
    PAT_HTTPDATE3 = Regexp.new(
      '\A\s*(' + ABBR_DAYS + ')\s+' \
      '(' + ABBR_MONTHS + ')\s+' \
      '(\d{1,2})\s+' \
      '(\d{2}):(\d{2}):(\d{2})\s+' \
      '(\d{4})\s*\z', I)
    PAT_JISX0301 = Regexp.new(
      '\A\s*([mtshr])?(\d{2})\.(\d{2})\.(\d{2})' \
      '(?:t' \
      '(?:(\d{2}):(\d{2})(?::(\d{2})(?:[,.](\d*))?)?' \
      '(z|[-+]\d{2}(?::?\d{2})?)?)?)?\s*\z', I)
    PAT_STRPTIME_ZONE = Regexp.new(
      '\A(' \
      '(?:gmt|utc?)?[-+]\d+(?:[,.:]\d+(?::\d+)?)?' \
      '|(?-i:[[:alpha:].\s]+)(?:standard|daylight)\s+time\b' \
      '|(?-i:[[:alpha:]]+)(?:\s+dst)?\b' \
      ')', I)

    HAVE_ALPHA = 1
    HAVE_DIGIT = 2
    HAVE_DASH  = 4
    HAVE_DOT   = 8
    HAVE_SLASH = 16

    SPACE = " \t\n\v\f\r"

    module_function

    def isspace(c) = !c.nil? && SPACE.include?(c)
    def isdigit(c) = !c.nil? && c >= "0" && c <= "9"
    def issign(c) = c == "-" || c == "+"

    def digit_span(s, i) = (s.index(/[^0-9]/, i) || s.length) - i

    # the first sign-or-digit and the digits after it (s3e's scan of y)
    S3E_YTOK = /[-+][0-9]*|[0-9]+/
    S3E_DIGITS = /[0-9]+/

    def s3e(h, y, m, d, bc)
      c = nil
      m = m.to_s unless m.is_a?(String)

      if !y.nil? && !m.nil? && d.nil?
        y, m, d = d, y, m
      end

      if y.nil?
        if !d.nil? && d.length > 2
          y = d
          d = nil
        end
        if !d.nil? && d.length > 0 && d[0] == "'"
          y = d
          d = nil
        end
      end

      if !y.nil? && (t = S3E_YTOK.match(y)) && t.end(0) < y.length
        y = d
        d = t[0]
      end

      if !m.nil? && (m[0] == "'" || m.length > 2)
        y, m, d = m, d, y # us -> be
      end

      if !d.nil? && (d[0] == "'" || d.length > 2)
        y, d = d, y
      end

      if !y.nil? && (t = y[S3E_YTOK])
        sign = t[0] == "-" || t[0] == "+"
        c = false if sign
        c = false if t.length - (sign ? 1 : 0) > 2
        h[:year] = t.to_i
      end

      h[:_bc] = true if bc

      if !m.nil? && (t = m[S3E_DIGITS])
        h[:mon] = t.to_i
      end

      if !d.nil? && (t = d[S3E_DIGITS])
        h[:mday] = t.to_i
      end

      h[:_comp] = c unless c.nil?
    end

    # C's str_end_with_word: length of " word" (plus the run of blanks before it)
    # at the end of s[0, l], or 0.
    def str_end_with_word(s, l, w)
      n = w.length
      return 0 if l <= n || !isspace(s[l - n - 1])
      return 0 unless s[l - n, n].casecmp(w) == 0
      n += 1
      n += 1 while l > n && isspace(s[l - n - 1])
      n
    end

    # shrink_space: each blank run becomes one " ", a trailing run is dropped
    def shrink_space(s, l)
      d = s[0, l].gsub(/[ \t\n\v\f\r]+/, " ")
      d.chop! if d.end_with?(" ")
      d
    end

    DIGITS_RE = /\G[0-9]*/
    STRTOUL_RE = /\G[ \t\n\v\f\r]*\+?([0-9]+)/

    # ruby_scan_digits: at most n (< 0: any) digits from s[i]; → [value, digits read]
    def scan_digits(s, i, n)
      d = DIGITS_RE.match(s, i)[0]
      d = d[0, n] if n >= 0 && d.length > n
      [d.to_i, d.length]
    end

    # STRTOUL(s + i): → [value, index after the number]
    def strtoul(s, i)
      m = STRTOUL_RE.match(s, i) or return [0, i]
      [m[1].to_i, m.end(0)]
    end

    FAST_OFFSET_RE = /\A([-+])([0-9][0-9])(:?)([0-9][0-9])\z/

    def zone_to_diff(str)
      # fast path for "+hh:mm" / "+hhmm"; same result as the general code below
      if (m = FAST_OFFSET_RE.match(str))
        hour = m[2].to_i
        min = m[4].to_i
        return nil if !m[3].empty? && (hour > 23 || min > 59)
        v = hour * 3600 + min * 60
        return m[1] == "-" ? -v : v
      end
      s = str.ascii_only? ? str : str.b
      l = s.length
      dst = false
      blank = s.match?(/[ \t\n\v\f\r]/)

      if !blank
        # no " word" suffix possible
      elsif (w = str_end_with_word(s, l, "time")) > 0
        wtime = w
        l -= w
        if (w = str_end_with_word(s, l, "standard")) > 0
          l -= w
        elsif (w = str_end_with_word(s, l, "daylight")) > 0
          l -= w
          dst = true
        else
          l += wtime
        end
      elsif (w = str_end_with_word(s, l, "dst")) > 0
        l -= w
        dst = true
      end

      zn = s
      sl = l
      if blank # shrunk_size: only a string with blanks can shrink
        d = shrink_space(s, l)
        if d.length < l && d.length <= MAX_WORD_LENGTH
          zn = d
          sl = d.length
        elsif d.length < l
          sl = d.length
        end
      end
      if sl > 0 && sl <= MAX_WORD_LENGTH
        z = ZONETAB[zn[0, sl].downcase]
        return dst ? z + 3600 : z if z
      end

      i = 0
      if l > 3 && (s[0, 3].casecmp("gmt") == 0 || s[0, 3].casecmp("utc") == 0)
        i = 3
        l -= 3
      end
      return nil unless issign(s[i])
      sign = s[i] == "-"
      i += 1
      l -= 1
      e = i + l
      hour = min = sec = 0
      hour, p = strtoul(s, i)
      if s[p] == ":"
        return nil if hour > 23
        min, p = strtoul(s, p + 1)
        return nil if min > 59
        if s[p] == ":"
          sec, p = strtoul(s, p + 1)
          return nil if sec > 59
        end
      elsif s[p] == "," || s[p] == "."
        return nil if hour > 23
        p += 1
        n = e - p
        n = 7 if n > 7
        sec, n = scan_digits(s, p, n)
        p += n
        if p < e && s[p] >= (sec.odd? ? "5" : "6") && s[p] <= "9"
          sec += 1 # round half to even
        end
        sec *= 36
        if sign
          hour = -hour
          sec = -sec
        end
        if n <= 2
          sec *= 10 if n == 1
          return sec + hour * 3600
        end
        r = Rational(sec, 10**(n - 2)) + hour * 3600
        return r.denominator == 1 ? r.numerator : r
      elsif l > 2
        hour, = scan_digits(s, i, 2 - l % 2)
        min, = scan_digits(s, i + 2 - l % 2, 2) if l >= 3
        sec, = scan_digits(s, i + 4 - l % 2, 2) if l >= 5
      end
      sec += min * 60 + hour * 3600
      sign ? -sec : sec
    end

    def day_num(s)
      d = s[0, 3].downcase
      ABBR_DAYS_A.index(d) || 7
    end

    def mon_num(s)
      d = s[0, 3].downcase
      (ABBR_MONTHS_A.index(d) || 12) + 1
    end

    def sec_fraction(f) = Rational(f.to_i, 10**f.length)

    def comp_year69(y) = y >= 69 ? y + 1900 : y + 2000
    def comp_year50(y) = y >= 50 ? y + 1900 : y + 2000

    # SUBS: on a match run the callback, then blank the matched text out.
    def subs(str, pat)
      m = pat.match(str) or return false
      yield m
      b = m.begin(0)
      str[b, m.end(0) - b] = " "
      true
    end

    def bc_p(b) = !b.nil? && (b[0] == "B" || b[0] == "b")

    def parse_day(str, h)
      subs(str, PAT_DAY) { |m| h[:wday] = day_num(m[1]) }
    end

    def parse_time(str, h)
      subs(str, PAT_TIME) do |m|
        h[:zone] = m[2] if m[2]
        if (m2 = PAT_TIME2.match(m[1]))
          hour = m2[1].to_i
          min = m2[2]&.to_i
          sec = m2[3]&.to_i
          f = m2[4] && sec_fraction(m2[4])
          if (p = m2[5])
            hour %= 12
            hour += 12 if p[0] == "P" || p[0] == "p"
          end
          h[:hour] = hour
          h[:min] = min unless min.nil?
          h[:sec] = sec unless sec.nil?
          h[:sec_fraction] = f unless f.nil?
        end
      end
    end

    def parse_eu(str, h)
      subs(str, PAT_EU) { |m| s3e(h, m[4], mon_num(m[2]), m[1], bc_p(m[3])) }
    end

    def parse_us(str, h)
      subs(str, PAT_US) { |m| s3e(h, m[4], mon_num(m[1]), m[2], bc_p(m[3])) }
    end

    def parse_iso(str, h)
      subs(str, PAT_ISO) { |m| s3e(h, m[1], m[2], m[3], false) }
    end

    def parse_iso2(str, h)
      return true if subs(str, PAT_ISO21) { |m|
        h[:cwyear] = m[1].to_i if m[1]
        h[:cweek] = m[2].to_i
        h[:cwday] = m[3].to_i if m[3]
      }
      return true if subs(str, PAT_ISO22) { |m| h[:cwday] = m[1].to_i }
      return true if subs(str, PAT_ISO23) { |m|
        h[:mon] = m[1].to_i if m[1]
        h[:mday] = m[2].to_i
      }
      return true if subs(str, PAT_ISO24) { |m|
        h[:mon] = m[1].to_i
        h[:mday] = m[2].to_i if m[2]
      }
      unless PAT_ISO25_0.match?(str)
        return true if subs(str, PAT_ISO25) { |m|
          h[:year] = m[1].to_i
          h[:yday] = m[2].to_i
        }
      end
      unless PAT_ISO26_0.match?(str)
        return true if subs(str, PAT_ISO26) { |m| h[:yday] = m[1].to_i }
      end
      false
    end

    def gengo(c)
      case c
      when "M", "m" then 1867
      when "T", "t" then 1911
      when "S", "s" then 1925
      when "H", "h" then 1988
      when "R", "r" then 2018
      else 0
      end
    end

    def parse_jis(str, h)
      subs(str, PAT_JIS) do |m|
        h[:year] = m[2].to_i + gengo(m[1][0])
        h[:mon] = m[3].to_i
        h[:mday] = m[4].to_i
      end
    end

    def parse_vms(str, h)
      return true if subs(str, PAT_VMS11) { |m| s3e(h, m[3], mon_num(m[2]), m[1], false) }
      subs(str, PAT_VMS12) { |m| s3e(h, m[3], mon_num(m[1]), m[2], false) }
    end

    def parse_sla(str, h)
      subs(str, PAT_SLA) { |m| s3e(h, m[1], m[2], m[3], false) }
    end

    def parse_dot(str, h)
      subs(str, PAT_DOT) { |m| s3e(h, m[1], m[2], m[3], false) }
    end

    def parse_year(str, h)
      subs(str, PAT_YEAR) { |m| h[:year] = m[1].to_i }
    end

    def parse_mon(str, h)
      subs(str, PAT_MON) { |m| h[:mon] = mon_num(m[1]) }
    end

    def parse_mday(str, h)
      subs(str, PAT_MDAY) { |m| h[:mday] = m[1].to_i }
    end

    def n2i(s, f, w) = s[f, w].to_i

    def parse_ddd_cb(m, h)
      s1, s2, s3, s4, s5 = m[1], m[2], m[3], m[4], m[5]
      neg = !s1.nil? && s1[0] == "-"
      l2 = s2.length
      tonly = s3.nil? && !s4.nil?
      case l2
      when 2
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
        else
          h[:mday] = n2i(s2, 0, 2)
        end
      when 4
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
          h[:min] = n2i(s2, l2 - 4, 2)
        else
          h[:mon] = n2i(s2, 0, 2)
          h[:mday] = n2i(s2, 2, 2)
        end
      when 6
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
          h[:min] = n2i(s2, l2 - 4, 2)
          h[:hour] = n2i(s2, l2 - 6, 2)
        else
          y = n2i(s2, 0, 2)
          y = -y if neg
          h[:year] = y
          h[:mon] = n2i(s2, 2, 2)
          h[:mday] = n2i(s2, 4, 2)
        end
      when 8, 10, 12, 14
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
          h[:min] = n2i(s2, l2 - 4, 2)
          h[:hour] = n2i(s2, l2 - 6, 2)
          h[:mday] = n2i(s2, l2 - 8, 2)
          h[:mon] = n2i(s2, l2 - 10, 2) if l2 >= 10
          if l2 == 12
            y = n2i(s2, l2 - 12, 2)
            y = -y if neg
            h[:year] = y
          end
          if l2 == 14
            y = n2i(s2, l2 - 14, 4)
            y = -y if neg
            h[:year] = y
            h[:_comp] = false
          end
        else
          y = n2i(s2, 0, 4)
          y = -y if neg
          h[:year] = y
          h[:mon] = n2i(s2, 4, 2)
          h[:mday] = n2i(s2, 6, 2)
          h[:hour] = n2i(s2, 8, 2) if l2 >= 10
          h[:min] = n2i(s2, 10, 2) if l2 >= 12
          h[:sec] = n2i(s2, 12, 2) if l2 >= 14
          h[:_comp] = false
        end
      when 3
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
          h[:min] = n2i(s2, l2 - 3, 1)
        else
          h[:yday] = n2i(s2, 0, 3)
        end
      when 5
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
          h[:min] = n2i(s2, l2 - 4, 2)
          h[:hour] = n2i(s2, l2 - 5, 1)
        else
          y = n2i(s2, 0, 2)
          y = -y if neg
          h[:year] = y
          h[:yday] = n2i(s2, 2, 3)
        end
      when 7
        if tonly
          h[:sec] = n2i(s2, l2 - 2, 2)
          h[:min] = n2i(s2, l2 - 4, 2)
          h[:hour] = n2i(s2, l2 - 6, 2)
          h[:mday] = n2i(s2, l2 - 7, 1)
        else
          y = n2i(s2, 0, 4)
          y = -y if neg
          h[:year] = y
          h[:yday] = n2i(s2, 4, 3)
        end
      end
      unless s3.nil?
        l3 = s3.length
        if !s4.nil?
          if l3 == 2 || l3 == 4 || l3 == 6
            h[:sec] = n2i(s3, l3 - 2, 2)
            h[:min] = n2i(s3, l3 - 4, 2) if l3 >= 4
            h[:hour] = n2i(s3, l3 - 6, 2) if l3 >= 6
          end
        elsif l3 == 2 || l3 == 4 || l3 == 6
          h[:hour] = n2i(s3, 0, 2)
          h[:min] = n2i(s3, 2, 2) if l3 >= 4
          h[:sec] = n2i(s3, 4, 2) if l3 >= 6
        end
      end
      h[:sec_fraction] = sec_fraction(s4) unless s4.nil?
      unless s5.nil?
        h[:zone] = s5
        if s5[0] == "["
          l5 = s5.length - 2
          inner = s5[1, l5]
          if (ci = inner.index(":"))
            zone = inner[ci + 1..]
            s5 = inner[0, ci + 1]
          else
            zone = inner
            s5 = isdigit(inner[0]) ? "+" + zone : zone
          end
          h[:zone] = zone
          h[:offset] = zone_to_diff(s5)
        end
      end
    end

    def parse_ddd(str, h)
      subs(str, PAT_DDD) { |m| parse_ddd_cb(m, h) }
    end

    def parse_bc(str, h)
      subs(str, PAT_BC) { h[:_bc] = true }
    end

    def parse_frag(str, h)
      subs(str, PAT_FRAG) do |m|
        s = m[1]
        if !h[:hour].nil? && h[:mday].nil?
          n = s.to_i
          h[:mday] = n if n >= 1 && n <= 31
        end
        if !h[:mday].nil? && h[:hour].nil?
          n = s.to_i
          h[:hour] = n if n >= 0 && n <= 24
        end
      end
    end

    def check_class(s)
      f = 0
      f |= HAVE_ALPHA if s.match?(/[A-Za-z]/)
      f |= HAVE_DIGIT if s.match?(/[0-9]/)
      f |= HAVE_DASH if s.include?("-")
      f |= HAVE_DOT if s.include?(".")
      f |= HAVE_SLASH if s.include?("/")
      f
    end

    def have?(str, x) = (check_class(str) & x) == x

    def _parse(str, comp)
      str = str.gsub(PAT_CLEAN, " ")
      h = { _comp: comp }

      # HAVE_ELEM_P re-reads the (mutated) string each time; the classes only
      # change when a SUBS succeeded, so cc is refreshed exactly then.
      cc = check_class(str)
      # PAT_EU/US/VMS*/MON all need a month abbreviation; SUBS only blanks text
      # out, so once absent it stays absent (a pure short cut, no behavior change).
      mon = cc & HAVE_ALPHA != 0 && PAT_ANY_MONTH.match?(str)
      cc = check_class(str) if cc & HAVE_ALPHA != 0 && parse_day(str, h)
      cc = check_class(str) if cc & HAVE_DIGIT != 0 && parse_time(str, h)

      ok = catch(:ok) do
        if mon && cc & (HAVE_ALPHA | HAVE_DIGIT) == (HAVE_ALPHA | HAVE_DIGIT)
          throw :ok, true if parse_eu(str, h)
          throw :ok, true if parse_us(str, h)
        end
        throw :ok, true if cc & (HAVE_DIGIT | HAVE_DASH) == (HAVE_DIGIT | HAVE_DASH) && parse_iso(str, h)
        throw :ok, true if cc & (HAVE_DIGIT | HAVE_DOT) == (HAVE_DIGIT | HAVE_DOT) && parse_jis(str, h)
        throw :ok, true if mon && cc & (HAVE_ALPHA | HAVE_DIGIT | HAVE_DASH) == (HAVE_ALPHA | HAVE_DIGIT | HAVE_DASH) && parse_vms(str, h)
        throw :ok, true if cc & (HAVE_DIGIT | HAVE_SLASH) == (HAVE_DIGIT | HAVE_SLASH) && parse_sla(str, h)
        throw :ok, true if cc & (HAVE_DIGIT | HAVE_DOT) == (HAVE_DIGIT | HAVE_DOT) && parse_dot(str, h)
        throw :ok, true if cc & HAVE_DIGIT != 0 && parse_iso2(str, h)
        throw :ok, true if cc & HAVE_DIGIT != 0 && parse_year(str, h)
        throw :ok, true if mon && cc & HAVE_ALPHA != 0 && parse_mon(str, h)
        throw :ok, true if cc & HAVE_DIGIT != 0 && parse_mday(str, h)
        throw :ok, true if cc & HAVE_DIGIT != 0 && parse_ddd(str, h)
        false
      end
      cc = check_class(str) if ok

      cc = check_class(str) if cc & HAVE_ALPHA != 0 && parse_bc(str, h)
      parse_frag(str, h) if cc & HAVE_DIGIT != 0

      if h.delete(:_bc)
        h[:cwyear] = -h[:cwyear] + 1 unless h[:cwyear].nil?
        h[:year] = -h[:year] + 1 unless h[:year].nil?
      end

      if h.delete(:_comp)
        y = h[:cwyear]
        h[:cwyear] = comp_year69(y) if !y.nil? && y >= 0 && y <= 99
        y = h[:year]
        h[:year] = comp_year69(y) if !y.nil? && y >= 0 && y <= 99
      end

      zone = h[:zone]
      h[:offset] = zone_to_diff(zone) if !zone.nil? && h[:offset].nil?
      h
    end

    def set_zone(h, z)
      h[:zone] = z
      h[:offset] = zone_to_diff(z)
    end

    def _iso8601(str)
      h = {}
      if (m = PAT_ISO8601_EXT_DATETIME.match(str))
        s = m
        if s[1]
          h[:mday] = s[3].to_i if s[3]
          if s[1] != "-"
            y = s[1].to_i
            y = comp_year69(y) if s[1].length < 4
            h[:year] = y
          end
          if s[2].nil?
            return h if s[1] != "-"
          else
            h[:mon] = s[2].to_i
          end
        elsif s[5]
          h[:yday] = s[5].to_i
          if s[4]
            y = s[4].to_i
            y = comp_year69(y) if s[4].length < 4
            h[:year] = y
          end
        elsif s[8]
          h[:cweek] = s[7].to_i
          h[:cwday] = s[8].to_i
          if s[6]
            y = s[6].to_i
            y = comp_year69(y) if s[6].length < 4
            h[:cwyear] = y
          end
        elsif s[9]
          h[:cwday] = s[9].to_i
        end
        if s[10]
          h[:hour] = s[10].to_i
          h[:min] = s[11].to_i
          h[:sec] = s[12].to_i if s[12]
        end
        h[:sec_fraction] = sec_fraction(s[13]) if s[13]
        set_zone(h, s[14]) if s[14]
      elsif (s = PAT_ISO8601_BAS_DATETIME.match(str))
        if s[3]
          h[:mday] = s[3].to_i
          if s[1] != "--"
            y = s[1].to_i
            y = comp_year69(y) if s[1].length < 4
            h[:year] = y
          end
          if s[2][0] == "-"
            return h if s[1] != "--"
          else
            h[:mon] = s[2].to_i
          end
        elsif s[5]
          h[:yday] = s[5].to_i
          y = s[4].to_i
          y = comp_year69(y) if s[4].length < 4
          h[:year] = y
        elsif s[6]
          h[:yday] = s[6].to_i
        elsif s[9]
          h[:cweek] = s[8].to_i
          h[:cwday] = s[9].to_i
          y = s[7].to_i
          y = comp_year69(y) if s[7].length < 4
          h[:cwyear] = y
        elsif s[11]
          h[:cweek] = s[10].to_i
          h[:cwday] = s[11].to_i
        elsif s[12]
          h[:cwday] = s[12].to_i
        end
        if s[13]
          h[:hour] = s[13].to_i
          h[:min] = s[14].to_i
          h[:sec] = s[15].to_i if s[15]
        end
        h[:sec_fraction] = sec_fraction(s[16]) if s[16]
        set_zone(h, s[17]) if s[17]
      elsif (s = PAT_ISO8601_EXT_TIME.match(str) || PAT_ISO8601_BAS_TIME.match(str))
        h[:hour] = s[1].to_i
        h[:min] = s[2].to_i
        h[:sec] = s[3].to_i if s[3]
        h[:sec_fraction] = sec_fraction(s[4]) if s[4]
        set_zone(h, s[5]) if s[5]
      end
      h
    end

    def _rfc3339(str)
      h = {}
      if (s = PAT_RFC3339.match(str))
        h[:year] = s[1].to_i
        h[:mon] = s[2].to_i
        h[:mday] = s[3].to_i
        h[:hour] = s[4].to_i
        h[:min] = s[5].to_i
        h[:sec] = s[6].to_i
        set_zone(h, s[8])
        h[:sec_fraction] = sec_fraction(s[7]) if s[7]
      end
      h
    end

    def _xmlschema(str)
      h = {}
      if (s = PAT_XMLSCHEMA_DATETIME.match(str))
        h[:year] = s[1].to_i
        h[:mon] = s[2].to_i if s[2]
        h[:mday] = s[3].to_i if s[3]
        h[:hour] = s[4].to_i if s[4]
        h[:min] = s[5].to_i if s[5]
        h[:sec] = s[6].to_i if s[6]
        h[:sec_fraction] = sec_fraction(s[7]) if s[7]
        set_zone(h, s[8]) if s[8]
      elsif (s = PAT_XMLSCHEMA_TIME.match(str))
        h[:hour] = s[1].to_i
        h[:min] = s[2].to_i
        h[:sec] = s[3].to_i if s[3]
        h[:sec_fraction] = sec_fraction(s[4]) if s[4]
        set_zone(h, s[5]) if s[5]
      elsif (s = PAT_XMLSCHEMA_TRUNC.match(str))
        h[:mon] = s[1].to_i if s[1]
        h[:mday] = s[2].to_i if s[2]
        h[:mday] = s[3].to_i if s[3]
        set_zone(h, s[4]) if s[4]
      end
      h
    end

    def _rfc2822(str)
      h = {}
      if (s = PAT_RFC2822.match(str))
        h[:wday] = day_num(s[1]) if s[1]
        h[:mday] = s[2].to_i
        h[:mon] = mon_num(s[3])
        y = s[4].to_i
        y = comp_year50(y) if s[4].length < 4
        h[:year] = y
        h[:hour] = s[5].to_i
        h[:min] = s[6].to_i
        h[:sec] = s[7].to_i if s[7]
        set_zone(h, s[8])
      end
      h
    end

    def _httpdate(str)
      h = {}
      if (s = PAT_HTTPDATE1.match(str))
        h[:wday] = day_num(s[1])
        h[:mday] = s[2].to_i
        h[:mon] = mon_num(s[3])
        h[:year] = s[4].to_i
        h[:hour] = s[5].to_i
        h[:min] = s[6].to_i
        h[:sec] = s[7].to_i
        h[:zone] = s[8]
        h[:offset] = 0
      elsif (s = PAT_HTTPDATE2.match(str))
        h[:wday] = day_num(s[1])
        h[:mday] = s[2].to_i
        h[:mon] = mon_num(s[3])
        y = s[4].to_i
        y = comp_year69(y) if y >= 0 && y <= 99
        h[:year] = y
        h[:hour] = s[5].to_i
        h[:min] = s[6].to_i
        h[:sec] = s[7].to_i
        h[:zone] = s[8]
        h[:offset] = 0
      elsif (s = PAT_HTTPDATE3.match(str))
        h[:wday] = day_num(s[1])
        h[:mon] = mon_num(s[2])
        h[:mday] = s[3].to_i
        h[:hour] = s[4].to_i
        h[:min] = s[5].to_i
        h[:sec] = s[6].to_i
        h[:year] = s[7].to_i
      end
      h
    end

    def _jisx0301(str)
      h = {}
      s = PAT_JISX0301.match(str) or return _iso8601(str)
      h[:year] = s[2].to_i + gengo(s[1].nil? ? "H" : s[1][0])
      h[:mon] = s[3].to_i
      h[:mday] = s[4].to_i
      if s[5]
        h[:hour] = s[5].to_i
        h[:min] = s[6].to_i if s[6]
        h[:sec] = s[7].to_i if s[7]
      end
      h[:sec_fraction] = sec_fraction(s[8]) if s[8]
      set_zone(h, s[9]) if s[9]
      h
    end

    # ---- strptime (date_strptime.c) ----

    NUM_PAT_CHARS = "CDdeFGgHIjkLlMmNQRrSsTUuVvWwXxYy"

    def num_pattern_p(fmt, i)
      c = fmt[i]
      return true if isdigit(c)
      if c == "%"
        i += 1
        i += 1 if fmt[i] == "E" || fmt[i] == "O"
        c = fmt[i]
        return true if !c.nil? && (NUM_PAT_CHARS.include?(c) || isdigit(c))
      end
      false
    end

    # → [value, new index] or nil (no digit)
    def read_digits(str, si, width)
      d = DIGITS_RE.match(str, si)[0]
      return nil if d.empty?
      d = d[0, width] if d.length > width
      [d.to_i, si + d.length]
    end

    def head_match(str, si, name, len)
      str.length - si >= len && str[si, len].casecmp(name[0, len]) == 0
    end

    INF = Float::INFINITY

    # date__strptime_internal on str[si..]; → the index past what was read, or
    # nil when it failed.
    def strptime_i(str, si, fmt, h)
      slen = str.length
      flen = fmt.length
      fi = 0
      while fi < flen
        fc = fmt[fi]
        if isspace(fc)
          si += 1 while si < slen && isspace(str[si])
          fi += 1
          fi += 1 while fi < flen && isspace(fmt[fi])
          next
        end
        return nil if si >= slen

        if fc != "%"
          return nil if str[si] != fc
          si += 1
          fi += 1
          next
        end

        ordinal = false
        while true # again
          fi += 1
          c = fmt[fi]
          case c
          when "E"
            nc = fmt[fi + 1]
            next if !nc.nil? && "cCxXyY".include?(nc)
            fi -= 1
            ordinal = true
          when "O"
            nc = fmt[fi + 1]
            next if !nc.nil? && "deHImMSuUVwWy".include?(nc)
            fi -= 1
            ordinal = true
          when ":"
            i = 1
            i += 1 while i < 3 && fi + i < flen && fmt[fi + i] == ":"
            return nil unless fmt[fi + i] == "z"
            fi += i - 1
            next
          end
          break
        end
        if ordinal
          return nil if str[si] != fmt[fi]
          si += 1
          fi += 1
          next
        end

        case c
        when "A", "a"
          i = 0
          found = false
          while i < 7
            name = DAY_NAMES[i]
            l = name.length
            if head_match(str, si, name, l) || head_match(str, si, name, l = 3)
              si += l
              h[:wday] = i
              found = true
              break
            end
            i += 1
          end
          return nil unless found
        when "B", "b", "h"
          i = 0
          found = false
          while i < 12
            name = MONTH_NAMES[i]
            l = name.length
            if head_match(str, si, name, l) || head_match(str, si, name, l = 3)
              si += l
              h[:mon] = i + 1
              found = true
              break
            end
            i += 1
          end
          return nil unless found
        when "C"
          r = read_digits(str, si, num_pattern_p(fmt, fi + 1) ? 2 : INF) or return nil
          n, si = r
          h[:_cent] = n
        when "c"
          si = strptime_i(str, si, "%a %b %e %H:%M:%S %Y", h) or return nil
        when "D", "x"
          si = strptime_i(str, si, "%m/%d/%y", h) or return nil
        when "d", "e"
          if str[si] == " "
            r = read_digits(str, si + 1, 1) or return nil
            n, si = r
          else
            r = read_digits(str, si, 2) or return nil
            n, si = r
          end
          return nil if n < 1 || n > 31
          h[:mday] = n
        when "F"
          si = strptime_i(str, si, "%Y-%m-%d", h) or return nil
        when "G"
          r = read_digits(str, si, num_pattern_p(fmt, fi + 1) ? 4 : INF) or return nil
          n, si = r
          h[:cwyear] = n
        when "g"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n > 99
          h[:cwyear] = n
          h[:_cent] = n >= 69 ? 19 : 20 if h[:_cent].nil?
        when "H", "k"
          if str[si] == " "
            r = read_digits(str, si + 1, 1) or return nil
            n, si = r
          else
            r = read_digits(str, si, 2) or return nil
            n, si = r
          end
          return nil if n > 24
          h[:hour] = n
        when "I", "l"
          if str[si] == " "
            r = read_digits(str, si + 1, 1) or return nil
            n, si = r
          else
            r = read_digits(str, si, 2) or return nil
            n, si = r
          end
          return nil if n < 1 || n > 12
          h[:hour] = n
        when "j"
          r = read_digits(str, si, 3) or return nil
          n, si = r
          return nil if n < 1 || n > 366
          h[:yday] = n
        when "L", "N"
          sign = 1
          if issign(str[si])
            sign = -1 if str[si] == "-"
            si += 1
          end
          osi = si
          w = num_pattern_p(fmt, fi + 1) ? (c == "L" ? 3 : 9) : INF
          r = read_digits(str, si, w) or return nil
          n, si = r
          n = -n if sign == -1
          h[:sec_fraction] = Rational(n, 10**(si - osi))
        when "M"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n > 59
          h[:min] = n
        when "m"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n < 1 || n > 12
          h[:mon] = n
        when "n", "t"
          si = strptime_i(str, si, " ", h) or return nil
        when "P", "p"
          return nil if slen - si < 2
          ch = str[si]
          hour = (ch == "P" || ch == "p") ? 12 : 0
          return nil if hour == 0 && !(ch == "A" || ch == "a")
          if (ch = str[si + 1]) == "."
            return nil if slen - si < 4 || str[si + 3] != "."
            si += 2
            ch = str[si]
          end
          return nil unless ch == "M" || ch == "m"
          si += 2
          h[:_merid] = hour
        when "Q"
          sign = 1
          if str[si] == "-"
            sign = -1
            si += 1
          end
          r = read_digits(str, si, INF) or return nil
          n, si = r
          n = -n if sign == -1
          h[:seconds] = Rational(n, 1000)
        when "R"
          si = strptime_i(str, si, "%H:%M", h) or return nil
        when "r"
          si = strptime_i(str, si, "%I:%M:%S %p", h) or return nil
        when "S"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n > 60
          h[:sec] = n
        when "s"
          sign = 1
          if str[si] == "-"
            sign = -1
            si += 1
          end
          r = read_digits(str, si, INF) or return nil
          n, si = r
          n = -n if sign == -1
          h[:seconds] = n
        when "T", "X"
          si = strptime_i(str, si, "%H:%M:%S", h) or return nil
        when "U", "W"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n > 53
          h[c == "U" ? :wnum0 : :wnum1] = n
        when "u"
          r = read_digits(str, si, 1) or return nil
          n, si = r
          return nil if n < 1 || n > 7
          h[:cwday] = n
        when "V"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n < 1 || n > 53
          h[:cweek] = n
        when "v"
          si = strptime_i(str, si, "%e-%b-%Y", h) or return nil
        when "w"
          r = read_digits(str, si, 1) or return nil
          n, si = r
          return nil if n > 6
          h[:wday] = n
        when "Y"
          sign = 1
          if issign(str[si])
            sign = -1 if str[si] == "-"
            si += 1
          end
          r = read_digits(str, si, num_pattern_p(fmt, fi + 1) ? 4 : INF) or return nil
          n, si = r
          n = -n if sign == -1
          h[:year] = n
        when "y"
          r = read_digits(str, si, 2) or return nil
          n, si = r
          return nil if n > 99
          h[:year] = n
          h[:_cent] = n >= 69 ? 19 : 20 if h[:_cent].nil?
        when "Z", "z"
          m = PAT_STRPTIME_ZONE.match(str[si..]) or return nil
          s = m[1]
          si += s.length
          h[:zone] = s
          h[:offset] = zone_to_diff(s)
        when "%"
          return nil if str[si] != "%"
          si += 1
        when "+"
          si = strptime_i(str, si, "%a %b %e %H:%M:%S %Z %Y", h) or return nil
        else
          return nil if str[si] != "%"
          si += 1
          if fi < flen
            return nil if si >= slen || str[si] != fmt[fi]
            si += 1
          end
        end
        fi += 1
      end
      si
    end

    def _strptime(str, fmt)
      h = {}
      si = strptime_i(str, 0, fmt, h) or return nil
      h[:leftover] = str[si..] if str.length > si
      if (cent = h.delete(:_cent))
        h[:cwyear] = h[:cwyear] + cent * 100 unless h[:cwyear].nil?
        h[:year] = h[:year] + cent * 100 unless h[:year].nil?
      end
      if (merid = h.delete(:_merid))
        h[:hour] = h[:hour] % 12 + merid unless h[:hour].nil?
      end
      h
    end

    # ---- frags → Date (date_core.c) ----

    COMPLETE_TAB = [
      [:time, %i[hour min sec]],
      [nil, %i[jd]],
      [:ordinal, %i[year yday hour min sec]],
      [:civil, %i[year mon mday hour min sec]],
      [:commercial, %i[cwyear cweek cwday hour min sec]],
      [:wday, %i[wday hour min sec]],
      [:wnum0, %i[year wnum0 wday hour min sec]],
      [:wnum1, %i[year wnum1 wday hour min sec]],
      [nil, %i[cwyear cweek wday hour min sec]],
      [nil, %i[year wnum0 cwday hour min sec]],
      [nil, %i[year wnum1 cwday hour min sec]],
    ].freeze

    UNIX_EPOCH_IN_CJD = 2440588

    def rewrite_frags(h)
      seconds = h.delete(:seconds)
      unless seconds.nil?
        offset = h[:offset]
        seconds += offset unless offset.nil?
        d = seconds.div(86400)
        fr = seconds % 86400
        hr = fr.div(3600)
        fr %= 3600
        min = fr.div(60)
        fr %= 60
        s = fr.div(1)
        fr %= 1
        h[:jd] = UNIX_EPOCH_IN_CJD + d
        h[:hour] = hr
        h[:min] = min
        h[:sec] = s
        h[:sec_fraction] = fr
      end
      h
    end

    def complete_frags(klass, h)
      k = a = nil
      eno = 0
      COMPLETE_TAB.each do |x|
        n = x[1].count { |key| !h[key].nil? }
        if n > eno
          eno = n
          k, a = x
        end
      end

      d = nil
      if !k.nil? && a.length > eno
        case k
        when :ordinal
          if h[:year].nil?
            d ||= Date.today
            h[:year] = d.year
          end
          h[:yday] = 1 if h[:yday].nil?
        when :civil, :commercial, :wnum0, :wnum1
          a.each do |e|
            break unless h[e].nil?
            d ||= Date.today
            h[e] = d.__send__(e)
          end
          case k
          when :civil
            h[:mon] = 1 if h[:mon].nil?
            h[:mday] = 1 if h[:mday].nil?
          when :commercial
            h[:cweek] = 1 if h[:cweek].nil?
            h[:cwday] = 1 if h[:cwday].nil?
          when :wnum0
            h[:wnum0] = 0 if h[:wnum0].nil?
            h[:wday] = 0 if h[:wday].nil?
          when :wnum1
            h[:wnum1] = 0 if h[:wnum1].nil?
            h[:wday] = 1 if h[:wday].nil?
          end
        when :wday
          d ||= Date.today
          h[:jd] = (d - d.wday + h[:wday]).jd
        end
      end

      if k == :time && klass <= DateTime
        d ||= Date.today
        h[:jd] = d.jd if h[:jd].nil?
      end

      h[:hour] = 0 if h[:hour].nil?
      h[:min] = 0 if h[:min].nil?
      if h[:sec].nil?
        h[:sec] = 0
      elsif h[:sec] > 59
        h[:sec] = 59
      end
      h
    end

    def valid_date_frags(h, sg)
      jd = h[:jd]
      return jd unless jd.nil?

      if !(yday = h[:yday]).nil? && !(year = h[:year]).nil?
        jd = Date.__valid_ordinal_jd_i(year, yday, sg)
        return jd unless jd.nil?
      end

      if !(mday = h[:mday]).nil? && !(mon = h[:mon]).nil? && !(year = h[:year]).nil?
        jd = Date.__valid_civil_jd_i(year, mon, mday, sg)
        return jd unless jd.nil?
      end

      wday = h[:cwday]
      if wday.nil?
        wday = h[:wday]
        wday = 7 if !wday.nil? && wday.zero?
      end
      if !wday.nil? && !(week = h[:cweek]).nil? && !(year = h[:cwyear]).nil?
        jd = Date.__valid_commercial_jd_i(year, week, wday, sg)
        return jd unless jd.nil?
      end

      wday = h[:wday]
      if wday.nil?
        wday = h[:cwday]
        wday = 0 if !wday.nil? && wday == 7
      end
      if !wday.nil? && !(week = h[:wnum0]).nil? && !(year = h[:year]).nil?
        jd = Date.__valid_weeknum_jd(year, week, wday, 0, sg)
        return jd unless jd.nil?
      end

      wday = h[:wday]
      wday = h[:cwday] if wday.nil?
      wday = (wday - 1) % 7 unless wday.nil?
      if !wday.nil? && !(week = h[:wnum1]).nil? && !(year = h[:year]).nil?
        jd = Date.__valid_weeknum_jd(year, week, wday, 1, sg)
        return jd unless jd.nil?
      end
      nil
    end
  end
  private_constant :Parse__

  # NUM2INT: a C int, else RangeError
  def self.__num2int(v)
    v = v.to_i
    if v > 2147483647 || v < -2147483648
      raise RangeError, "integer #{v} too big to convert to 'int'"
    end
    v
  end

  # rt__valid_*_p: the year stays a VALUE, the other fields are NUM2INT'd.
  def self.__valid_civil_jd_i(y, m, d, sg)
    _valid_civil_jd(y.to_i, __num2int(m), __num2int(d), sg)
  end
  def self.__valid_ordinal_jd_i(y, d, sg) = _valid_ordinal_jd(y.to_i, __num2int(d), sg)
  def self.__valid_commercial_jd_i(y, w, d, sg) = _valid_commercial_jd(y.to_i, __num2int(w), __num2int(d), sg)

  # c_find_fdoy: the first existing day of year y.
  def self.__find_fdoy(y, sg)
    1.upto(30) do |d|
      jd = _valid_civil_jd(y, 1, d, sg)
      return jd if jd
    end
    nil
  end

  def self.__weeknum_to_jd(y, w, d, f, sg)
    rjd2 = __find_fdoy(y, sg) + 6
    (rjd2 - ((rjd2 - f) + 1) % 7 - 7) + 7 * w + d
  end

  def self.__jd_to_weeknum(jd, f, sg)
    ry, = jd_to_civil(jd, sg)
    rjd = __find_fdoy(ry, sg) + 6
    j = jd - (rjd - ((rjd - f) + 1) % 7) + 7
    [ry, j.div(7), j % 7]
  end

  def self.__valid_weeknum_jd(y, w, d, f, sg)
    y = y.to_i
    w = __num2int(w)
    d = __num2int(d)
    d += 7 if d < 0
    if w < 0
      rjd2 = __weeknum_to_jd(y + 1, 1, f, f, sg)
      ry2, rw2, = __jd_to_weeknum(rjd2 + w * 7, f, sg)
      return nil if ry2 != y
      w = rw2
    end
    jd = __weeknum_to_jd(y, w, d, f, sg)
    ry2, rw, rd = __jd_to_weeknum(jd, f, sg)
    return nil if y != ry2 || w != rw || d != rd
    jd
  end

  def self.__string_value(str)
    return str if str.is_a?(String)
    if str.respond_to?(:to_str)
      s = str.to_str
      return s if s.is_a?(String)
    end
    raise TypeError, "no implicit conversion of #{str.nil? ? 'nil' : str.class} into String"
  end

  def self.__check_limit(str, opt)
    str = __string_value(str)
    limit = opt.empty? ? 128 : opt[:limit]
    if !limit.nil? && str.bytesize > limit
      raise ArgumentError, "string length (#{str.bytesize}) exceeds the limit #{limit}"
    end
    str
  end

  def self.__check_ascii(str, what = "string")
    enc = str.encoding
    unless enc.ascii_compatible?
      raise ArgumentError, "#{what} should have ASCII compatible encoding"
    end
  end

  def self._parse(str, comp = true, **opt)
    str = __check_limit(str, opt)
    __check_ascii(str)
    Parse__._parse(str, comp)
  end

  def self._iso8601(str, **opt)   = __fmt_hash(:_iso8601, str, opt)
  def self._rfc3339(str, **opt)   = __fmt_hash(:_rfc3339, str, opt)
  def self._xmlschema(str, **opt) = __fmt_hash(:_xmlschema, str, opt)
  def self._rfc2822(str, **opt)   = __fmt_hash(:_rfc2822, str, opt)
  def self._httpdate(str, **opt)  = __fmt_hash(:_httpdate, str, opt)
  def self._jisx0301(str, **opt)  = __fmt_hash(:_jisx0301, str, opt)
  class << self; alias_method :_rfc822, :_rfc2822; end

  # nil passes the limit check and matches nothing ({}), as in C.
  def self.__fmt_hash(kind, str, opt)
    return {} if str.nil?
    str = __check_limit(str, opt)
    Parse__.__send__(kind, str)
  end

  NO_FMT__ = Object.new.freeze
  private_constant :NO_FMT__

  def self._strptime(str, fmt = NO_FMT__)
    fmt_default = fmt.equal?(NO_FMT__)
    __strptime_hash(str, fmt_default ? "%F" : fmt, fmt_default)
  end

  def self.__strptime_hash(str, fmt, fmt_default)
    str = __string_value(str)
    fmt = __string_value(fmt) unless fmt_default
    __check_ascii(str)
    __check_ascii(fmt, "format") unless fmt_default
    Parse__._strptime(str, fmt)
  end

  # c_valid_start_p: NaN or a finite day outside the reform window is ignored.
  def self.__valid_start(sg)
    f = __sgnum(sg)
    return false unless f.is_a?(Numeric)
    f = f.to_f
    return false if f.nan?
    return true if f.infinite?
    f >= 2298874 && f <= 2426355
  end

  def self.__fix_sg(sg)
    return sg if __valid_start(sg)
    warn "invalid start is ignored" if $VERBOSE
    ITALY
  end

  def self.__frags_jd(hash, sg)
    if hash[:jd].nil? && hash[:yday].nil? &&
       !hash[:year].nil? && !hash[:mon].nil? && !hash[:mday].nil?
      [__valid_civil_jd_i(hash[:year], hash[:mon], hash[:mday], sg), true]
    else
      Parse__.rewrite_frags(hash)
      Parse__.complete_frags(self, hash)
      [Parse__.valid_date_frags(hash, sg), false]
    end
  end

  # d_new_by_frags
  def self.__new_by_frags(hash, sg)
    sg = __fix_sg(sg)
    raise Date::Error, "invalid date" if hash.nil?
    jd, = __frags_jd(hash, sg)
    raise Date::Error, "invalid date" if jd.nil?
    new!(jd, 0, 0, 0, sg)
  end

  def self.parse(str = "-4712-01-01", comp = true, sg = ITALY, **opt)
    __new_by_frags(_parse(str, comp, **opt), sg)
  end

  def self.strptime(str = "-4712-01-01", fmt = "%F", sg = ITALY)
    __new_by_frags(_strptime(str, fmt), sg)
  end

  def self.iso8601(str = "-4712-01-01", sg = ITALY, **opt) = __new_by_frags(_iso8601(str, **opt), sg)
  def self.rfc3339(str = "-4712-01-01T00:00:00+00:00", sg = ITALY, **opt) = __new_by_frags(_rfc3339(str, **opt), sg)
  def self.xmlschema(str = "-4712-01-01", sg = ITALY, **opt) = __new_by_frags(_xmlschema(str, **opt), sg)
  def self.rfc2822(str = "Mon, 1 Jan -4712 00:00:00 +0000", sg = ITALY, **opt) = __new_by_frags(_rfc2822(str, **opt), sg)
  class << self; alias_method :rfc822, :rfc2822; end
  def self.httpdate(str = "Mon, 01 Jan -4712 00:00:00 GMT", sg = ITALY, **opt) = __new_by_frags(_httpdate(str, **opt), sg)
  def self.jisx0301(str = "-4712-01-01", sg = ITALY, **opt) = __new_by_frags(_jisx0301(str, **opt), sg)

  # "+9" / "+09:00" / "Z" / "JST" / "EST5EDT"... → seconds (C: date_zone_to_diff);
  # nil when it is not a zone.
  def self.__zone_to_offset(z)
    Parse__.zone_to_diff(z.to_s)
  end
end


# DateTime adds a time of day and a UTC offset on top of Date.
class DateTime < Date
  private def __default_strftime_format = "%FT%T%:z"

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
    if jd.is_a?(Numeric) && !jd.is_a?(Integer) && h == 0 && min.nil?   # a day fraction is the time of day
      whole = jd.floor
      h = (jd - whole) * 24
      jd = whole
    end
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
    ns = frac * 1_000_000_000
    # An exact (Rational) second keeps its sub-nanosecond part, which %12N needs.
    [0, h * 3600 + min * 60 + sec, ns.is_a?(Float) ? ns.round : ns]
  end

  # Accepts a day fraction or a zone string ("+9" / "+09:00" / "Z" / "JST").
  # A numeric outside (-1, 1) is not a day fraction — CRuby answers 0 there.
  def self.__offset_seconds(of)
    case of
    when String
      # C offset_to_sec: a string that is not a zone, or out of range, is 0
      sec = __zone_to_offset(of)
      sec.is_a?(Integer) && sec.abs <= 86400 ? sec : 0
    when Numeric
      return 0 if of <= -1 || of >= 1
      (of * 86400).round
    else 0
    end
  end

  public :offset, :zone
  # the same instant under another offset
  def new_offset(of = 0)
    nof = DateTime.__offset_seconds(of)
    carry, df = (@df - @of + nof).divmod(86400)
    self.class.new!(@jd + carry, df, @sf, nof, @sg)
  end

  def hour = @df / 3600
  def min = (@df % 3600) / 60
  alias_method :minute, :min
  def sec = @df % 60
  alias_method :second, :sec
  def sec_fraction = Rational(@sf, 1_000_000_000)
  alias_method :second_fraction, :sec_fraction

  def to_s = __strftimev("%Y-%m-%dT%H:%M:%S%:z")
  def iso8601(n = 0) = __strftimev("%Y-%m-%d") + __iso8601_timediv(n)
  alias_method :xmlschema, :iso8601
  def rfc3339(n = 0) = iso8601(n)
  def jisx0301(n = 0) = super() + __iso8601_timediv(n)

  # iso8601_timediv: n is NUM2LONG'd (a Float truncates)
  private def __iso8601_timediv(n)
    raise TypeError, "no implicit conversion of #{n.class} into Integer" unless n.respond_to?(:to_int)
    n = n.to_int
    __strftimev("T%H:%M:%S" + (n > 0 ? ".%#{n}N" : "") + "%:z")
  end
  def to_date = Date.new!(@jd, 0, 0, 0, @sg)
  def to_datetime = self

  # The instant comes from the JD (so a Julian date converts correctly), and the
  # Time keeps this DateTime's own offset whatever the local zone is (CRuby).
  def to_time
    Time.at(Rational(__epoch_seconds) + sec_fraction, in: @of)
  end

  def deconstruct_keys(keys)
    all = { year: year, month: month, day: day, yday: yday, wday: wday,
            hour: hour, min: min, sec: sec, sec_fraction: sec_fraction, zone: zone }
    return all if keys.nil?
    raise TypeError, "wrong argument type #{keys.class} (expected Array or nil)" unless keys.is_a?(Array)
    h = {}
    keys.each { |k| h[k] = all[k] if all.key?(k) }
    h
  end

  def self._strptime(str, fmt = NO_FMT__)
    fmt_default = fmt.equal?(NO_FMT__)
    __strptime_hash(str, fmt_default ? "%FT%T%z" : fmt, fmt_default)
  end

  # dt_new_by_frags
  def self.__new_by_frags(hash, sg)
    sg = __fix_sg(sg)
    raise Date::Error, "invalid date" if hash.nil?
    jd, civil = __frags_jd(hash, sg)
    if civil
      hash[:hour] = 0 if hash[:hour].nil?
      hash[:min] = 0 if hash[:min].nil?
      if hash[:sec].nil?
        hash[:sec] = 0
      elsif hash[:sec] == 60
        hash[:sec] = 59
      end
    end
    raise Date::Error, "invalid date" if jd.nil?

    h, mi, s = __num2int(hash[:hour]), __num2int(hash[:min]), __num2int(hash[:sec])
    h += 24 if h < 0
    mi += 60 if mi < 0
    s += 60 if s < 0
    if h < 0 || h > 24 || mi < 0 || mi > 59 || s < 0 || s > 59 || (h == 24 && (mi > 0 || s > 0))
      raise Date::Error, "invalid date"
    end
    df = h * 3600 + mi * 60 + s

    sf = hash[:sec_fraction]
    sf = sf.nil? ? 0 : sf * 1_000_000_000
    sf = sf.to_i if sf.is_a?(Rational) && sf.denominator == 1

    of = hash[:offset]
    of = of.nil? ? 0 : __num2int(of)
    if of < -86400 || of > 86400
      warn "invalid offset is ignored" if $VERBOSE
      of = 0
    end
    if df >= 86400 # 24:00:00
      jd += 1
      df -= 86400
    end
    new!(jd, df, sf, of, sg)
  end

  def self.parse(str = "-4712-01-01T00:00:00+00:00", comp = true, sg = ITALY, **opt)
    __new_by_frags(_parse(str, comp, **opt), sg)
  end

  def self.strptime(str = "-4712-01-01T00:00:00+00:00", fmt = "%FT%T%z", sg = ITALY)
    __new_by_frags(Date._strptime(str, fmt), sg)
  end

  def self.iso8601(str = "-4712-01-01T00:00:00+00:00", sg = ITALY, **opt) = __new_by_frags(_iso8601(str, **opt), sg)
  def self.rfc3339(str = "-4712-01-01T00:00:00+00:00", sg = ITALY, **opt) = __new_by_frags(_rfc3339(str, **opt), sg)
  def self.xmlschema(str = "-4712-01-01T00:00:00+00:00", sg = ITALY, **opt) = __new_by_frags(_xmlschema(str, **opt), sg)
  def self.rfc2822(str = "Mon, 1 Jan -4712 00:00:00 +0000", sg = ITALY, **opt) = __new_by_frags(_rfc2822(str, **opt), sg)
  class << self; alias_method :rfc822, :rfc2822; end
  def self.httpdate(str = "Mon, 01 Jan -4712 00:00:00 GMT", sg = ITALY, **opt) = __new_by_frags(_httpdate(str, **opt), sg)
  def self.jisx0301(str = "-4712-01-01T00:00:00+00:00", sg = ITALY, **opt) = __new_by_frags(_jisx0301(str, **opt), sg)
end


class Time
  # Time is proleptic Gregorian, so the conversion goes through the Gregorian
  # julian day; the resulting Date still carries the default (ITALY) reform.
  def to_date = Date.jd(Date.civil_to_jd(year, month, day, Date::GREGORIAN))
  def to_datetime
    DateTime.jd(Date.civil_to_jd(year, month, day, Date::GREGORIAN),
                hour, min, sec + Rational(nsec, 1_000_000_000), Rational(utc_offset, 86400))
  end
  def to_time = self
end
