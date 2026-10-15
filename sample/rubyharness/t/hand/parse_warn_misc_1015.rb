# prism's parse warnings, integer flip-flop ends ($.), `if /re/` ($_), inject's ignored block,
# Float#ceil/floor with large negative digits, sysseek after a line read,
# MatchData#string of a chilled literal, frozen immediates, alias in
# instance_eval, Errno constants, top-level @@var under instance_eval (2026-10-15)
require "stringio"
def t(tag)
  r = yield
  puts "#{tag}: #{r.inspect}"
rescue Exception => e
  puts "#{tag}: #{e.class}: #{e.message}"
end
def warns(verbose = true)
  old = $stderr
  $stderr = StringIO.new
  v, $VERBOSE = $VERBOSE, verbose
  yield
  $stderr.string
ensure
  $stderr = old
  $VERBOSE = v
end
t(:dup_when) { warns { eval("case 1\nwhen 2 then 1\nwhen 2 then 2\nend", nil, "w.rb") } }
t(:dup_when_quiet) { warns(false) { eval("case 1\nwhen 2 then 1\nwhen 2 then 2\nend", nil, "w.rb") } }
t(:ff_int_warn) { warns(false) { eval("x = 1\nif (1..2) then end", nil, "f.rb") } }
t(:ff_int) { $. = 0; r = []; 10.times { |i| r << i if (eval "4..5") }; $. = 4; [r, (0..3).select { |i| (i == 2)..6 ? true : false }] }
t(:last_line) { $_ = "hello world"; a = (if eval("/wor/") then $~[0] end); $_ = "zzz"; [a, (eval("/wor/") ? 1 : 2)] }
t(:inject_warn) { warns { [[1, 2].inject(1, :+) { 0 }, (1..3).inject(1, :*) { 0 }, { a: 1 }.inject([], :+) { 0 }] }.lines.size }
t(:inject_range) { $VERBOSE = nil; (1..3).inject(1, :*) { 0 } }
t(:ceil_neg) { [12.0.ceil(-50), -12.0.floor(-50), 5.5.ceil(-1), -15.5.truncate(-1), 1234.5.ceil(-2)] }
t(:sysseek_line) { File.open(__FILE__) { |f| f.readline; f.sysseek(-5, IO::SEEK_CUR) } }
t(:sysseek_read) { File.open(__FILE__) { |f| f.read(3); f.sysseek(0) } }
t(:md_string) { s = /(.)(.)(\d+)(\d)/.match("THX1138.").string; [s, s.frozen?] }
t(:riv_imm) { [nil, :foo, 1].map { |o| (o.remove_instance_variable(:@a) rescue $!.class) } }
t(:alias_int) { 1.instance_eval { alias foo to_s } }
t(:alias_value) { s = +"a"; [s.instance_eval { alias foo to_s }, s.foo] }
t(:errno) { [Errno::NOERROR::Errno, Errno::EWOULDBLOCK, Errno::EDEADLOCK, Errno::EOPNOTSUPP, SystemCallError.new(0).class,
             SystemCallError.new(Errno.constants.size + 1000).class, Errno.constants.include?(:BY_NUM__)] }
class CvEnv; end
t(:cvar_top_ie) { CvEnv.new.instance_eval { @@cv_top_x } }
t(:cvar_top_ie_set) { CvEnv.new.instance_eval { @@cv_top_y = 1 } }
