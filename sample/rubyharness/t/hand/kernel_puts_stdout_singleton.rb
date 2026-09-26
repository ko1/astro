# Kernel#puts on the default $stdout still goes through a singleton STDOUT.puts
# (CRuby's rb_f_puts sends $stdout.puts; pry's cli_spec mocks it); `super` from
# that singleton writes.
def STDOUT.puts(*a) = (write("hooked #{a.inspect}\n"); super)
puts "x"
puts
print "y\n"
p 1
$stdout.puts "direct"
