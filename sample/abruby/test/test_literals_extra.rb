require_relative 'test_helper'

class TestLiteralsExtra < AbRubyTest
  # heredoc
  def test_heredoc = assert_eval("<<~HEREDOC\nhello\nHEREDOC", "hello\n")
  def test_heredoc_multi = assert_eval("<<~HEREDOC\nhello\nworld\nHEREDOC", "hello\nworld\n")
  def test_heredoc_with_indent = assert_eval("<<~HEREDOC\n  hello\n  world\nHEREDOC", "hello\nworld\n")

  # %w
  def test_percent_w = assert_eval('%w(a b c)', ["a", "b", "c"])
  def test_percent_w_empty = assert_eval('%w()', [])
  def test_percent_w_single = assert_eval('%w(hello)', ["hello"])

  # %i
  def test_percent_i = assert_eval('%i(a b c)', [:a, :b, :c])
  def test_percent_i_empty = assert_eval('%i()', [])
  def test_percent_i_single = assert_eval('%i(foo)', [:foo])
end
