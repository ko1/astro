# Ruby 4.1 Range#clamp.  Differential vs CRuby 4.1.0dev.
def t(label)
  print label.ljust(40), " => "
  p(yield)
rescue Exception => e
  puts "#{e.class}: #{e.message}"
end

class DuckRange
  def initialize(b, e, x) = (@b, @e, @x = b, e, x)
  def begin = @b
  def end = @e
  def exclude_end? = @x
end

puts "-- min, max"
t("(1..10).clamp(3, 7)") { (1..10).clamp(3, 7) }
t("(1...10).clamp(3, 7)") { (1...10).clamp(3, 7) }
t("(1...10).clamp(3, 10)") { (1...10).clamp(3, 10) }
t("(0...).clamp(0, 10)") { (0...).clamp(0, 10) }
t("(..10).clamp(3, 7)") { (..10).clamp(3, 7) }
t("(...10).clamp(3, 7)") { (...10).clamp(3, 7) }
t("(1..10).clamp(nil, nil)") { (1..10).clamp(nil, nil) }
t("(1..).clamp(nil, 0)") { (1..).clamp(nil, 0) }
t("(1..).clamp(3, nil)") { (1..).clamp(3, nil) }

puts "-- a Range argument"
t("(1..10).clamp(3..7)") { (1..10).clamp(3..7) }
t("(1..10).clamp(3...7)") { (1..10).clamp(3...7) }
t("(1..5).clamp(3...7)") { (1..5).clamp(3...7) }
t("(..10).clamp(3...7)") { (..10).clamp(3...7) }
t("(..5).clamp(3...7)") { (..5).clamp(3...7) }
t("(1..10).clamp(3...10)") { (1..10).clamp(3...10) }
t("(1..10).clamp(..7)") { (1..10).clamp(..7) }
t("(1..10).clamp(...7)") { (1..10).clamp(...7) }
t("(1..5).clamp(...7)") { (1..5).clamp(...7) }
t("(1..10).clamp(3..)") { (1..10).clamp(3..) }
t("(1..10).clamp(3...)") { (1..10).clamp(3...) }
t("(1..).clamp(3..)") { (1..).clamp(3..) }
t("(1...).clamp(3...)") { (1...).clamp(3...) }
t("(1..).clamp(..0)") { (1..).clamp(..0) }
t("(1..10).clamp(nil..nil)") { (1..10).clamp(nil..nil) }

puts "-- self entirely outside the bounds"
t("(1..10).clamp(20, 30)") { (1..10).clamp(20, 30) }
t("(1..10).clamp(-10, 0)") { (1..10).clamp(-10, 0) }
t("(..10).clamp(20, 30)") { (..10).clamp(20, 30) }
t("(1..10).clamp(20..30)") { (1..10).clamp(20..30) }
t("(1..10).clamp(-10..0)") { (1..10).clamp(-10..0) }
t("(..10).clamp(20..30)") { (..10).clamp(20..30) }
t("(1..2).clamp(3, 3)") { (1..2).clamp(3, 3) }
t("(4..10).clamp(3, 3)") { (4..10).clamp(3, 3) }
t("(1..10).clamp(3, 3)") { (1..10).clamp(3, 3) }
t("(1..10).clamp(3..3)") { (1..10).clamp(3..3) }

puts "-- other receivers"
t("('a'..'z').clamp('c', 'f')") { ("a".."z").clamp("c", "f") }
t("(1.0..9.5).clamp(2, 8)") { (1.0..9.5).clamp(2, 8) }
t("duck exclusive") { (1..10).clamp(DuckRange.new(3, 7, true)) }
t("duck inclusive") { (1..10).clamp(DuckRange.new(3, 7, false)) }
t("subclass -> Range") { Class.new(Range).new(1, 10).clamp(3, 7).class }
t("exclude_end? of 3...7") { (1..10).clamp(3...7).exclude_end? }

puts "-- errors"
t("(1..3).clamp(1, 'z')") { (1..3).clamp(1, "z") }
t("(1..3).clamp('a', 'z')") { (1..3).clamp("a", "z") }
t("(1..3).clamp(2, 1)") { (1..3).clamp(2, 1) }
t("(1..3).clamp(2..1)") { (1..3).clamp(2..1) }
t("(1..3).clamp(1)") { (1..3).clamp(1) }
t("(1..3).clamp(nil)") { (1..3).clamp(nil) }
t("(1..3).clamp(Object.new)") { (1..3).clamp(Object.new) }
t("(1..3).clamp()") { (1..3).clamp }
t("(1..3).clamp(1, 2, 3)") { (1..3).clamp(1, 2, 3) }
t("(1..3).clamp(1.0, 'z')") { (1..3).clamp(1.0, "z") }
t("arity") { (1..2).method(:clamp).arity }

puts "-- a small exhaustive table"
t("all (a..b).clamp(3, 6)") do
  (0..8).flat_map { |a| (0..8).map { |b| (a..b).clamp(3, 6).inspect } }
end
t("all (a...b).clamp(3, 6)") do
  (0..8).flat_map { |a| (0..8).map { |b| (a...b).clamp(3, 6).inspect } }
end
t("all (a..b).clamp(3...6)") do
  (0..8).flat_map { |a| (0..8).map { |b| (a..b).clamp(3...6).inspect } }
end
