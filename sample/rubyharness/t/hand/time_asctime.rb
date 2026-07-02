# Time#asctime / #ctime → fixed "Www Mmm dd hh:mm:ss yyyy". vs ruby.
puts Time.utc(2000, 1, 2, 3, 4, 5).asctime
puts Time.utc(2000, 1, 2, 3, 4, 5).ctime
puts Time.utc(1999, 12, 31, 23, 59, 59).asctime
puts Time.utc(2026, 7, 2, 0, 0, 0).asctime
p Time.utc(2000, 1, 1).asctime.class
