# required by cfile_lib.rb: raises at its own top level, so the unwind crosses
# the Kernel#require_relative C frame, whose file is the requirer's (cfile_lib).
[1].each { raise "from required file" }
