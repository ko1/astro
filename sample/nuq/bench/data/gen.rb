#!/usr/bin/env ruby
# Generate a synthetic users.json dataset for the real-world benches.
require 'json'

n = (ARGV[0] || 10000).to_i
arr = []
n.times do |i|
  arr << {
    "id" => i,
    "name" => "user#{i}",
    "email" => "user#{i}@example.com",
    "age" => (18 + (i % 60)),
    "active" => (i % 3 != 0),
    "city" => ["Tokyo", "Osaka", "Kyoto", "Nagoya", "Sapporo"][i % 5],
    "score" => (i * 137 % 1000) / 10.0,
    "tags" => ["alpha", "beta", "gamma", "delta"].first(1 + i % 4),
    "stats" => { "logins" => i % 100, "posts" => i % 50, "followers" => (i * 7) % 200 },
  }
end
File.write("#{__dir__}/users.json", JSON.generate(arr))
puts "users.json: #{n} entries, #{File.size("#{__dir__}/users.json")} bytes"
