require "#{ENV['HOME']}/ruby/src/master/spec/ruby/core/kernel/fixtures/classes"
EvalSpecs::A.new.c
p EvalSpecs::A.constants(false).sort
begin; p EvalSpecs::A::C.name; rescue => e; p e; end
