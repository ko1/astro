# Loader: shim Test::Unit, then load a CRuby test/ruby/*.rb file given
# on ARGV, and run every TestCase subclass that pops out.
#
# Usage:
#   koruby test/cruby_runner/run_cruby_test.rb path/to/cruby/test_foo.rb

DIR = File.dirname(__FILE__)
require_relative "tu_shim"

# Stub `require 'test/unit'` and friends so the test files don't fail.
# We define require / require_relative as no-ops at top level so any
# `require 'test/unit'` in the loaded test file silently succeeds.
# Defining them on Object directly (not via Kernel) so they don't
# accidentally shadow module-level Kernel.require dispatches.
$require_stubbed ||= false
unless $require_stubbed
  $require_stubbed = true
  $loaded_features ||= {}
  $require_target_dir = nil   # set just below to the test file's dir
  class Object
    # `require` (gem-style) — for `require 'test/unit'` etc. just succeed.
    # We don't have a load path so support files aren't reachable this way.
    def require(_name); true; end

    # `require_relative 'foo'` — try to actually load the file from the
    # test's own directory so e.g. test_marshal.rb's
    # `require_relative 'marshaltestlib'` works.
    def require_relative(name)
      base = $require_target_dir
      return true unless base
      path = File.join(base, name.to_s)
      path += ".rb" unless path.end_with?(".rb")
      return true unless File.exist?(path)
      return true if $loaded_features[path]
      $loaded_features[path] = true
      load path
      true
    rescue Exception => e
      # Failure to load support file: print + carry on.  Tests that
      # depend on it will fail individually rather than wedging the run.
      puts "  REQUIRE_RELATIVE FAIL: #{name} — #{e.class}: #{e.message}"
      true
    end

    def gem(*_args); true; end
  end
end

target = ARGV[0]
abort "no test file" unless target
$require_target_dir = File.dirname(File.expand_path(target))

before = Test::Unit::TestCase.descendants rescue (Test::Unit::TestCase.respond_to?(:descendants) ? Test::Unit::TestCase.descendants : [])

# Track subclasses ourselves since we don't have Class#descendants.
$tu_classes = []
tc = Test::Unit::TestCase
class << tc
  def inherited(child)
    $tu_classes << child
  end
end

begin
  load target
  puts "loaded; subclasses: #{$tu_classes.size}"
rescue Exception => e
  puts "LOAD ERROR: #{e.class}: #{e.message}"
  e.backtrace[0, 5].each { |l| puts "  #{l}" } rescue nil
  report_tu(File.basename(target))
  exit 1
end

$tu_classes.uniq.each do |k|
  puts "  class: #{k.name}, methods: #{k.test_methods.size rescue '?'}"
  begin
    k.run_all
  rescue Exception => e
    puts "  CLASS-LEVEL ERR: #{e.class}: #{e.message rescue '?'}"
  end
end

report_tu(File.basename(target))
