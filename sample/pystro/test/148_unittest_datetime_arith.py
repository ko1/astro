import unittest
import datetime


class DateArithTest(unittest.TestCase):
    def test_add_timedelta(self):
        d = datetime.date(2026, 5, 6)
        d2 = d + datetime.timedelta(days=7)
        self.assertEqual((d2.year, d2.month, d2.day), (2026, 5, 13))

    def test_sub_timedelta(self):
        d = datetime.date(2026, 5, 6)
        d2 = d - datetime.timedelta(days=7)
        self.assertEqual((d2.year, d2.month, d2.day), (2026, 4, 29))

    def test_date_diff(self):
        a = datetime.date(2026, 5, 13)
        b = datetime.date(2026, 5, 6)
        self.assertEqual((a - b).days, 7)

    def test_cross_month(self):
        d = datetime.date(2026, 1, 31) + datetime.timedelta(days=1)
        self.assertEqual((d.year, d.month, d.day), (2026, 2, 1))

    def test_cross_year(self):
        d = datetime.date(2026, 12, 31) + datetime.timedelta(days=1)
        self.assertEqual((d.year, d.month, d.day), (2027, 1, 1))

    def test_leap_feb(self):
        d = datetime.date(2024, 2, 28) + datetime.timedelta(days=1)
        self.assertEqual((d.year, d.month, d.day), (2024, 2, 29))

    def test_iso(self):
        d = datetime.date(2026, 5, 6)
        self.assertEqual(d.isoformat(), "2026-05-06")
        self.assertEqual(str(d), "2026-05-06")

    def test_time_str(self):
        t = datetime.time(14, 30, 0)
        self.assertEqual(str(t), "14:30:00")
        self.assertEqual(t.isoformat(), "14:30:00")

    def test_weekday(self):
        # 2026-05-04 is Monday
        self.assertEqual(datetime.date(2026, 5, 4).weekday(), 0)
        # 2026-05-10 is Sunday
        self.assertEqual(datetime.date(2026, 5, 10).weekday(), 6)


class TimedeltaArithTest(unittest.TestCase):
    def test_mul_int(self):
        td = datetime.timedelta(days=1)
        self.assertEqual((td * 3).days, 3)

    def test_sub(self):
        td1 = datetime.timedelta(days=7)
        td2 = datetime.timedelta(days=3)
        self.assertEqual((td1 - td2).days, 4)


unittest.main(globals())
