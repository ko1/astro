# frozen_string_literal: true
# CRuby 4.x keeps cgi/util as a shim over cgi/escape (CGI::Util itself is gone).
require "cgi/escape"
warn <<-WARNING, uplevel: 1 if $VERBOSE
CGI::Util is removed from Ruby 4.0. Please use cgi/escape instead for CGI.escape and CGI.unescape features.
If you are using CGI.parse, please install and use the cgi gem instead.
WARNING
