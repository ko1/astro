# __method__ through a binding taken in a proc an eval'd string made — after
# that eval has returned (pry's Pry::Method.from_binding).
def b3 = eval("proc { binding }")
p b3.call.eval("__method__")
def b4 = eval("binding")
p b4.eval("__method__")
def b5 = proc { binding }
p b5.call.eval("__method__")
def b6 = eval("__method__")
p b6
def b7 = eval("proc { __method__ }").call
p b7
def b8 = instance_eval("proc { binding }")
p b8.call.eval("__method__")
