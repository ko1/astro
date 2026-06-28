h = Hash.new(42)
p h.default_proc
h.default_proc = ->(hash, k) { "v:#{k}" }
p h.default_proc.class
p h[:x]
p h[:y]
h2 = Hash.new(7)
h2.default_proc = proc { |hash, k| 99 }
p h2[:z]
p (begin; ({}).default_proc = 5; rescue TypeError; "TE"; end)
h3 = {}
h3.default_proc = nil
p h3.default_proc
