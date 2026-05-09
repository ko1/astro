fun fact n = if n <= 1 then 1 else n * fact (n - 1)
val _ = println (Int.toString (fact 10))
val _ = println (Int.toString (fact 15))
