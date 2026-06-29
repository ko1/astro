log = []
sup = Class.new do
  def self.inherited(c); $log << :inherited; super; end
end
$log = log
d = Class.new(sup) { $log << :block_run }
p log
p (Class.new(sup).superclass == sup)
p Class.new.superclass
