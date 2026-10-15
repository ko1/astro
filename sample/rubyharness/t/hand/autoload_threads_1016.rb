# A constant being autoloaded by one thread stays undefined to the others: they wait for the load,
# and const_source_location still names the autoload registration (2026-10-16)
require "tmpdir"
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
DIR = Dir.mktmpdir
PATH = File.join(DIR, "al_slow.rb")
File.write(PATH, "$al_loads = ($al_loads || 0) + 1\nmodule AlSlow\n  sleep 0.05\n  def self.foo = :foo\nend\n")
t(:wait) do
  Object.autoload :AlSlow, PATH
  ths = 6.times.map { Thread.new { begin; Object.const_get(:AlSlow).foo; rescue NameError, NoMethodError => e; e.class; end } }
  [ths.map(&:value).uniq, $al_loads]
end
PATH2 = File.join(DIR, "al_loc.rb")
File.write(PATH2, "module AlLoc\n  $al_mid.call\nend\n")
t(:source_location) do
  reg_line = __LINE__ + 1
  Object.autoload :AlLoc, PATH2
  seen = []
  q1 = Queue.new; q2 = Queue.new
  $al_mid = -> { seen << [:loading, Object.const_source_location(:AlLoc)&.last]; q1.push(1); q2.pop }
  th = Thread.new { q1.pop; seen << [:other, Object.const_source_location(:AlLoc)&.last]; q2.push(1) }
  Object.const_get(:AlLoc)
  th.join
  seen << [:after, Object.const_source_location(:AlLoc)&.last]
  seen.map { |k, l| [k, l == reg_line ? :registration : l] }
end
