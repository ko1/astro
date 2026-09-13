def probe(tag)
  puts "== #{tag} caller"
  caller.each { |l| puts "  #{l.sub(__dir__ + '/', '')}" }
  puts "== #{tag} caller_locations(0)"
  caller_locations(0).each { |l| puts "  #{l.label} | #{l.base_label} | #{l.path.sub(__dir__ + '/', '')} | #{l.lineno}" }
end

probe("#{$tag} top")

def t1_deep
  probe("#{$tag} method")
  raise 'boom'
end

begin
  t1_deep
rescue => e
  puts "== #{$tag} backtrace"
  e.backtrace.each { |l| puts "  #{l.sub(__dir__ + '/', '')}" }
end
