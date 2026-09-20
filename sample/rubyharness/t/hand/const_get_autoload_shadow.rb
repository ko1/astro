require "tmpdir"
# Module#const_get は、同名の top-level 定数に落ちる前に、レシーバに登録された
# autoload を発火する (kramdown の Kramdown::Converter.const_get(:Kramdown))。
dir = File.join(Dir.tmpdir, "korb_cga_#{Process.pid}")
Dir.mkdir(dir)
File.write(File.join(dir, "cga_inner.rb"), "module Kdn; module Conv; class Kdn; def self.go = :inner; end; end; end\n")
$LOAD_PATH.unshift dir
module Kdn
  module Conv
    autoload :Kdn, "cga_inner"
  end
end
p Kdn::Conv.const_get(:Kdn).go
p Kdn::Conv.const_get(:Kdn).equal?(Kdn)
p Kdn::Conv::Kdn.go
# astrogre: ネストしたクラスの後ろの `-` はリテラル (kramdown の AUTOLINK_START)
%w([[:alpha:]-z] [\d-x] [[:alpha:]-] [[[:alnum:]]-_.] [[a-c]-z]).each do |t|
  begin
    r = Regexp.new(t)
    p [t, :ok, !r.match("a").nil?, !r.match("-").nil?]
  rescue RegexpError => e
    p [t, :err]
  end
end
File.unlink(File.join(dir, "cga_inner.rb"))
Dir.rmdir(dir)
