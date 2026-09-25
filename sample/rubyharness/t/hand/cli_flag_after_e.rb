# Command-line switches after `-e CODE` are still switches (`ruby -e code -v`),
# while words after the script are ARGV; Ruby's -v is not the framework's chatter.
require "rbconfig"
run = ->(*args) { IO.popen([RbConfig.ruby, *args], err: [:child, :out], &:read).lines.reject { |l| l.start_with?("ruby ") } }
p run.("-e", "p ARGV, $VERBOSE", "-v")
p run.("-v", "-e", "p $VERBOSE")
p run.("-e", "p ARGV", "--", "-v")
p run.("-e", "p [ARGV, $VERBOSE]", "-w")
