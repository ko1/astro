# String#% / sprintf coerce numeric directives via #to_int (%d/%x/%o/%b) and #to_f
# (%f/%e/%g), like Kernel#Integer/Float. vs ruby.
class TI; def to_int; 42; end; end
class TF; def to_f; 3.5; end; end
p("%d" % TI.new)
p("%5d" % TI.new)
p("%x" % TI.new)
p("%o" % TI.new)
p("%b" % TI.new)
p("%f" % TF.new)
p("%.2f" % TF.new)
p("%d %d" % [TI.new, 7])
class TA; def to_ary; [1, 2]; end; end
p("%d-%d" % TA.new)
