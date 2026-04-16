require 'mkmf'

$srcs = ['abruby.c', 'node.c']

# Add builtin sources
builtin_dir = File.join($srcdir || '.', 'builtin')
Dir.glob(File.join(builtin_dir, '*.c')).each do |f|
  $srcs << f
end

$VPATH << "$(srcdir)/builtin"
$INCFLAGS << " -I$(srcdir)"
$INCFLAGS << " -I$(srcdir)/../../runtime"
$CFLAGS << " -fno-plt"
$LDFLAGS << " -ldl"

create_makefile('abruby')
