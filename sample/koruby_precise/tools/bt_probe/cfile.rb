# A C method has no source file of its own: CRuby reports it in the file of the
# frame BELOW it (its call site), not in whatever file is being loaded when the
# raise unwinds past it.  Every case here puts the C frame's caller in a file
# other than this one, so a "file being read now" answer shows up as a diff.
# Run under CRuby and koruby and diff.
require_relative 'cfile_lib'

def show(tag)
  yield
rescue => e
  puts "== #{tag}"
  e.backtrace.each { |l| puts "  " + l.sub(__dir__ + "/", "") }
end

# 1: block in another file, under Array#each, unwound all the way out to here.
show("block through C, caught here") { CF.each_raise }
# 2: same, but caught inside the other file — the C frame is then the OUTERMOST
#    unwound frame and has to borrow from the first LIVE frame (in cfile_lib.rb).
puts "== block through C, caught there"
CF.each_raise_rescued.each { |l| puts "  " + l.sub(__dir__ + "/", "") }
# 3: two C frames stacked (each -> map), both in the other file.
show("two C frames") { CF.nested_c }
# 4: a C frame whose caller is this file, with a Ruby frame from the other file
#    further down — the borrow must stop at the first frame below.
show("C frame called from here") { CF.yielder { raise "x" } }
# 5: raise straight out of a C method called in the other file.
show("direct C call") { CF.direct }
# 6: a raise crossing a require of a THIRD file (nested require frames).
show("raise through require") { CF.load_raiser }
