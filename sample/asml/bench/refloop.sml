(* refloop — ref cell を while-loop ふうに使い、deref / assign を
   ホットに踏む。内側 loop は 1-arg で APPN_FAST_PATH のホットパスに乗る。 *)

fun println s = (print s; print "\n")

fun count n =
  let
    val r = ref 0
    fun loop i =
      if i >= n then !r
      else (r := !r + i; loop (i + 1))
  in loop 0 end

val _ = println (Int.toString (count 50000000))
