
$stdout.flush rescue nil
puts "pass=#{$ms_pass} fail=#{$ms_fail} err=#{$ms_error} skip=#{$ms_skip}"
