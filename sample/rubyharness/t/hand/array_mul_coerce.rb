# Array#* coercion: #to_str (join) takes precedence over #to_int (repeat),
# matching CRuby's rb_check_string_type-then-to_int order. vs ruby.
class OnlyToInt; def to_int; 3; end; end
class OnlyToStr; def to_str; "+"; end; end
class Both; def to_int; 9; end; def to_str; "."; end; end
p [1, 2] * OnlyToInt.new          # repeat
p [1, 2, 3] * OnlyToStr.new       # join
p [:a, :b] * Both.new             # to_str wins → join
p [1, 2, 3] * 2                   # plain int
p [1, 2] * ","                    # plain string
begin; [1, 2] * Object.new; rescue => e; p e.class; end
