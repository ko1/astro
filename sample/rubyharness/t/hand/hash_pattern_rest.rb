case {a: 1, b: 2, c: 3}; in {a:, **rest}; p [a, rest]; end
case {x: 1}; in {x:, **rest}; p rest; end
case {a: 1, b: 2}; in {a:, **nil}; p "no extra"; else; p "has extra"; end
case {a: 1}; in {a:, **nil}; p "exactly a"; else; p "no"; end
case {name: "Bob", age: 30, role: :admin}; in {name:, **other}; p [name, other]; end
case {a: 1, b: 2, c: 3, d: 4}; in {a: 1, **rest}; p rest; end
case {status: 200, data: [1,2]}; in {status: 200, **rest}; p rest; end
config = {host: "x", port: 80, opts: {ssl: true}}
case config; in {host:, port:, **extra}; p [host, port, extra]; end
case {a: 1, b: 2}; in {a: Integer => av, **rest}; p [av, rest]; end
def parse_opts(h); case h; in {required:, **opts}; [required, opts]; else; :invalid; end; end
p parse_opts({required: true, x: 1, y: 2})
p parse_opts({x: 1})
case {a: 1, b: 2, c: 3}; in {**all}; p all; end
case {type: :user, id: 5, name: "x", extra: 1}
in {type: :user, id:, **rest}; p [id, rest]
end
