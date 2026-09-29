# CRuby の default gem "prism" と同じ入口。
#
# koruby は prism の C 拡張の代わりに lib/prism/prism.rb (builtin __prism_dump)
# を使い、Ruby 側一式は同梱の prism submodule (prism/lib) から読む。この 2 つは
# シリアライズ形式を共有するのでバージョンが揃っていなければならない。
#
# この 1 枚が $LOAD_PATH 上で先に見つかることで、rubygems が別バージョンの
# prism gem を activate して serialize.rb だけ差し替わる事故を防ぐ
# (版の違う dump を Loader が読んで "Invalid serialization")。
require_relative "../prism/lib/prism"
