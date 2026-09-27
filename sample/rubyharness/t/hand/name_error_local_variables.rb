# A bare-name miss records the caller's local variables (NameError#local_variables),
# which did_you_mean's VariableNameChecker suggests from.
foo_bar = 1
p((foo_baz rescue $!.local_variables))
def m(alpha, beta = 2) = (gamma = 3; alphx)
p((m(1) rescue $!.local_variables))
[1].each { |item| x = 1; p((itme rescue $!.local_variables)) }
p((foo_baz rescue $!.detailed_message(highlight: false).lines.last))
p((m(1) rescue $!.corrections))
