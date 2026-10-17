# frozen_string_literal: true
# koruby's rubygems defaults (the packager hook rubygems.rb loads after its own
# defaults).  koruby runs CRuby's rubygems unchanged; only the places where koruby
# is not CRuby are adjusted here.

module Gem
  # replaced below; removed first so `ruby -W` does not report the redefinitions
  class << self
    remove_method :default_bindir, :user_dir, :operating_system_defaults
  end

  # Executables go to the install root's bin/, not beside the koruby binary
  # (RbConfig bindir is the running binary's directory).  A bindir someone set
  # afterwards (rubygems' own tests do) is followed, as CRuby does.
  KORB_EXE_BINDIR = RbConfig::CONFIG["bindir"].dup.freeze
  private_constant :KORB_EXE_BINDIR
  def self.default_bindir
    b = RbConfig::CONFIG["bindir"]
    b == KORB_EXE_BINDIR ? File.join(RbConfig::CONFIG["prefix"], "bin") : b
  end

  # Gems a user installs with koruby stay apart from CRuby's (~/.gem/ruby/...):
  # koruby builds C extensions as .kompat.so (or leaves them out), so the two installs differ.
  def self.user_dir
    File.join(Gem.user_home, ".gem", "koruby", RbConfig::CONFIG["ruby_version"])
  end

  # rdoc (ri generation) requires rbs, whose C extension koruby cannot load
  def self.operating_system_defaults
    { "gem" => "--no-document" }
  end

  # C extensions are built with mkmf as foo.kompat.so, for koruby's compatibility
  # layer (the .so CRuby builds is never loaded).  One that does not build is left
  # out instead of failing the install: the gem's pure-Ruby parts work, koruby's own
  # lib/ stands in where it has one, and a `require` of the rest is a LoadError.
  # The build-complete marker is still written: rubygems and bundler would otherwise
  # ignore the gem as one whose extensions are missing.
  module KorubyExtensionFallback
    # extensions that build but misbehave on koruby (docs/todo.md): left out on purpose
    KNOWN_BAD = {
      "date" => "Date.parse reads captures after mutating the matched string; koruby's MatchData follows the mutation",
    }.freeze

    def build_extensions
      return if @spec.extensions.empty?
      return korb_leave_out(KNOWN_BAD[@spec.name]) if KNOWN_BAD.key?(@spec.name)
      super
    rescue Gem::Ext::BuildError, Gem::InstallError => e
      korb_leave_out("did not build for koruby: #{e.message.lines.first&.chomp}")
    end

    private def korb_leave_out(reason)
      warn "koruby: left out the C extensions of #{@spec.full_name} (#{reason}); koruby's own lib/ stands in where it has one"
      # all or nothing: an extension that did build next to one that did not (json: generator yes,
      # parser no) would load against koruby's own half
      [@spec.full_gem_path, @spec.extension_dir].each do |dir|
        Dir.glob(File.join(dir, "**", "*.kompat.so")).each { |f| File.delete(f) }
      end
      marker = @spec.gem_build_complete_path
      FileUtils.mkdir_p File.dirname(marker)
      FileUtils.touch marker
    end
  end

  pre_install do |_installer|
    Gem::Ext::Builder.prepend(KorubyExtensionFallback) unless Gem::Ext::Builder < KorubyExtensionFallback
    true
  end
end
