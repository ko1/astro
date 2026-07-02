# File/Dir raise the right Errno::* (SystemCallError subclasses) + qualified names.
T = "/tmp/koruby_noexist_errtest_98765"
begin; File.read(T); rescue Errno::ENOENT => e; p [e.class.name, e.message.start_with?("No such"), e.is_a?(SystemCallError), e.is_a?(StandardError)]; end
begin; File.delete(T); rescue Errno::ENOENT => e; p e.class.name; end
begin; Dir.mkdir("/tmp"); rescue Errno::EEXIST => e; p e.class.name; end
begin; Dir.entries(T); rescue Errno::ENOENT => e; p e.class.name; end
begin; Dir.chdir(T); rescue Errno::ENOENT => e; p e.class.name; end
# anonymous class named via const in a module → qualified name
module Ns
  Anon = Class.new
  Anon2 = Class.new(StandardError)
end
p Ns::Anon.name
p Ns::Anon2.name
p Ns::Anon2.ancestors.include?(StandardError)
