e = [1].each
p e.next
p (begin; e.next; rescue StopIteration; "SI"; end)
p (StopIteration.ancestors.include?(IndexError))
p (begin; [].each.peek; rescue StopIteration; "SI"; end)
