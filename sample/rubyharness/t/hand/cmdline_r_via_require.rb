# -rLIB goes through Kernel#require (so rubygems can activate a bundled gem such as
# drb for `ruby -rdrb/drb`, rinda's test_take_bug_8215); a missing one is a LoadError.
require 'rbconfig'
run = ->(*a) { IO.popen([RbConfig.ruby, *a], err: [:child, :out], &:read) }
p run.("-rjson", "-rset", "-e", "p [JSON.generate([1]), Set[1].size]")
out = run.("-rnosuchlib9zz", "-e", "p 1")
p out.lines.first[/cannot load such file -- \S+ \(LoadError\)/], $?.exitstatus
p run.("-e", "p defined?(DRb)")
