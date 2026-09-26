# String Range#include? with one-byte ASCII ends compares codes ("A".."z" holds "_").
p [("A".."z").include?("z"), ("A".."z").include?("_"), ("a".."z").include?("bb"), ("a".."zz").include?("bb"), ("A".."z") === "z",
   ("A".."z").member?("z"), ("A".."z").cover?("zz"), ("aa".."b").include?("az"), ("a"..."z").include?("z"), ("a".."z").include?(""),
   ("z".."a").include?("m"), ("a".."e").to_a.size, ("A".."z").include?("é")]
