# Hash#default_proc= rejects a lambda whose arity isn't 2 (procs are lenient). vs ruby.
h = {}
begin; h.default_proc = ->(a) {}; rescue => e; p e.class; end
begin; h.default_proc = ->(a, b, c) {}; rescue => e; p e.class; end
h.default_proc = ->(a, b) { a + b }; p h.default_proc.lambda?
h.default_proc = proc { |a| a }; p :ok
h.default_proc = ->(*a) { a }; p :ok_splat
h.default_proc = ->(a, b, *r) { }; p :ok_2plus
h.default_proc = nil; p h.default_proc
