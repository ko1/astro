# frozen_string_literal: true
# erb/escape is a C extension in CRuby; koruby's html_escape is a builtin, so
# its source_location is nil there too (erb's test_html_escape_extension).
class ERB
  module Escape
    define_method(:html_escape, ::Kernel.instance_method(:__erb_html_escape))
    module_function :html_escape
  end
end
