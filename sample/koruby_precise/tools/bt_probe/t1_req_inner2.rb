def probe2(tag)
  puts "== #{tag} caller"
  caller.each { |l| puts "  #{l.sub(__dir__ + '/', '')}" }
  puts "== #{tag} caller_locations(0)"
  caller_locations(0).each { |l| puts "  #{l.label} | #{l.base_label} | #{l.path.sub(__dir__ + '/', '')} | #{l.lineno}" }
end

probe2("#{$tag} top")

def t1_deep2
  probe2("#{$tag} method")
  raise 'boom'
end

begin
  t1_deep2
rescue => e
  puts "== #{$tag} backtrace"
  e.backtrace.each { |l| puts "  #{l.sub(__dir__ + '/', '')}" }
end
