require 'mkmf'

$srcs = ['arjsv.c', 'node.c']

$INCFLAGS << " -I$(srcdir)"
$INCFLAGS << " -I$(srcdir)/../../runtime"
$CFLAGS << " -fno-plt"
$LDFLAGS << " -ldl"

create_makefile('arjsv')
