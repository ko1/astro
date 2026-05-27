# run a single rubyspec file under koruby with the mspec_shim.
# usage: ./koruby test/cruby_runner/run_rubyspec.rb <spec.rb>

target = ARGV[0]
unless target
  puts "usage: koruby run_rubyspec.rb <spec.rb>"
  exit 1
end

load File.expand_path('mspec_shim.rb', __dir__)

# Stub out require/require_relative — most language specs only
# do `require_relative '../spec_helper'` and otherwise don't need it.
class Object
  alias_method :__orig_require_relative, :require_relative
  def require_relative(path)
    return true if path =~ /spec_helper/
    __orig_require_relative(path) rescue false
  end
  alias_method :__orig_require, :require
  def require(path)
    return false if path == 'mspec' || path.start_with?('mspec/')
    __orig_require(path) rescue false
  end
end

begin
  load target
rescue Exception => e
  puts "LOAD ERROR: #{e.class}: #{e.message}"
  e.backtrace[0, 5].each { |l| puts "  #{l}" } rescue nil
end

base = File.basename(target)
puts "#{base}: pass=#{$ms_pass} fail=#{$ms_fail} err=#{$ms_error} skip=#{$ms_skip}"
exit($ms_fail + $ms_error == 0 ? 0 : 1)
