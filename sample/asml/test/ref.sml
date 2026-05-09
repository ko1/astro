val r = ref 0;
r := !r + 5;
r := !r + 10;
println (Int.toString (!r))
