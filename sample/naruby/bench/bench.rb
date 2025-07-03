
require 'benchmark'

BITEM = ARGV.shift || raise

OPT = {
  ruby: false,
  gcc: true,
}

Benchmark.bm{|x|
  x.report("ruby"){
    system("ruby bench/#{BITEM}.na.rb") || raise
  } if OPT[:ruby]
  x.report("ruby/yjit"){
    system("ruby --yjit bench/#{BITEM}.na.rb") || raise
  } if OPT[:ruby]
  x.report("naruby/interpret"){
    system("./naruby -i -q bench/#{BITEM}.na.rb") || raise
  }
  x.report("naruby/static"){
    system("./static_naruby -s -q -b bench/#{BITEM}.na.rb") || raise
  }
  x.report("naruby/compiled"){
    system("./compiled_naruby -q -b bench/#{BITEM}.na.rb") || raise
  }
  x.report("naruby/pg"){
    system("./pg_naruby -p -q -b bench/#{BITEM}.na.rb") || raise
  }
  x.report("gcc/-O0"){
    system("./b0") || raise
  } if OPT[:gcc]
  x.report("gcc/-O1"){
    system("./b1") || raise
  } if OPT[:gcc]
  x.report("gcc/-O2"){
    system("./b2") || raise
  } if OPT[:gcc]
  x.report("gcc/-O3"){
    system("./b3") || raise
  } if OPT[:gcc]
}

puts
puts RUBY_DESCRIPTION
