def rm
  l = caller_locations(0)[0]
  [File.basename(l.path), l.lineno, method(:rm).source_location.then { |p, n| [File.basename(p), n] }]
end
