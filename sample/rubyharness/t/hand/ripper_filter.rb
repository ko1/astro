require "ripper"
class Counter < Ripper::Filter
  def on_ident(tok, data) = (data[:ident] << [tok, lineno, column]; data)
  def on_default(event, tok, data) = (data[:n] += 1; data)
end
p Counter.new("a = 1\nfoo(bar)\n").parse({ident: [], n: 0})
p Ripper::Filter.new("x").filename, Ripper::Filter.new("x", "f.rb").filename
