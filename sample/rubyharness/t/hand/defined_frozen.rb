# defined? returns a frozen String. vs ruby.
p defined?(self)
p defined?(self).frozen?
p defined?(nil).frozen?
p defined?(true).frozen?
p defined?(false).frozen?
p defined?(puts).frozen?
x = 1
p defined?(x).frozen?
p defined?([1, 2]).frozen?
@iv = 1
p defined?(@iv).frozen?
