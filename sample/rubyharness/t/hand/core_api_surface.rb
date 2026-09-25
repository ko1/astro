# Reflection has to agree with CRuby: internal machinery must not be public, and
# a class method must not also answer on an instance.
require "set"

# Marshal exposes dump / load / restore, nothing else.
p Marshal.singleton_methods(false).sort
p Marshal.respond_to?(:_dump_val), Marshal.respond_to?(:_long), Marshal.respond_to?(:__resym)
p Marshal.load(Marshal.dump({ "a" => [1, :b, 2.5, nil, true] }))

# Thread's class methods are singleton-only.
p Thread.instance_methods(false).grep(/\A(current|list|main|pass|stop)\z/)
t = Thread.new { 1 }
t.join
p(begin; t.current; rescue NoMethodError => e; e.class; end)
p Thread.current.is_a?(Thread), Thread.list.is_a?(Array), Thread.main.is_a?(Thread)
p Integer.instance_methods(false).include?(:sqrt), Integer.sqrt(17)
p Symbol.instance_methods(false).include?(:all_symbols), Symbol.all_symbols.is_a?(Array)
p Random.instance_methods(false).grep(/\A(new_seed|urandom)\z/)
p Random.new_seed.is_a?(Integer), Random.urandom(4).bytesize

# Fiber's class methods are real methods, so respond_to? sees them.
p Fiber.respond_to?(:current), Fiber.respond_to?(:yield)
p Fiber.respond_to?(:[]), Fiber.respond_to?(:[]=), Fiber.respond_to?(:blocking?)
f = Fiber.new { Fiber.yield 1; 2 }
p f.resume, f.resume
p Fiber.current.is_a?(Fiber), Fiber.blocking?
Fiber.new { Fiber[:k] = 7; p Fiber[:k] }.resume

# Regexp#timeout / #~
p Regexp.new("a", timeout: 2.0).timeout, /a/.timeout
$_ = "hello"
p(~/ell/)
$_ = "nope"
p(~/ell/)
p Regexp.instance_methods.include?(:timeout), Regexp.instance_methods.include?(:~)
begin; Regexp.new("a", bogus: 1); rescue ArgumentError => e; p [e.class, e.message]; end
