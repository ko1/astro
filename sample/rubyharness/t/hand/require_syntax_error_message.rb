# A SyntaxError from require/load carries prism's "FILE:LINE: syntax errors found"
# header (rspec-core matches /file.rb:\d: syntax error/), and #path names the file.
require 'tmpdir'
Dir.mktmpdir do |d|
  f = File.join(d, "broken_file.rb")
  File.write(f, "\nclass WorkInProgress\n  def initialize(arg)\n  def foo\n  end\nend\n")
  [-> { require f }, -> { load f }].each do |pr|
    begin
      pr.call
    rescue SyntaxError => e
      p e.message.lines.first.sub(d, "DIR"), e.path == f
    end
  end
end
