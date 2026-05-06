import unittest
import statistics


class StatsExtraTest(unittest.TestCase):
    def test_harmonic_mean(self):
        self.assertAlmostEqual(statistics.harmonic_mean([1, 2, 4]), 12.0/7, places=4)

    def test_harmonic_mean_negative(self):
        with self.assertRaises(statistics.StatisticsError):
            statistics.harmonic_mean([1, -1, 2])

    def test_geometric_mean(self):
        self.assertAlmostEqual(statistics.geometric_mean([1, 2, 4]), 2.0, places=4)

    def test_multimode_unique(self):
        self.assertEqual(set(statistics.multimode("aabbcc")), {"a", "b", "c"})

    def test_multimode_one_winner(self):
        self.assertEqual(statistics.multimode([1, 1, 2]), [1])


unittest.main(globals())
