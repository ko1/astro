# C-extension build for astrogre.
#
# Wraps the in-house regex engine so Ruby code (especially the test
# driver in `tests/run.rb`) can talk to it directly without going through
# the grep CLI.  The same compiled engine sources from `sample/astrogre/`
# are pulled into the .so via $VPATH and $srcs.
#
# Build: ruby extconf.rb && make
# Test:  ruby -I . -e 'require "astrogre_ext"; puts ASTrogre.compile(/\d+/).match?("a1b")'

require "mkmf"

ROOT    = File.expand_path("..", __dir__)        # sample/astrogre
RUNTIME = File.expand_path("../../runtime", ROOT) # ../../runtime

unless File.exist?(File.join(ROOT, "node_eval.c"))
  abort "astrogre/node_eval.c missing — run `make` in #{ROOT} first to generate AST glue"
end

$INCFLAGS << " -I#{ROOT} -I#{RUNTIME}"
$CFLAGS << " -mavx2 -O2"
$CFLAGS << " -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter -Wno-unused-but-set-variable"

# Bake the astrogre source dir so the Cext can point ASTRO_CS_SRC_DIR at it.
# Without this, INIT() reads /proc/self/exe which inside Ruby resolves to
# the rbenv binary path — astro_cs_compile then emits SD source files
# that #include "<rbenv-bin-dir>/node.h" and the build fails.
$CFLAGS << " -DASTROGRE_SRC_DIR=\\\"#{ROOT}\\\""

# The engine sources we want compiled into this .so live in the parent
# dir; tell mkmf to find them via VPATH and list them in $srcs alongside
# our wrapper.  mkmf compiles each .c into the corresponding .o.
$VPATH << ROOT
$srcs = ["astrogre_ext.c", "astrogre_dump_helper.c",
         "node.c", "parse.c", "match.c", "aho_corasick.c"]

$LIBS << " -ldl"

create_makefile("astrogre_ext")
