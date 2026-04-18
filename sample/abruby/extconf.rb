require 'mkmf'

$srcs = ['abruby.c', 'node.c', 'node_helper.c']

# Add builtin sources
builtin_dir = File.join($srcdir || '.', 'builtin')
Dir.glob(File.join(builtin_dir, '*.c')).each do |f|
  $srcs << f
end

$VPATH << "$(srcdir)/builtin"
$INCFLAGS << " -I$(srcdir)"
$INCFLAGS << " -I$(srcdir)/../../runtime"
$CFLAGS << " -fno-plt"
$CFLAGS << " -DABRUBY_PROFILE=1" if ENV['ABRUBY_PROFILE']
$LDFLAGS << " -ldl"

create_makefile('abruby')
