# Fixture for t2_unwind.rb: raises so the raise leaves the file through
# require/load.  Line numbers matter — keep them stable.
def t2_boom
  raise 'boom'
end

t2_boom
