# Small Kernel / core differences fixed together (2026-10-15)
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
class AttachedPlain; end
class SymprocTarget
  def method_missing(n, *) = [:mm, n]
  def respond_to_missing?(*) = true
  private def pv = 1
  protected def pr = 2
end
class SymprocPlain
  private def pv = 1
  protected def pr = 2
end
t(:yield_self) { e = Object.new.yield_self; [e.class, e.size] }
t(:then) { e = 1.then; [e.class, e.size, e.each { |x| x + 1 }] }
t(:throw3) { catch(:a) { throw :a, 1, 2 } }
t(:loop_result) { e = [1].each; loop { e.next } }
t(:loop_result_hash) { e = { a: 1 }.each; loop { e.next } }
t(:attached) { AttachedPlain.attached_object }
t(:attached_nil) { nil.singleton_class.attached_object }
t(:dup_root) { BasicObject.dup }
t(:clone_root) { BasicObject.clone }
t(:cv_dump) { Thread::ConditionVariable.new.marshal_dump }
t(:symproc_private) { [SymprocPlain.new].map(&:pv) }
t(:symproc_protected) { :pr.to_proc.call(SymprocPlain.new) }
t(:symproc_var) { m = :pv; [SymprocPlain.new].map(&m) }
t(:symproc_mm) { [SymprocTarget.new].map(&:pv) }
t(:symproc_public) { [1, 2].map(&:to_s) }
t(:to_r_int) { o = BasicObject.new; def o.to_r = 1; Rational(o) }
t(:to_int_ok) { o = Object.new; def o.to_int = 3; Rational(o) }
t(:sized_new) { SizedQueue.new(Object.new) }
t(:sized_nil) { SizedQueue.new(nil) }
t(:sized_max) { q = SizedQueue.new(2); q.max = Object.new }
t(:sized_float) { SizedQueue.new(2.7).max }
t(:argv0) { Process.argv0.frozen? }
t(:md_named) { /(?<first>\w+) (?<last>\w+)/.match("abc def ghi") }
t(:md_dup_names) { /(?<x>a)|(?<x>b)/.match("b").inspect }
t(:aliases) { [Process.method(:waitpid) == Process.method(:wait), Process.method(:waitpid2) == Process.method(:wait2),
               FileTest.method(:zero?) == FileTest.method(:empty?),
               MatchData.instance_method(:deconstruct) == MatchData.instance_method(:captures)] }
t(:timeout_error) { Regexp::TimeoutError.ancestors.take(3) }
