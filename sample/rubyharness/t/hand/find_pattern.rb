case [1, 2, 3, 4, 5]; in [*, 3, *post]; p post; end
case [1, 2, 3, 4, 5]; in [*pre, 3, *]; p pre; end
case [1, 2, 3, 4, 5]; in [*pre, 3, *post]; p [pre, post]; end
case ["a", 1, "b", 2, "c"]; in [*, String => s, Integer => i, *]; p [s, i]; end
case [1, 2, 3]; in [*, 2, *]; p "found 2"; else; p "no"; end
case [1, 2, 3]; in [*, 9, *]; p "found 9"; else; p "no 9"; end
case [1, 2, 3, 2, 1]; in [*a, 2, *b]; p [a, b]; end
case [10, 20, 30]; in [*, Integer => x, *] if x > 25; p "big: #{x}"; else; p "small"; end
case [:a, :b, :c, :d]; in [*, :c, *post]; p post; end
log = ["start", "process", "error", "cleanup", "end"]
case log; in [*before, "error", *after]; p [before.size, after]; end
case [[1,2], [3,4], [5,6]]; in [*, [3, x], *]; p x; end
case []; in [*, 1, *]; p "found"; else; p "empty no match"; end
case [1]; in [*, 1, *]; p "single found"; end
