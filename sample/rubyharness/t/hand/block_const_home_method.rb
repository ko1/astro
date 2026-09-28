module D
  def self.append_features(mod)
    super
    mod.module_eval do
      class << self
        COLORS = [:blue]
        def colors(*keys)
          keys.each do |k|
            define_method(:"#{k}=") { |v| [k, COLORS.include?(v)] }
          end
        end
      end
    end
  end
end
class Font; include D; colors :color; end
p Font.new.send(:color=, :blue)
module D2
  def self.append_features(mod)
    super
    mod.module_eval do
      class << self
        COLORS = [:blue]
        def colors(*keys)
          keys.each do |k|
            define_method(:"#{k}=") { |v| COLORS.include?(v) }
            define_method(:"#{k}_list") { COLORS }
          end
        end
        def direct = COLORS
      end
    end
  end
end
class Font2; include D2; colors :color; end
f = Font2.new
p f.send(:color=, :blue), f.color_list, Font2.direct
