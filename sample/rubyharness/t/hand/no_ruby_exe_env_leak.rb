# The interpreter must not plant variables in its own environment: a child
# started with a cleared environment sees only what it was given.
require "rbconfig"
out = IO.popen([RbConfig.ruby, "-e", "p ENV.keys.sort"], unsetenv_others: true, &:read)
p out
p File.executable?(RbConfig.ruby)
