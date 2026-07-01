# Integer/Float/Symbol #=== is an alias of #== (same UnboundMethod), and case/
# when / grep behaviour is unchanged. vs ruby.
p Integer.instance_method(:===) == Integer.instance_method(:==)
p Float.instance_method(:===) == Float.instance_method(:==)
p Symbol.instance_method(:===) == Symbol.instance_method(:==)
p(5 === 5)
p(5 === 5.0)
p(5 === "x")
p(2.5 === 2.5)
p(:a === :a)
p(:a === "a")
case 5
when 5 then p "int"
end
case :foo
when :foo then p "sym"
end
p [1, 2, 3, 2].grep(2)
p [:a, :b, :a].grep(:a)
