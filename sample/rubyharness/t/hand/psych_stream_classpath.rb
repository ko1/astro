# Psych delivers the events before a syntax error first (libyaml streams), so a
# handler that stops at the first document never sees a later error; an
# unknown class path is CRuby's ArgumentError naming the missing segment.
require 'yaml'
class PsH < Psych::Handler
  def end_document(implicit) = throw(:done, :first_doc_ok)
end
y = "---\ntitle: Blah\n---\n\nsome text\n\nfoo: bar\n"
p catch(:done) { Psych::Parser.new(PsH.new).parse(y) }
p YAML.load(y)
p((YAML.load_stream(y) rescue [$!.class, $!.message]))
p((YAML.load("a: [1, 2\n") rescue [$!.class, $!.line, $!.column, $!.problem]))
p((YAML.load("--- 1\n--- [\n") rescue [$!.class, $!.message]))
["Nope9", "Nope9::A", "Psych::Nope9::A", "String::A"].each do |n|
  p((Psych::ClassLoader.new.send(:path2class, n) rescue [$!.class, $!.message]))
end
p((YAML.unsafe_load("--- !ruby/object:Mechanize::Cookie\nname: a\n") rescue [$!.class, $!.message]))
