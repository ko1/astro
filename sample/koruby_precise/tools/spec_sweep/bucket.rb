# Bucket harvested failure blocks (harvest.rb output) by a normalized signature.
#
#   ruby tools/spec_sweep/bucket.rb <raw.json> [top]
#
# The point is to surface ONE root cause spread over N one-example files.  The
# signature strips the parts that differ per call site (quoted literals, numbers,
# object addresses, receiver class names in some shapes) and keeps the shape of
# the complaint plus, for NoMethodError/NameError/ArgumentError, the thing that
# was missing -- that is the part that maps to one fix.
require 'json'

def normalize(b)
  m = b['msg'].to_s
  first = m.lines.first.to_s.chomp
  case first
  # mspec ExceptionState prints "ClassName: message" for an unexpected exception.
  when /\Aundefined method ['`](.+?)' for /                  then "NoMethodError: ##{$1}"
  when /\ANoMethodError: undefined method ['`](.+?)' for /   then "NoMethodError: ##{$1}"
  when /\A(?:NameError: )?undefined local variable or method ['`](.+?)'/ then "NameError: #{$1}"
  when /\A(?:NameError: )?uninitialized constant (\S+)/      then "NameError: uninitialized constant #{$1}"
  when /\AArgumentError: (.*)\z/                             then "ArgumentError: #{scrub($1)}"
  when /\ATypeError: (.*)\z/                                 then "TypeError: #{scrub($1)}"
  when /\A(\w+(?:::\w+)*Error|\w+Exception): (.*)\z/         then "#{$1}: #{scrub($2)}"
  else
    "FAIL: #{scrub(m.gsub(/\s+/, ' ').strip)[0, 160]}"
  end
end

# Drop the per-call-site noise so identical complaints collapse.
def scrub(s)
  s = s.dup
  s.gsub!(/0x[0-9a-f]+/, '0xADDR')
  s.gsub!(/"(?:[^"\\]|\\.)*"/, '"S"')
  s.gsub!(/'(?:[^'\\]|\\.)*'/, "'S'")
  s.gsub!(/#<[^>]*>/, '#<OBJ>')
  s.gsub!(/\b\d+\b/, 'N')
  s.gsub!(/\s+/, ' ')
  s.strip
end

raw = JSON.parse(File.read(ARGV[0] || '/tmp/ksw/raw12.json'))
# mspec wraps a mock-verification failure in an outer block whose first line is
# "An exception occurred during: Mock.verify_count" and whose *message* starts
# with the real example description.  Unwrap it or 13 unrelated causes collapse
# into one meaningless bucket.
# Unwrap ONLY the mock wrapper.  The other shapes mspec wraps the same way
# ("loading <path>", "before :each") carry the real message on line 1 of msg,
# and shifting it away leaves an empty signature -- that collapsed all 68
# LoadError files into one blank bucket.
raw.each do |b|
  next unless b['desc'].to_s =~ /\AAn exception occurred during: (?:Mock\.verify_count|before |after )/
  lines = b['msg'].to_s.lines
  head  = lines.shift.to_s.chomp
  b['desc'] = head.sub(/\s(FAILED|ERROR)\z/, '')
  b['msg']  = lines.join
end

# A file that cannot even load says nothing about the interpreter's semantics
# (missing stdlib).  Count them, then drop them: bucketing them buries the rest.
loaderr, raw = raw.partition { |b| b['msg'].to_s.start_with?('LoadError:') }
unless loaderr.empty?
  puts "# dropped %d LoadError blocks / %d files (missing stdlib):" %
       [loaderr.size, loaderr.map { |b| b['rel'] }.uniq.size]
  loaderr.group_by { |b| b['msg'].to_s.lines.first.to_s.chomp }
         .sort_by { |_, v| -v.size }
         .each { |m, v| puts "#   %-4d %s" % [v.size, m] }
  puts
end
top = (ARGV[1] || 40).to_i

buckets = Hash.new { |h, k| h[k] = [] }
raw.each { |b| buckets[normalize(b)] << b }

rows = buckets.map { |sig, bs| [sig, bs.map { |b| b['rel'] }.uniq, bs] }
rows.sort_by! { |sig, files, bs| [-files.size, -bs.size, sig] }

puts "# %d failure blocks / %d distinct signatures / %d files" %
     [raw.size, buckets.size, raw.map { |b| b['rel'] }.uniq.size]
puts
printf("%-5s %-5s  %s\n", "files", "exs", "signature")
puts "-" * 100
rows.first(top).each do |sig, files, bs|
  printf("%-5d %-5d  %s\n", files.size, bs.size, sig)
  files.first(8).each { |f| puts "            #{f}" }
  puts "            ... and #{files.size - 8} more" if files.size > 8
end
puts
puts "# tail: signatures hit in exactly 1 file: #{rows.count { |_, f, _| f.size == 1 }}"

# --- Second view: bucket by the EXAMPLE DESCRIPTION tail. -------------------
# Shared specs (core/*/shared/*.rb, it_behaves_like) produce the same sentence
# in every file that includes them, so one root cause shows up as one row even
# when the failure *message* differs.  This view found "copies the finalizer"
# (8 files) and "works for eval with a given line" (3 files).
puts
puts "=" * 100
puts "# BY EXAMPLE DESCRIPTION (last 6 words) -- catches shared specs"
puts "=" * 100
db = Hash.new { |h, k| h[k] = [] }
raw.each { |b| db[b['desc'].to_s.split(/\s+/).last(6).join(' ')] << b }
db.map { |d, bs| [d, bs.map { |b| b['rel'] }.uniq, bs] }
  .select { |_, f, _| f.size >= 2 }
  .sort_by { |d, f, bs| [-f.size, d] }
  .each do |d, files, bs|
    printf("%-5d %-5d  %s\n", files.size, bs.size, d)
    files.each { |f| puts "            #{f}" }
  end

# --- Third view: bucket by the SHARED SPEC frame in the backtrace. ----------
# mspec's shared specs live in core/*/shared/*.rb, core/shared/*.rb and
# shared/*.rb, and are `it_behaves_like`d from several files.  When one is in
# the backtrace, every file that includes it fails for the SAME reason, so this
# view names the single place to fix.  (The description view misses the cases
# where the same shared spec produces different messages per includer.)
puts
puts "=" * 100
puts "# BY SHARED SPEC FILE (backtrace frame under a shared/ dir)"
puts "=" * 100
sh = Hash.new { |h, k| h[k] = [] }
raw.each do |b|
  f = b['bt'].to_s.scan(%r{spec/ruby/(\S*shared/[\w/]+\.rb):(\d+)}).first
  sh["#{f[0]}"] << b if f
end
sh.map { |d, bs| [d, bs.map { |b| b['rel'] }.uniq, bs] }
  .sort_by { |d, f, bs| [-f.size, -bs.size, d] }
  .each do |d, files, bs|
    printf("%-5d %-5d  %s\n", files.size, bs.size, d)
    files.each { |f| puts "            #{f}" }
  end
