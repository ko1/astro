# NoMethodError names the receiver's class through its #name, which can run Ruby and
# move the class under the moving GC: the fallback re-reads it (was a stale-pointer
# SEGV under GC stress).  An empty #name prints empty, as in CRuby.
class E < StandardError
  def self.name
    Object.new
    nil
  end
end
begin; E.new.zzz; rescue NoMethodError => e; p e.message; end
begin; E.zzz; rescue NoMethodError => e; p e.message.sub(/0x\h+/, "X"); end
class F; def self.name = ""; end
begin; F.new.zzz; rescue NoMethodError => e; p e.message; end
class G; def self.name = (GC.start; Array.new(100) { "x" }; "Gee"); end
begin; G.new.zzz; rescue NoMethodError => e; p e.message; end
begin; E.new("m").zzz; rescue NoMethodError => e; p e.message; end
