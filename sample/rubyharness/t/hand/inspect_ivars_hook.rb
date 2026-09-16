# Ruby 4.x: a private #instance_variables_to_inspect tells the default #inspect
# which ivars to show.  nil = all; an Array FILTERS (the object's own ivar order
# is kept, not the list's); anything else is a TypeError.
def t(tag); r = yield; puts "#{tag}: => #{r.inspect}"; rescue => e; puts "#{tag}: #{e.class}: #{e.message}"; end
n = ->(s) { s.gsub(/0x\h+/, "0xA") }
mk = lambda do |body|
  o = Object.new
  o.instance_eval { @host = "localhost"; @user = "root"; @password = "hunter2" }
  o.singleton_class.class_eval(&body) if body
  o
end
t("subset")   { n.(mk.(proc { private def instance_variables_to_inspect = %i[@host @user @does_not_exist] }).inspect) }
t("empty")    { n.(mk.(proc { private def instance_variables_to_inspect = [] }).inspect) }
t("nil")      { n.(mk.(proc { private def instance_variables_to_inspect = nil }).inspect) }
t("reorder")  { n.(mk.(proc { private def instance_variables_to_inspect = %i[@user @host] }).inspect) }
t("bad")      { mk.(proc { private def instance_variables_to_inspect = {} }).inspect }
t("none")     { n.(mk.(nil).inspect) }
t("respond")  { Object.new.respond_to?(:instance_variables_to_inspect, true) }
t("public?")  { Object.new.respond_to?(:instance_variables_to_inspect) }
