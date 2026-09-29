# frozen_string_literal: true
# koruby's stand-in for stackprof's C extension (as the gem does on TruffleRuby): the
# sampler needs CRuby's rb_profile_frames / postponed jobs, which koruby has no
# counterpart of.  Requiring stackprof works (a Gemfile may list it, railsbench does);
# profiling raises.
module StackProf
  class << self
    def running? = false
    def run(*) = unimplemented
    def start(*) = unimplemented
    def stop = unimplemented
    def results(*) = unimplemented
    def sample = unimplemented
    def use_postponed_job! = nil

    private def unimplemented
      raise NotImplementedError, "StackProf needs CRuby's sampling API (rb_profile_frames); koruby has none — profile with perf"
    end
  end
end
