# Layer A bootstrap: $LOAD_PATH, at_exit (reverse order), exit-code constants,
# and the process/thread stub constants exist. vs ruby (diff-able parts only).
p $LOAD_PATH.class
p $:.equal?($LOAD_PATH)
$LOAD_PATH << "/tmp/x"
p $LOAD_PATH.last
p defined?(Thread)
p defined?(Process)
p defined?(GC)
p defined?(RbConfig)
p Process.pid.is_a?(Integer)
at_exit { puts "exit_b" }
at_exit { puts "exit_a" }
puts "body"
