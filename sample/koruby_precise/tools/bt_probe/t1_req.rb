# require / require_relative / load put a frame of their own between the loaded
# file's '<top (required)>' and the requirer ("Kernel#require", at the CALL SITE's
# file and line).  Run under CRuby and koruby and diff.
D = __dir__

def via_require;  require File.expand_path('t1_req_inner.rb', D);  end
def via_relative; require_relative 't1_req_inner';                 end
def via_load;     load File.expand_path('t1_req_inner.rb', D);     end

$tag = 'require';          via_require
$tag = 'require_relative'; via_relative
$tag = 'load';             via_load

# through a C-built window (a block Array#each runs) and through a lambda
$tag = 'in block'; [1].each { load File.expand_path('t1_req_inner.rb', D) }
$tag = 'in lambda'; ->{ load File.expand_path('t1_req_inner.rb', D) }.call
$tag = 'via send'; send(:load, File.expand_path('t1_req_inner.rb', D))

# a #to_path argument: require converts it and must not lose the chain
class P; def initialize(s); @s = s; end; def to_path; @s; end; end
def via_to_path; require P.new(File.expand_path('t1_req_inner2.rb', D)); end
$tag = 'to_path'; via_to_path
