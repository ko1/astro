# NoMethodError#private_call? is true only when the call that missed had NO
# explicit receiver -- did_you_mean offers private methods and argument-taking
# reserved words (`yield`, `super`, ...) only then.  And a NameError's receiver
# is NOT marshalled: CRuby keeps it in a hidden ivar, so a round trip loses it
# and an otherwise-undumpable receiver (a File) never blocks the dump
# (rspec-support's `in_sub_process` sends exceptions back over a pipe).
class U
  private def secret(a) = a
  def call_missing = secrat(1)
end
begin
  U.new.call_missing
rescue NoMethodError => e
  p [e.class, e.name, e.receiver.class, e.private_call?]
end
begin
  yeild(1)
rescue NoMethodError => e
  p [e.name, e.private_call?]
end
begin
  "x".nosuch
rescue NoMethodError => e
  p [e.name, e.receiver, e.private_call?]
end
class V; private def hidden(x) = x; end
begin
  V.new.hidden(1)
rescue NoMethodError => e
  p [e.name, e.private_call?]
end
# (known gap: CRuby also reports true for an explicit `self.` receiver; koruby
# only knows the call shape on the receiverless path.)
p NoMethodError.new("m", :nm).private_call?
p NoMethodError.new("m", :nm, [1], true).private_call?

f = File.open("/dev/null")
begin; f.sizee(1, 2); rescue NoMethodError => e; end
f.close
r = Marshal.load(Marshal.dump(e))
p [r.class, r.message, r.name, r.args]
p (r.receiver rescue [$!.class, $!.message])
p r.private_call?

begin; "s".nosuch; rescue NoMethodError => e2; end
r2 = Marshal.load(Marshal.dump(e2))
p [r2.message, r2.name, (r2.receiver rescue $!.class)]
begin; nosuchlocal; rescue NameError => e3; end
r3 = Marshal.load(Marshal.dump(e3))
p [r3.class, r3.message, r3.name, (r3.receiver rescue $!.class)]
