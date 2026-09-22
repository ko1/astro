# net/https has been merged into net/http; the file remains so that code written
# before that still loads.  CRuby's is the same two requires.
require_relative "http"
require "openssl"
