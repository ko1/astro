# #pretty_inspect exists only once pp is loaded (CRuby), and then honours a
# user #pretty_print.
o = Object.new; def o.pretty_print(q) = q.text("CUSTOM")
p((o.pretty_inspect rescue $!.class))
p Object.new.respond_to?(:pretty_inspect)
require "pp"
p o.pretty_inspect, [1, {a: 2}].pretty_inspect
pp({x: 1})
