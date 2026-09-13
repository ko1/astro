# Fixture for t2_unwind.rb: raises straight from the file's top level, so the
# '<top (required)>' frame is the innermost one.
x = 1
raise "top #{x}"
