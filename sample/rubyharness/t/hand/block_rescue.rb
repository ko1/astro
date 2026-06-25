# block body with inline rescue / begin-rescue-ensure (rubyspec follow-up)
p [1, 2, 0, 4].map { |x| 10 / x rescue -1 }
r = []
[1, 2, 3].each { |x| begin; r << x * 2; ensure; r << :done; end }
p r
p [1, 2].map { |x|
  begin
    raise "boom" if x == 2
    x
  rescue => e
    "caught:" + e.message
  end
}
p [4, 0, 2].map { |x| begin; 8 / x; rescue ZeroDivisionError; :zero; end }
