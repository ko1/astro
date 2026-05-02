require 'benchmark'

BITEM = ARGV.shift || raise

# Each row controlled by an env var so a long bench can opt out of
# slow rows (e.g. plain ruby on ackermann).  Default: everything on.
#   NORUBY=1     skip ruby / ruby/yjit
#   NOYJIT=1     skip ruby/yjit only
#   NOGCC=1      skip gcc -O0..-O3 rows
#   SPINEL_DIR=… enable the spinel row (Matz's Ruby AOT) — point to a
#                checkout of https://github.com/matz/spinel that has
#                been `make`d (so spinel_parse and spinel_codegen exist
#                inside).  Untimed compile of the bench file happens
#                once before the timed run.
OPT = {
  ruby:   ENV['NORUBY'] != '1',
  yjit:   ENV['NORUBY'] != '1' && ENV['NOYJIT'] != '1',
  gcc:    ENV['NOGCC']  != '1',
  spinel: !!ENV['SPINEL_DIR'],
}

if OPT[:spinel]
  spinel_bin = "./b_spinel"
  spinel_cmd = "#{ENV['SPINEL_DIR']}/spinel bench/#{BITEM}.na.rb -O 3 -o #{spinel_bin}"
  system(spinel_cmd) || raise("spinel compile failed: #{spinel_cmd}")
end

# Code-store warm-up.  Bakes SD_<hash> for whatever AST the given mode
# produces, untimed.  After this any subsequent `./naruby -b <file>`
# (with the matching mode flag) dlopen-loads those SDs.
#
# `mode` selects the bake variant:
#   nil  — `-c`  : compile-only, no profile-aware nodes (`node_call`).
#   '-p' — `-p`  : run + profile-aware AST (`node_call2`); the run is
#                  needed for any future PGC layer to gather data.
def warm_aot(mode = nil)
  system("rm -rf code_store") || raise
  cmd = mode ? "./naruby -q #{mode} bench/#{BITEM}.na.rb"
             : "./naruby -q -c bench/#{BITEM}.na.rb"
  system(cmd) || raise
end

Benchmark.bm(20) {|x|
  # Bigger stack so deeply-recursive benches (ackermann) don't blow up
  # MRI's default thread-VM stack.  32 MiB is plenty for ack(3, 11)
  # ~16k frames; harmless on the iterative benches.
  ruby_env = "RUBY_THREAD_VM_STACK_SIZE=33554432"
  # Bench scripts now print their result with `p` so the work is
  # observable to optimizers (no DCE asymmetry).  Redirect stdout to
  # /dev/null inside the timed block so the bench table itself stays
  # tidy.
  null = ">/dev/null"
  x.report("ruby") {
    system("#{ruby_env} ruby bench/#{BITEM}.na.rb #{null}") || raise
  } if OPT[:ruby]
  x.report("ruby/yjit") {
    system("#{ruby_env} ruby --yjit bench/#{BITEM}.na.rb #{null}") || raise
  } if OPT[:yjit]
  x.report("spinel") {
    system("./b_spinel #{null}") || raise
  } if OPT[:spinel]

  # Plain mode: no AOT load, no AOT bake.
  x.report("naruby/plain") {
    system("./naruby -i -q bench/#{BITEM}.na.rb #{null}") || raise
  }

  # AOT cached: build code_store/all.so once cold, then time the warm run.
  warm_aot
  x.report("naruby/aot") {
    system("./naruby -b -q bench/#{BITEM}.na.rb #{null}") || raise
  }

  # Profile-guided cached.  `-p` makes the parser emit `node_call2`
  # with `sp_body` linked at parse time (callsite_resolve walks pending
  # forward references once each `def` finishes).  HASH excludes
  # `sp_body` so the live-AST hash matches the bake-time hash, and the
  # SD's call-site emission uses indirect dispatch through
  # `sp_body->head.dispatcher` so method redefinition stays correct.
  warm_aot("-p")
  x.report("naruby/pg") {
    system("./naruby -p -b -q bench/#{BITEM}.na.rb #{null}") || raise
  }

  x.report("gcc/-O0") { system("./b0 #{null}") || raise } if OPT[:gcc]
  x.report("gcc/-O1") { system("./b1 #{null}") || raise } if OPT[:gcc]
  x.report("gcc/-O2") { system("./b2 #{null}") || raise } if OPT[:gcc]
  x.report("gcc/-O3") { system("./b3 #{null}") || raise } if OPT[:gcc]
}

puts
puts RUBY_DESCRIPTION
