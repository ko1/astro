# pad
# pad
# pad
class RC
  def cm
    l = caller_locations(0)[0]
    [File.basename(l.path), l.lineno, method(:cm).source_location.then { |p, n| [File.basename(p), n] }]
  end
end
