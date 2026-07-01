# `rescue => e` inside a block where e is an enclosing local writes to that
# outer local (previously a compile error: "local not in scope table"). vs ruby.
e = "predefined"
[1, 2, 3].each do |i|
  begin
    raise "err" if i.even?
  rescue => e
    p e.message
  end
end
p e.class
p e.message

errs = []
[1, 2].each { begin; raise "x"; rescue => e; errs << e.message; end }
p errs

# nested blocks
[[1]].each { |a| a.each { begin; raise "deep"; rescue => e; p e.message; end } }

# a purely block-local rescue var (depth 0) still works
[1].each { begin; raise "local"; rescue => local_only; p local_only.message; end }

# outer var not touched when no exception is raised
outer = "keep"
[1].each { begin; 1 + 1; rescue => outer; end }
p outer
