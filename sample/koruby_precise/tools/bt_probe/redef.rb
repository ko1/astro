ic = File.expand_path('redef', __dir__)

def show(tag)
  v = yield
  puts "#{tag}: bt=#{v[0]}:#{v[1]} srcloc=#{v[2][0]}:#{v[2][1]}"
end

# --- load the same name twice, calling (i.e. taking a backtrace) in between ---
load "#{ic}/d1.rb"
show('load1/call')  { rm }
load "#{ic}/d2.rb"
show('load2/call')  { rm }

# --- load twice with NO backtrace taken in between ---
load "#{ic}/d1.rb"
load "#{ic}/d2.rb"
show('load2/nocall') { rm }

# --- the SAME file loaded twice ---
load "#{ic}/d2.rb"
show('sameload/a') { rm }
load "#{ic}/d2.rb"
show('sameload/b') { rm }

# --- require then require_relative of a different file defining the same name ---
require "#{ic}/r1.rb"
show('require1') { rr }
require_relative 'redef/r2.rb'
show('require2') { rr }

# --- redefinition inside a class body, via load ---
load "#{ic}/c1.rb"
show('cls1') { RC.new.cm }
load "#{ic}/c2.rb"
show('cls2') { RC.new.cm }

# --- eval redefines a loaded method ---
load "#{ic}/d1.rb"
show('preeval') { rm }
eval <<~'RB', TOPLEVEL_BINDING, 'evaled.rb', 10
  def rm
    l = caller_locations(0)[0]
    [File.basename(l.path), l.lineno, method(:rm).source_location.then { |p, n| [File.basename(p), n] }]
  end
RB
show('posteval') { rm }

# --- class_eval redefines a class method ---
load "#{ic}/c1.rb"
show('precleval') { RC.new.cm }
RC.class_eval <<~'RB', 'cevaled.rb', 20
  def cm
    l = caller_locations(0)[0]
    [File.basename(l.path), l.lineno, method(:cm).source_location.then { |p, n| [File.basename(p), n] }]
  end
RB
show('postcleval') { RC.new.cm }

# --- define_method replacing a `def`-defined method ---
load "#{ic}/d1.rb"
show('predm') { rm }
load "#{ic}/d2.rb"   # a second def, then a define_method over it
Object.send(:define_method, :rm) do
  l = caller_locations(0)[0]
  [File.basename(l.path), l.lineno, method(:rm).source_location.then { |p, n| [File.basename(p), n] }]
end
show('postdm') { rm }
