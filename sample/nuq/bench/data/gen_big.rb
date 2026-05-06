#!/usr/bin/env ruby
# Generate ~100MB of diverse-shape JSON for the big-data bench suite.
#
# Outputs to bench/data/big/:
#   users_big.json   ~25MB — wide flat array of 130k user records
#   logs_big.json    ~25MB — HTTP-log entries (object with nested headers)
#   tree_big.json    ~25MB — deeply nested tree (recursive)
#   table_big.json   ~25MB — array of small arrays (CSV-like rows)
#
# Each shape stresses a different jq access pattern.

require 'json'

OUT = File.join(__dir__, 'big')

# 1. users_big.json — like users.json but ~25× larger.
def gen_users(n, path)
  cities = %w[Tokyo Osaka Kyoto Nagoya Sapporo Fukuoka Sendai Yokohama]
  tags = %w[alpha beta gamma delta epsilon zeta eta theta]
  File.open(path, 'w') do |f|
    f.print '['
    n.times do |i|
      f.print ',' unless i == 0
      f.print JSON.generate({
        "id" => i,
        "name" => "user#{i}",
        "email" => "user#{i}@example.com",
        "age" => (18 + (i % 60)),
        "active" => (i % 3 != 0),
        "city" => cities[i % cities.size],
        "score" => (i * 137 % 1000) / 10.0,
        "tags" => tags.first(1 + i % 4),
        "stats" => { "logins" => i % 100, "posts" => i % 50, "followers" => (i * 7) % 200 },
        "address" => {
          "street" => "#{1 + i % 999} Main St",
          "zip" => format("%05d", i % 100000),
          "country" => "JP"
        }
      })
    end
    f.print ']'
  end
  puts "users_big.json:  #{n} entries, #{File.size(path) / 1024 / 1024} MB"
end

# 2. logs_big.json — array of HTTP request log entries with nested headers.
def gen_logs(n, path)
  methods = %w[GET POST PUT DELETE]
  paths = %w[/api/users /api/posts /api/comments /static/css /static/js / /health /metrics]
  File.open(path, 'w') do |f|
    f.print '['
    n.times do |i|
      f.print ',' unless i == 0
      status = [200, 200, 200, 201, 301, 400, 404, 500][i % 8]
      f.print JSON.generate({
        "ts" => 1700000000 + i,
        "method" => methods[i % methods.size],
        "path" => paths[i % paths.size],
        "status" => status,
        "duration_ms" => (i * 13) % 500,
        "client_ip" => "10.#{(i / 65536) % 256}.#{(i / 256) % 256}.#{i % 256}",
        "headers" => {
          "user-agent" => "Mozilla/5.0 (compatible; bench/#{i % 100})",
          "accept" => "application/json",
          "x-request-id" => "req-#{i}",
          "host" => "example.com"
        },
        "bytes_out" => (i * 419) % 100000
      })
    end
    f.print ']'
  end
  puts "logs_big.json:   #{n} entries, #{File.size(path) / 1024 / 1024} MB"
end

# 3. tree_big.json — recursive tree of objects with branching.
# Depth ~5, branching ~10 → 100k leaves.
def gen_tree(target_bytes, path)
  # We want a mostly-uniform tree.  At depth=4 branching=12 → ~20k leaves
  # → ~25MB after generating chunky leaves.  Adjust branching to hit size.
  branching = 16
  depth = 4
  build = lambda do |d|
    if d == 0
      # leaf: object with several fields
      {
        "type" => "leaf",
        "value" => rand(10000),
        "label" => "leaf_#{rand(1000000)}",
        "tags" => Array.new(3) { "t#{rand(100)}" },
        "meta" => {
          "created" => 1700000000 + rand(1_000_000),
          "score" => rand * 100,
          "active" => [true, false].sample
        }
      }
    else
      {
        "type" => "branch",
        "depth" => d,
        "id" => "b#{d}_#{rand(1000000)}",
        "children" => Array.new(branching) { build.call(d - 1) }
      }
    end
  end
  loop do
    File.write(path, JSON.generate(build.call(depth)))
    break if File.size(path) >= target_bytes * 0.8
    branching += 2
  end
  puts "tree_big.json:   depth=#{depth} branching=#{branching}, #{File.size(path) / 1024 / 1024} MB"
end

# 4. table_big.json — array of small arrays (CSV row-like).
def gen_table(n, path)
  File.open(path, 'w') do |f|
    f.print '['
    n.times do |i|
      f.print ',' unless i == 0
      f.print JSON.generate([
        i,                                      # id
        "name_#{i}",                            # name
        (i * 137 % 1000) / 10.0,                # value1
        (i * 419 % 10000) / 100.0,              # value2
        (i % 7 == 0),                           # flag
        ["red", "green", "blue", "yellow"][i % 4],
        format("%08x", i * 12345 % 0xffffffff)
      ])
    end
    f.print ']'
  end
  puts "table_big.json:  #{n} rows, #{File.size(path) / 1024 / 1024} MB"
end

FileUtils.mkdir_p(OUT) if !Dir.exist?(OUT)
require 'fileutils'

# Targets: ~25MB each, ~100MB total.
gen_users(130_000, "#{OUT}/users_big.json")     # ~25MB
gen_logs(120_000,  "#{OUT}/logs_big.json")      # ~25MB
gen_tree(25 * 1024 * 1024, "#{OUT}/tree_big.json")
gen_table(800_000, "#{OUT}/table_big.json")     # ~25MB

total = Dir["#{OUT}/*.json"].sum { |p| File.size(p) }
puts "---"
puts "total: #{total / 1024 / 1024} MB across #{Dir["#{OUT}/*.json"].size} files"
