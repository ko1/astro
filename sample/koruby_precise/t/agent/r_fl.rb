def m
  a = 1; b = 2
  p __frame_locals.sort
  p send(:__frame_locals).sort
  p self.send(:__frame_locals).sort rescue p $!
end
m
class K
  def n
    q = 9
    p Kernel.instance_method(:__frame_locals) rescue p $!
  end
end
K.new.n
