# frozen_string_literal: true
# koruby's rubygems defaults (the packager hook rubygems.rb loads after its own
# defaults).  koruby runs CRuby's rubygems unchanged; only the places where koruby
# is not CRuby are adjusted here.

module Gem
  # Executables go to the install root's bin/, not beside the koruby binary
  # (RbConfig bindir is the running binary's directory).
  def self.default_bindir
    File.join(RbConfig::CONFIG["prefix"], "bin")
  end

  # Gems a user installs with koruby stay apart from CRuby's (~/.gem/ruby/...):
  # koruby skips C extensions, so the two installs of one gem differ.
  def self.user_dir
    File.join(Gem.user_home, ".gem", "koruby", RbConfig::CONFIG["ruby_version"])
  end

  # rdoc (ri generation) requires rbs, whose C extension koruby cannot load
  def self.operating_system_defaults
    { "gem" => "--no-document" }
  end

  # koruby cannot load C extensions (.so), so a gem is installed without building
  # them; its pure-Ruby parts work, and a `require` of the extension is a LoadError.
  # The build-complete marker is still written: rubygems and bundler would otherwise
  # ignore the gem as one whose extensions are missing.
  module KorubySkipExtensions
    def build_extensions
      return if @spec.extensions.empty?
      warn "koruby: skipped building the C extensions of #{@spec.full_name} (no mkmf for koruby yet; koruby's own lib/ stands in where it has one)"
      marker = @spec.gem_build_complete_path
      FileUtils.mkdir_p File.dirname(marker)
      FileUtils.touch marker
    end
  end

  pre_install do |_installer|
    Gem::Ext::Builder.prepend(KorubySkipExtensions) unless Gem::Ext::Builder < KorubySkipExtensions
    true
  end
end
