p defined?(FrozenError)
p (FrozenError < RuntimeError)
checks = []
s = "x".freeze
checks << (begin; s << "y"; "no"; rescue => e; e.class.to_s; end)
checks << (begin; s[0] = "z"; "no"; rescue => e; e.class.to_s; end)
a = [1,2].freeze
checks << (begin; a << 3; "no"; rescue => e; e.class.to_s; end)
checks << (begin; a.push(3); "no"; rescue => e; e.class.to_s; end)
checks << (begin; a[0] = 9; "no"; rescue => e; e.class.to_s; end)
h = {a:1}.freeze
checks << (begin; h[:b] = 2; "no"; rescue => e; e.class.to_s; end)
checks << (begin; h.delete(:a); "no"; rescue => e; e.class.to_s; end)
p checks
# non-frozen still works
p ([1].tap { |x| x << 2 })
