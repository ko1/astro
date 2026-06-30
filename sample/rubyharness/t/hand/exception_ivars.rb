class NotFound < StandardError
  attr_reader :id
  def initialize(id); @id = id; super("not found: #{id}"); end
end
begin; raise NotFound.new(42); rescue => e; p [e.message, e.id]; end
e = NotFound.new(7)
p e.id
p e.instance_variable_get(:@id)
e.instance_variable_set(:@extra, "x")
p e.instance_variable_get(:@extra)
p e.instance_variables.sort
p e.instance_variable_defined?(:@id)
p e.instance_variable_defined?(:@nope)
class Multi < RuntimeError
  attr_accessor :code, :detail
  def initialize(c, d); @code = c; @detail = d; super("err #{c}"); end
end
m = Multi.new(404, "missing")
p [m.code, m.detail, m.message]
m.code = 500
p m.code
class Plain < StandardError; end
p Plain.new("msg").message
p Plain.new.message
begin; raise Multi.new(1, "x"); rescue => ex; p ex.code; p ex.is_a?(RuntimeError); end
