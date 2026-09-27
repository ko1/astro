# The top-level error printer asks #detailed_message (after at_exit), as CRuby does:
# user overrides and did_you_mean decorate it, nil / a raise fall back to the class
# name, and only SystemStackError has its middle elided.
cases = [
  'class NameError; def detailed_message(**) = "CUSTOM " + super; end; foo_baz',
  '[].frist',
  'class E < StandardError; def detailed_message(**) = nil; end; raise E, "x"',
  'class F < StandardError; def detailed_message(**) = raise("boom"); end; def q = raise(F, "y"); q',
  'def r(n) = n == 0 ? raise("x") : r(n - 1); r(25)',
  'at_exit { $stderr.puts "at_exit #{$!.class}" }; raise "multi\nline"',
]
cases.each do |src|
  out = IO.popen([RbConfig.ruby, "-e", src, err: [:child, :out]], &:read)
  puts out.gsub(/0x\h+/, "X")
end
