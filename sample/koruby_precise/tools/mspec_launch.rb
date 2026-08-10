# Run one rubyspec file through mspec's own runner, at top level.
#
# Why not just `koruby some_spec.rb`?  ruby/spec's spec_helper self-runs
# (`ARGV.unshift $0; MSpecRun.main`) when MSPEC_RUNNER is unset.  That reenters
# the spec file from *inside* the require chain, so any intermediate
# spec_helper that requires a library after `require_relative '../../spec_helper'`
# has not run that require yet when the examples execute.  library/socket/
# spec_helper.rb is exactly that shape, so every socket spec sees an
# uninitialized Socket/Addrinfo.  CRuby fails the same way in that mode --
# it is the harness that is wrong, not the interpreter.
#
# Driving MSpecRun ourselves keeps the spec file at top level, so its
# require_relative chain completes before any example runs.
ENV['MSPEC_RUNNER'] = '1'
$LOAD_PATH.unshift(ENV['MSPEC_LIB'] || "#{ENV['HOME']}/ruby/src/master/spec/mspec/lib")
require 'mspec'
require 'mspec/commands/mspec-run'

MSpecScript.child_process = true
script = MSpecRun.new
script.load_default
script.options
script.signals
script.register
script.setup_env
MSpec.register_tags_patterns script.config[:tags_patterns]
MSpec.register_files script.instance_variable_get(:@files)
MSpec.process
exit MSpec.exit_code
