# A proc that captured `yield` must keep working after its defining method frame
# is gone.  Each block below runs a DISTURBANCE between the escape and the call:
#   plain  - nothing
#   steal  - another method of the same frame shape yields, so its block trio
#            lands exactly on the cells the returned frame used
#   bury   - a deep recursion overwrites those cells with live data
# If the proc still reads the returned frame, `steal` yields the wrong block and
# `bury` crashes.  (koruby_precise: was a use-after-free; see docs/todo.md.)

def steal; yield; end

def bury(n)
  a = n; b = n + 1; c = n + 2; d = n + 3; e = n + 4
  f = n + 5; g = n + 6; h = n + 7; i = n + 8; j = n + 9
  return 0 if n <= 0
  bury(n - 1) + a + b + c + d + e + f + g + h + i + j
end

def disturb
  yield
  steal { 99 }
  yield
  bury(200)
  yield
end

# --- the escaping shapes ------------------------------------------------------
def mk_proc;    proc { yield };        end
def mk_procnew; Proc.new { yield };    end
def mk_lambda;  lambda { yield };      end
def mk_arrow;   -> { yield };          end
def mk_arg;     proc { yield 3 };      end
def mk_splat;   a = [1, 2]; proc { yield(*a) }; end
def mk_lvar;    x = 1; proc { x + yield }; end
def mk_nested;  proc { proc { yield } }; end
def mk_deep;    [1].map { proc { yield } }[0]; end
def mk_flags;   proc { [block_given?, defined?(yield)] }; end
def mk_two;     [proc { yield }, proc { yield }]; end
def mk_shared;  x = 0; [proc { x += yield }, proc { x }]; end
obj = Object.new
def obj.mk;     proc { yield };        end

pr = mk_proc    { 7 }; disturb { p pr.call }
pr = mk_procnew { 7 }; disturb { p pr.call }
pr = mk_lambda  { 7 }; disturb { p pr.call }
pr = mk_arrow   { 7 }; disturb { p pr.call }
pr = obj.mk     { 7 }; disturb { p pr.call }
pr = mk_arg  { |x| x * 2 };   disturb { p pr.call }
pr = mk_splat { |x, y| x + y }; disturb { p pr.call }
pr = mk_lvar { 7 };           disturb { p pr.call }
pr = mk_nested { 7 };         disturb { p pr.call.call }
pr = mk_deep { 7 };           disturb { p pr.call }
pr = mk_flags { 7 };          disturb { p pr.call }
pr = mk_flags;                disturb { p pr.call }
a, b = mk_two { 7 };          disturb { p [a.call, b.call] }
a, b = mk_shared { 5 };       disturb { a.call; p [a.call, b.call] }

# the yielded-to block itself closes over a frame that also dies
def outer_chain; x = 7; mk_proc { x }; end
pr = outer_chain;             disturb { p pr.call }

# block argument kinds, reached through the escaped proc
pr = mk_arg(&:to_s);              disturb { p pr.call }
pr = mk_arg(&->(x) { x * 10 });   disturb { p pr.call }
pr = mk_proc { 7 };               disturb { p(begin; mk_proc.call; rescue LocalJumpError; :ljerror; end) }

# `yield` in a block whose OWN frame materialized an env (a sibling closure
# captured an outer local) must still reach the METHOD's block, not its own scope.
def own_env
  y = 1
  [1].each { sib = proc { y }; p(yield); p sib.call }
  nil
end
own_env { 7 }
