module M; end
M.const_set(:FOO, 1)
p M.const_source_location(:FOO)
p Object.const_source_location(:M)
