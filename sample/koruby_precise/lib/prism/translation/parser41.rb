# Ruby 4.1 entry point for `Prism::Translation::Parser`.
#
# The bundled prism (submodule, 1.4.1) stops at Parser35; CRuby 4.1 ships prism
# 1.9, which has these.  `version` only picks the syntax level handed to
# Prism.parse, and prism maps anything it does not know to "latest" — which is
# exactly what koruby's own parser accepts, so the two agree.
# Remove once the submodule is updated (ideas: koruby-prism-submodule-upgrade).
require "prism/translation/parser"

module Prism
  module Translation
    class Parser41 < Parser
      def version = 41
    end
  end
end
