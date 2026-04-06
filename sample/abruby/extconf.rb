require 'mkmf'

$srcs = ['abruby.c', 'node.c']

# Add builtin sources
builtin_dir = File.join($srcdir || '.', 'builtin')
Dir.glob(File.join(builtin_dir, '*.c')).each do |f|
  $srcs << f
end

$VPATH << "$(srcdir)/builtin"
$INCFLAGS << " -I$(srcdir)"

if enable_config('debug', false)
  $defs << '-DABRUBY_DEBUG=1'
end

create_makefile('abruby')
