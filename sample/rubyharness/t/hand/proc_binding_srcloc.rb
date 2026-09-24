# `Proc#binding.source_location` is where the BLOCK was written — the same place
# Proc#source_location reports.  koruby built the binding with no source node at
# all, so it answered nil.  bundler filters its own hooks with
# `proc.binding.source_location[0] == __FILE__`.
def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

PR = proc { 1 }
LA = lambda { 2 }
def cap(&b) = b
BM = cap { 3 }

t(:proc_matches)   { PR.binding.source_location == PR.source_location }
t(:lambda_matches) { LA.binding.source_location == LA.source_location }
t(:blockarg)       { BM.binding.source_location == BM.source_location }
t(:file_tail)      { PR.binding.source_location.first.end_with?("proc_binding_srcloc.rb") }
t(:line_kind)      { PR.binding.source_location.last.class }
t(:nested_block)   { p2 = [1].map { |x| proc { x } }.first; p2.binding.source_location == p2.source_location }
t(:proc_in_method) { def mk = proc { :m }; q = mk; q.binding.source_location == q.source_location }

# a Method#to_proc reports the method's own def; a Symbol#to_proc has no Ruby
# body at all and CRuby refuses to make a Binding from it
t(:method_proc)    { m = method(:cap).to_proc; m.binding.source_location == method(:cap).source_location }
t(:symbol_proc)    { :to_s.to_proc.binding }

t(:binding_self)   { PR.binding.receiver.class }
t(:binding_eval)   { x = 41; pr = proc { x }; pr.binding.eval("1 + 1") }
t(:still_callable) { [PR.call, LA.call, BM.call] }
