module Outer; INNER = 1; module Mid; DEEP = 2; end; end
TOP = 99
p Outer.const_defined?(:INNER)
p Outer.const_defined?("Mid::DEEP")
p Object.const_defined?("Outer::Mid::DEEP")
p Object.const_defined?("::TOP")
p Object.const_defined?("Outer::Nope")
p Object.const_defined?(:Missing)
p Outer.const_defined?("Mid")
