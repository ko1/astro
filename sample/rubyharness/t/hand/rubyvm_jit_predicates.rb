# RubyVM::YJIT / ::ZJIT answer their enabled? predicates (false), as CRuby without the
# JIT does; code probing RubyVM.const_defined?("YJIT") without a defined? guard works.
p RubyVM.const_defined?("YJIT"), RubyVM::YJIT.enabled?, RubyVM::ZJIT.enabled?, RubyVM::YJIT.stats_enabled?
p [["YJIT", 1], ["MJIT", 5], ["RJIT", 5]].any? { |jit, _| RubyVM.const_defined?(jit) && RubyVM.const_get(jit).respond_to?(:enabled?) && RubyVM.const_get(jit).enabled? }
