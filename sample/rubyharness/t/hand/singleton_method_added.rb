$log = []
obj = Object.new
def obj.singleton_method_added(name); $log << name; end
def obj.foo; end
def obj.bar; end
p $log
module M
  def self.singleton_method_added(name); (@l ||= []) << name; end
  def self.helper; end
  def self.l; @l; end
end
p M.l
