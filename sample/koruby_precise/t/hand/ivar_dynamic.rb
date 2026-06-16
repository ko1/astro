class Config
  DEFAULTS = { loglevel: 1, frames: 0, scale: 2 }
  def initialize(opts)
    DEFAULTS.merge(opts).each { |id, val| instance_variable_set(:"@#{id}", val) }
  end
  def loglevel = @loglevel
  def get(n) = instance_variable_get(:"@#{n}")
end
c = Config.new({ frames: 30 })
p c.loglevel
p c.get(:frames)
p c.get(:scale)
p c.instance_variable_get("@loglevel")
c.instance_variable_set(:@scale, 4)
p c.get(:scale)
# interpolated symbol values
3.times { |i| p :"item_#{i}" }
p :"plain"
