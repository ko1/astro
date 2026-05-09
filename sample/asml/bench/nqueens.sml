(* N-Queens — classic backtracking search.  10-queens has 724 solutions. *)

fun abs n = if n < 0 then ~n else n

fun safe (q, qs) =
  let
    fun loop (qs, d) =
      case qs of
          []      => true
        | h :: t  => h <> q andalso abs (h - q) <> d andalso loop (t, d + 1)
  in
    loop (qs, 1)
  end

fun add_queen (col, row, n, sols, qs) =
  if row > n then sols
  else
    let
      val sols' =
        if safe (row, qs)
        then place_queens (col + 1, n, sols, row :: qs)
        else sols
    in
      add_queen (col, row + 1, n, sols', qs)
    end
and place_queens (col, n, sols, qs) =
  if col > n then sols + 1
  else add_queen (col, 1, n, sols, qs)

fun rep (k, n) =
  if k = 0 then 0
  else
    let val s = place_queens (1, n, 0, [])
    in if k = 1 then s else (rep (k - 1, n))
    end

val _ = println (Int.toString (rep (3, 10)))
