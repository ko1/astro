require "set"
p Set[1, 2, 3, 4, 5].delete_if { |x| x.even? }.to_a.sort
p Set[1, 2, 3, 4, 5].keep_if { |x| x.even? }.to_a.sort
p Set[1, 2, 3, 4, 5].reject! { |x| x > 3 }.to_a.sort
p Set[1, 2].reject! { |x| x > 5 }
p Set[1, 2, 3, 4, 5].select! { |x| x > 3 }.to_a.sort
p Set[1, 2].select! { |x| x > 0 }
p Set[1, 2, 3, 4, 5].map! { |x| x * 10 }.to_a.sort
p Set[1, 2, 3].filter! { |x| x > 1 }.to_a.sort
c = Set[1, 2, 3, 4].classify { |x| x.even? ? :e : :o }
p c[:e].to_a.sort
p c[:o].to_a.sort
