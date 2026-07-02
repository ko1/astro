# ENV (deterministic: only touch our own keys) + ARGV. vs ruby.
p ENV.class
ENV["KORUBY_T1"] = "alpha"
ENV["KORUBY_T2"] = "beta"
p ENV["KORUBY_T1"]
p ENV["KORUBY_MISSING_ZZZ"]
p ENV.key?("KORUBY_T1")
p ENV.key?("KORUBY_MISSING_ZZZ")
p ENV.fetch("KORUBY_T2")
p ENV.fetch("KORUBY_MISSING_ZZZ", "dflt")
p ENV.fetch("KORUBY_MISSING_ZZZ") { |k| "blk:#{k}" }
p ENV.keys.include?("KORUBY_T1")
p ENV.values.include?("alpha")
p ENV.to_h["KORUBY_T2"]
p ENV.has_value?("beta")
p ENV.size > 0
collected = {}
ENV.each { |k, v| collected[k] = v if k.start_with?("KORUBY_T") }
p collected
p ENV.delete("KORUBY_T1")
p ENV["KORUBY_T1"]
p ENV.delete("KORUBY_T1")
begin; ENV.fetch("KORUBY_MISSING_ZZZ"); rescue KeyError => e; p e.message.include?("KORUBY_MISSING_ZZZ"); end
ENV["KORUBY_T2"] = nil
p ENV["KORUBY_T2"]
p ARGV.class
p ARGV.length >= 0
