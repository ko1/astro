# a mode string is validated whole (a path passed as the mode is an ArgumentError, not ENOENT)
%w[sample.log .quick_test.log rw rbt r+x].each do |m|
  begin; File.open("/nonexistent_dir_zz/f", m) {}; rescue ArgumentError, SystemCallError => e; p [e.class, e.message[0, 40]]; end
end
# Date.today / DateTime.now read the clock themselves: a stubbed Time.now does not move them
require 'date'
class << Time; alias_method :orig_now, :now; def now = Time.utc(2015, 12, 9); end
p Date.today.year > 2020, DateTime.now.year > 2020, Time.now.year
class << Time; alias_method :now, :orig_now; end
