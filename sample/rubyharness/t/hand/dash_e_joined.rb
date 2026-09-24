# CRuby accepts `-e` both detached (`-e code`) and JOINED (`-ecode`), and
# concatenates repeats with a newline.  koruby only took the detached form and
# rejected a second -e outright, so a gem shelling out with `-e'...'` got
# "unknown flag".  pry's bundler/hanami integration specs build their child
# command exactly that way.
require "rbconfig"

def run(*args)
  r, w = IO.pipe
  pid = Process.spawn(RbConfig.ruby, *args, out: w, err: w)
  w.close
  Process.wait(pid)
  st = $?.exitstatus
  out = r.read
  r.close
  # the child inherits the harness gemhome (koruby warns about unbuilt C
  # extensions) and any GC-stress banner, neither of which is what is under test
  noise = ["Ignoring", "missing extensions", "[aro_gc="]
  [st, out.lines.reject { |l| noise.any? { |n| l.include?(n) } }.join]
end

def t(l)
  p [l, yield]
rescue Exception => e
  p [l, e.class, e.message.to_s[0, 60]]
end

t(:joined)     { run("-ep 1 + 1") }
t(:detached)   { run("-e", "p :x") }
t(:multi)      { run("-ea = 1", "-ep a + 1") }
t(:multi_mix)  { run("-e", "b = 2", "-ep b * 3") }
t(:with_lib)   { run("-Ilib", "-ep 42") }
t(:argv)       { run("-ep ARGV", "x", "y") }
t(:argv_dash)  { run("-ep ARGV", "--", "-z") }
t(:exit_code)  { run("-eexit 7").first }
t(:stderr)     { run("-ewarn 'to stderr'") }
t(:raise)      { run("-eraise 'boom'").first }
t(:three)      { run("-ea = 1", "-eb = 2", "-ep a + b") }
t(:newline_in) { run("-ep 1\np 2") }
