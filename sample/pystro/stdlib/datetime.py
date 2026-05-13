# pystro stdlib `datetime` (minimal but functional).
import time as _time

# Days per month, leap-aware.
_MONTH_DAYS = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]

def _is_leap(y):
    return (y % 4 == 0 and y % 100 != 0) or y % 400 == 0

def _month_days(y, m):
    if m == 2 and _is_leap(y):
        return 29
    return _MONTH_DAYS[m - 1]


class timedelta:
    def __init__(self, days=0, seconds=0, microseconds=0, milliseconds=0,
                 minutes=0, hours=0, weeks=0):
        total_us = (
            int(microseconds) +
            int(milliseconds) * 1000 +
            int(seconds) * 1_000_000 +
            int(minutes) * 60_000_000 +
            int(hours) * 3_600_000_000 +
            int(days) * 86_400_000_000 +
            int(weeks) * 604_800_000_000
        )
        # Normalise to days/seconds/microseconds.
        us_per_day = 86_400_000_000
        d = total_us // us_per_day
        rem = total_us - d * us_per_day
        s = rem // 1_000_000
        u = rem - s * 1_000_000
        # Python normalises so seconds in [0, 86400) and us in [0, 1_000_000).
        if u < 0:
            u += 1_000_000
            s -= 1
        if s < 0:
            s += 86400
            d -= 1
        self.days = d
        self.seconds = s
        self.microseconds = u

    def total_seconds(self):
        return (self.days * 86400.0 + self.seconds +
                self.microseconds / 1_000_000.0)

    def __add__(self, other):
        if not isinstance(other, timedelta): return NotImplemented
        return timedelta(days=self.days + other.days,
                         seconds=self.seconds + other.seconds,
                         microseconds=self.microseconds + other.microseconds)
    def __sub__(self, other):
        if not isinstance(other, timedelta): return NotImplemented
        return timedelta(days=self.days - other.days,
                         seconds=self.seconds - other.seconds,
                         microseconds=self.microseconds - other.microseconds)
    def __neg__(self):
        return timedelta(days=-self.days, seconds=-self.seconds,
                         microseconds=-self.microseconds)
    def __mul__(self, n):
        if not isinstance(n, int):
            if isinstance(n, float):
                total_us = (self.days * 86_400_000_000 +
                            self.seconds * 1_000_000 +
                            self.microseconds)
                scaled = int(total_us * n)
                return timedelta(microseconds=scaled)
            return NotImplemented
        return timedelta(days=self.days * n, seconds=self.seconds * n,
                         microseconds=self.microseconds * n)
    def __rmul__(self, n):
        return self.__mul__(n)
    def __floordiv__(self, n):
        if isinstance(n, int):
            total_us = (self.days * 86_400_000_000 +
                        self.seconds * 1_000_000 + self.microseconds)
            return timedelta(microseconds=total_us // n)
        if isinstance(n, timedelta):
            a = (self.days * 86_400_000_000 +
                 self.seconds * 1_000_000 + self.microseconds)
            b = (n.days * 86_400_000_000 +
                 n.seconds * 1_000_000 + n.microseconds)
            return a // b
        return NotImplemented
    def __truediv__(self, n):
        if isinstance(n, (int, float)):
            total_us = (self.days * 86_400_000_000 +
                        self.seconds * 1_000_000 + self.microseconds)
            return timedelta(microseconds=int(total_us / n))
        if isinstance(n, timedelta):
            a = (self.days * 86_400_000_000 +
                 self.seconds * 1_000_000 + self.microseconds)
            b = (n.days * 86_400_000_000 +
                 n.seconds * 1_000_000 + n.microseconds)
            return a / b
        return NotImplemented
    def __abs__(self):
        if self.days < 0: return -self
        return self
    def __bool__(self):
        return self.days != 0 or self.seconds != 0 or self.microseconds != 0
    def __hash__(self):
        return self.days * 1000000 + self.seconds + self.microseconds
    def __eq__(self, other):
        if not isinstance(other, timedelta): return False
        return (self.days == other.days and self.seconds == other.seconds
                and self.microseconds == other.microseconds)
    def __lt__(self, other):
        if not isinstance(other, timedelta): return NotImplemented
        return self.total_seconds() < other.total_seconds()
    def __le__(self, other):
        return self < other or self == other
    def __gt__(self, other): return not self <= other
    def __ge__(self, other): return not self < other
    def __repr__(self):
        return ("datetime.timedelta(days=" + str(self.days) +
                ", seconds=" + str(self.seconds) +
                ", microseconds=" + str(self.microseconds) + ")")
    def __str__(self):
        h = self.seconds // 3600
        m = (self.seconds - h * 3600) // 60
        s = self.seconds - h * 3600 - m * 60
        prefix = ""
        if self.days != 0:
            prefix = str(self.days) + " day" + ("s, " if abs(self.days) != 1 else ", ")
        return prefix + (str(h) if h >= 10 else "0" + str(h)) + ":" + \
               (str(m) if m >= 10 else "0" + str(m)) + ":" + \
               (str(s) if s >= 10 else "0" + str(s))


class date:
    def __init__(self, year, month, day):
        self.year = year
        self.month = month
        self.day = day
    @classmethod
    def today(cls):
        t = _time.localtime()
        return cls(t.tm_year, t.tm_mon, t.tm_mday)
    @classmethod
    def fromtimestamp(cls, ts):
        t = _time.localtime(ts)
        return cls(t.tm_year, t.tm_mon, t.tm_mday)
    def isoformat(self):
        y = str(self.year)
        m = str(self.month) if self.month >= 10 else "0" + str(self.month)
        d = str(self.day) if self.day >= 10 else "0" + str(self.day)
        return y + "-" + m + "-" + d
    def __repr__(self):
        return "datetime.date(" + str(self.year) + ", " + str(self.month) + ", " + str(self.day) + ")"
    def __str__(self):
        return self.isoformat()
    def __eq__(self, other):
        if not isinstance(other, date): return False
        return self.year == other.year and self.month == other.month and self.day == other.day
    def __lt__(self, other):
        if not isinstance(other, date): return NotImplemented
        return (self.year, self.month, self.day) < (other.year, other.month, other.day)
    def weekday(self):
        # Zeller's-ish: Jan 1 1970 was a Thursday.
        days = self._ord_days() - date(1970, 1, 1)._ord_days()
        return (days + 3) % 7
    def _ord_days(self):
        # Day count since year 0001-01-01 (rough).
        d = (self.year - 1) * 365 + (self.year - 1) // 4 - (self.year - 1) // 100 + (self.year - 1) // 400
        for m in range(1, self.month):
            d += _month_days(self.year, m)
        d += self.day
        return d
    @staticmethod
    def _from_ordinal(o):
        # Approximate inverse of _ord_days.
        y = 1
        while True:
            yd = 366 if _is_leap(y) else 365
            if o <= yd: break
            o -= yd; y += 1
        m = 1
        while m <= 12:
            md = _month_days(y, m)
            if o <= md: break
            o -= md; m += 1
        return date(y, m, o)
    def __add__(self, other):
        if isinstance(other, timedelta):
            o = self._ord_days() + other.days
            return self._from_ordinal(o)
        return NotImplemented
    def __sub__(self, other):
        if isinstance(other, timedelta):
            return self.__add__(timedelta(-other.days))
        if isinstance(other, date):
            return timedelta(days=self._ord_days() - other._ord_days())
        return NotImplemented
    def __hash__(self):
        return self.year * 10000 + self.month * 100 + self.day
    def strftime(self, fmt):
        return _strftime(fmt, self.year, self.month, self.day, 0, 0, 0, 0)


_MONTH_NAMES = ["January", "February", "March", "April", "May", "June",
                "July", "August", "September", "October", "November", "December"]
_DAY_NAMES = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
              "Saturday", "Sunday"]

def _pad(n, w):
    s = str(n)
    return "0" * (w - len(s)) + s if len(s) < w else s

def _strftime(fmt, y, m, d, h, mi, s, us):
    # Compute weekday: 0=Mon..6=Sun (matches CPython date.weekday()).
    days = date(y, m, d)._ord_days() - date(1970, 1, 1)._ord_days()
    wd = (days + 3) % 7
    # Day of year.
    doy = 0
    for mm in range(1, m): doy += _month_days(y, mm)
    doy += d
    out = []
    i = 0
    n = len(fmt)
    while i < n:
        c = fmt[i]
        if c != "%" or i + 1 >= n:
            out.append(c); i += 1; continue
        spec = fmt[i + 1]
        i += 2
        if spec == "Y": out.append(_pad(y, 4))
        elif spec == "y": out.append(_pad(y % 100, 2))
        elif spec == "m": out.append(_pad(m, 2))
        elif spec == "d": out.append(_pad(d, 2))
        elif spec == "H": out.append(_pad(h, 2))
        elif spec == "M": out.append(_pad(mi, 2))
        elif spec == "S": out.append(_pad(s, 2))
        elif spec == "I":
            hh = h % 12
            if hh == 0: hh = 12
            out.append(_pad(hh, 2))
        elif spec == "p": out.append("AM" if h < 12 else "PM")
        elif spec == "B": out.append(_MONTH_NAMES[m - 1])
        elif spec == "b": out.append(_MONTH_NAMES[m - 1][:3])
        elif spec == "A": out.append(_DAY_NAMES[wd])
        elif spec == "a": out.append(_DAY_NAMES[wd][:3])
        elif spec == "j": out.append(_pad(doy, 3))
        elif spec == "w": out.append(str((wd + 1) % 7))
        elif spec == "f": out.append(_pad(us, 6))
        elif spec == "%": out.append("%")
        elif spec == "Z": out.append("")
        elif spec == "z": out.append("")
        else:
            out.append("%"); out.append(spec)
    return "".join(out)


class datetime(date):
    def __init__(self, year, month, day, hour=0, minute=0, second=0, microsecond=0):
        date.__init__(self, year, month, day)
        self.hour = hour
        self.minute = minute
        self.second = second
        self.microsecond = microsecond
    @classmethod
    def now(cls, tz=None):
        t = _time.time()
        s = int(t)
        us = int((t - s) * 1_000_000)
        lt = _time.localtime(s)
        return cls(lt.tm_year, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, us)
    @classmethod
    def fromtimestamp(cls, ts, tz=None):
        s = int(ts)
        us = int((ts - s) * 1_000_000)
        lt = _time.localtime(s)
        return cls(lt.tm_year, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, us)
    @classmethod
    def utcnow(cls):
        t = _time.time()
        s = int(t)
        us = int((t - s) * 1_000_000)
        lt = _time.gmtime(s)
        return cls(lt.tm_year, lt.tm_mon, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec, us)
    def isoformat(self, sep="T"):
        return date.isoformat(self) + sep + self._time_iso()
    def _time_iso(self):
        h = str(self.hour) if self.hour >= 10 else "0" + str(self.hour)
        m = str(self.minute) if self.minute >= 10 else "0" + str(self.minute)
        s = str(self.second) if self.second >= 10 else "0" + str(self.second)
        if self.microsecond:
            us = str(self.microsecond)
            us = "0" * (6 - len(us)) + us
            return h + ":" + m + ":" + s + "." + us
        return h + ":" + m + ":" + s
    def date(self):
        return date(self.year, self.month, self.day)
    def __repr__(self):
        return ("datetime.datetime(" + str(self.year) + ", " + str(self.month) + ", " + str(self.day)
                + ", " + str(self.hour) + ", " + str(self.minute) + ", " + str(self.second) + ")")
    def __str__(self):
        return self.isoformat(" ")
    def strftime(self, fmt):
        return _strftime(fmt, self.year, self.month, self.day,
                         self.hour, self.minute, self.second, self.microsecond)


class time:
    def __init__(self, hour=0, minute=0, second=0, microsecond=0):
        self.hour = hour
        self.minute = minute
        self.second = second
        self.microsecond = microsecond
    def isoformat(self):
        h = str(self.hour) if self.hour >= 10 else "0" + str(self.hour)
        m = str(self.minute) if self.minute >= 10 else "0" + str(self.minute)
        s = str(self.second) if self.second >= 10 else "0" + str(self.second)
        return h + ":" + m + ":" + s
    def __repr__(self):
        return "datetime.time(" + str(self.hour) + ", " + str(self.minute) + ", " + str(self.second) + ")"
    def __str__(self):
        return self.isoformat()


# Singleton timezone (UTC only — pystro doesn't have a tz database).
# tzinfo abstract base — concrete classes (timezone) inherit from this.
# Many libraries (tomllib, dateutil) gate on `isinstance(x, tzinfo)`.
class tzinfo:
    def utcoffset(self, dt): raise NotImplementedError
    def tzname(self, dt): raise NotImplementedError
    def dst(self, dt): return None
    def fromutc(self, dt): return dt + self.utcoffset(dt)

class timezone(tzinfo):
    def __init__(self, offset, name=None):
        self.offset = offset
        self.name = name
    def utcoffset(self, dt): return self.offset
    def tzname(self, dt): return self.name or "UTC"
    def dst(self, dt): return None
    def __repr__(self):
        return f"datetime.timezone({self.offset!r})"

timezone.utc = timezone(timedelta(0), "UTC")

MINYEAR = 1
MAXYEAR = 9999
