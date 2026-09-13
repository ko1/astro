def y0; yield; end
def foo(*a); a; end
def go
  pr = proc { puts caller(0) }
  begin; foo(1, 2, 3, 4, 5, 6, y0); rescue LocalJumpError; end
  pr.call
end
go
puts "--"
def go2
  pr = proc { puts caller(0) }
  begin; y0; rescue LocalJumpError; end
  pr.call
end
go2
