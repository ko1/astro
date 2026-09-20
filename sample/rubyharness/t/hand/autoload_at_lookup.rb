# autoload は「参照した時点」で走る。const_missing を自前で持つモジュール
# (dry-types) でも autoload が効かなければならない (CRuby は const_missing を
# 経由しない)。
require "tmpdir"
dir = File.join(Dir.tmpdir, "korb_al_#{Process.pid}")
Dir.mkdir(dir)
File.write(File.join(dir, "al_target.rb"), "module AlHost; class Thing; def hi = :hi; end; end\n")
$LOAD_PATH.unshift dir
module AlHost
  autoload :Thing, "al_target"
  def self.const_missing(n)
    :HOOK_RAN
  end
  def self.make = Thing.new
end
p AlHost.make.hi
p AlHost::Thing.new.hi
p AlHost.autoload?(:Thing)
p AlHost::Nope
File.unlink(File.join(dir, "al_target.rb"))
Dir.rmdir(dir)
