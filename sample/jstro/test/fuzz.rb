#!/usr/bin/env ruby
# Quick differential fuzzer: generate random JS expressions, run with both
# jstro and node, compare outputs.  Reports any divergence.

require 'open3'
require 'tempfile'

class Gen
  PRIM = ["1", "2", "3", "0", "-1", "1.5", "true", "false", "null",
          '"a"', '""', '"hello"']
  OPS  = %w[+ - * / % == != === !== < > <= >= && ||]
  UNOPS = %w[+ - ! ~]

  def initialize(seed = nil)
    @rng = Random.new(seed || Random.new_seed)
  end

  def expr(depth = 4)
    return PRIM.sample(random: @rng) if depth <= 0
    case @rng.rand(10)
    when 0..2 then "(" + expr(depth - 1) + " " + OPS.sample(random: @rng) + " " + expr(depth - 1) + ")"
    when 3    then UNOPS.sample(random: @rng) + expr(depth - 1)
    when 4    then "(" + expr(depth - 1) + " ? " + expr(depth - 1) + " : " + expr(depth - 1) + ")"
    when 5    then "[#{(0..@rng.rand(4)).map { expr(depth - 1) }.join(',')}].length"
    when 6    then "({a: " + expr(depth - 1) + ", b: " + expr(depth - 1) + "}).a"
    when 7    then "Math.abs(" + expr(depth - 1) + ")"
    when 8    then "String(" + expr(depth - 1) + ")"
    else PRIM.sample(random: @rng)
    end
  end
end

def run(cmd, src)
  out, _err, _st = Open3.capture3(cmd, stdin_data: "console.log(JSON.stringify(#{src}))\n")
  out.strip
rescue
  ""
end

trials = (ENV['N'] || 500).to_i
diffs = 0
seen = 0
trials.times do |i|
  e = Gen.new(i).expr(rand(2..4))
  begin
    a = nil
    Tempfile.create(['fuzz', '.js']) do |f|
      f.write("console.log(JSON.stringify(#{e}))\n"); f.flush
      a = `./jstro #{f.path} 2>&1`.strip
    end
    b = nil
    Tempfile.create(['fuzz', '.js']) do |f|
      f.write("console.log(JSON.stringify(#{e}))\n"); f.flush
      b = `node #{f.path} 2>&1`.strip
    end
    seen += 1
    if a != b
      diffs += 1
      puts "DIFF #{i}: #{e}"
      puts "  jstro: #{a}"
      puts "  node:  #{b}"
    end
  rescue => err
    # ignore parse / runtime errors
  end
end
require 'tempfile'
puts
puts "Total: #{seen}, divergences: #{diffs}"
