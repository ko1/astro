require 'minitest/autorun'
require 'rbconfig'
require_relative '../lib/abruby'

ABRUBY_MODE = ENV['ABRUBY_MODE'] || 'plain'

class AbRubyTest < Minitest::Test
  def setup
    @vm = AbRuby.new
    if ABRUBY_MODE == 'compiled'
      src_dir = File.expand_path('..', __dir__)
      store_dir = File.join(src_dir, 'code_store')
      so_mtime = File.mtime(File.join(src_dir, 'abruby.so')).to_i rescue 0
      AbRuby.cs_init(store_dir, src_dir, so_mtime)
    end
  end

  def assert_eval(code, expected)
    result = AbRuby.eval(code)
    if expected.nil?
      assert_nil result, "AbRuby.eval(#{code.inspect})"
    else
      assert_equal expected, result, "AbRuby.eval(#{code.inspect})"
    end
  end
end
