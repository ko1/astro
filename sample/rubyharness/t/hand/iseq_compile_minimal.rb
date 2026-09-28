p((begin; RubyVM::InstructionSequence.compile("def ("); rescue SyntaxError => e; e.class; end))
p RubyVM::InstructionSequence.compile("x = 5; x * 2").eval, defined?(x)
p RubyVM::InstructionSequence.new("puts 1").class
p RubyVM::InstructionSequence.compile("1\n2\n", "f.rb", "/a/f.rb", 10).absolute_path
i = RubyVM::InstructionSequence.compile("1+2"); p i, i.eval, i.path, i.first_lineno, i.label, i.absolute_path, RubyVM::InstructionSequence.of(method(:p))
