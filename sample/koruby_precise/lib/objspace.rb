# objspace — the parts koruby can answer truthfully.  Allocation tracing is not
# implemented (the allocator has no hook, and a moving GC would need the side
# table keyed by identity), so the trace_* entry points raise NotImplementedError
# and the allocation_* queries answer nil, as CRuby does for an untraced object.

module ObjectSpace
  module_function

  def trace_object_allocations_start
    raise NotImplementedError, "ObjectSpace.trace_object_allocations_start() function is unimplemented on this machine"
  end

  def trace_object_allocations_stop
    raise NotImplementedError, "ObjectSpace.trace_object_allocations_stop() function is unimplemented on this machine"
  end

  def trace_object_allocations_clear
    raise NotImplementedError, "ObjectSpace.trace_object_allocations_clear() function is unimplemented on this machine"
  end

  def trace_object_allocations
    raise NotImplementedError, "ObjectSpace.trace_object_allocations() function is unimplemented on this machine"
  end

  def allocation_sourcefile(obj) = nil
  def allocation_sourceline(obj) = nil
  def allocation_class_path(obj) = nil
  def allocation_method_id(obj) = nil
  def allocation_generation(obj) = nil
end
