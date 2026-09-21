d = File.join(ENV['TMPDIR'] || '/tmp', "kdir_eisdir_#{Process.pid}")
Dir.mkdir(d)
begin
  p (File.readlines(d) rescue [$!.class, $!.message.split(" @ ").first])
  p (IO.readlines(d) rescue [$!.class, $!.message.split(" @ ").first])
  p (File.read(d) rescue [$!.class, $!.message.split(" @ ").first])
  p (File.binread(d) rescue [$!.class, $!.message.split(" @ ").first])
  p (File.foreach(d) { } rescue [$!.class, $!.message.split(" @ ").first])
  p (File.new(d).gets rescue [$!.class, $!.message.split(" @ ").first])
  p (File.open(d) { |f| f.read } rescue [$!.class, $!.message.split(" @ ").first])
  # the label and path still name the stream
  p (File.readlines(d) rescue $!.message.include?(d))
  p (File.new(d).gets rescue $!.message.include?(d))
ensure
  Dir.rmdir(d)
end
