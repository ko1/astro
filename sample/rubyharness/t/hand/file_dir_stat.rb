# File stat predicates + File.read + Dir (deterministic paths). vs ruby.
p File.exist?(__FILE__)
p File.exist?("/nonexistent_koruby_zzz_98765")
p File.file?(__FILE__)
p File.file?("/")
p File.directory?("/")
p File.directory?(__FILE__)
p File.size(__FILE__) > 0
p Dir.pwd.class
p Dir.pwd.start_with?("/")
p Dir.exist?("/")
p Dir.exist?("/nonexistent_koruby_zzz")
src = File.read(__FILE__)
p src.class
p src.include?("File.exist?")
p src == File.read(__FILE__)
