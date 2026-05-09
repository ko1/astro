#!/usr/bin/env ruby
# Download Google's CEL conformance suite (cel-spec/tests/simple/testdata)
# into this directory.  Uses api.github.com (sandboxed network whitelist)
# and base64-decodes the `content` field of the contents API.
#
# Usage:  ruby fetch.rb            # only fetch missing files
#         ruby fetch.rb --force    # re-download everything
#
# No GitHub auth needed (anonymous rate limit = 60 req/hr, suite has ~31 files).

require 'json'
require 'base64'
require 'net/http'
require 'uri'
require 'fileutils'

REPO  = 'google/cel-spec'
PATH  = 'tests/simple/testdata'
HERE  = File.expand_path(__dir__)
FORCE = ARGV.include?('--force')

def gh_get(path)
  uri = URI("https://api.github.com/repos/#{REPO}/#{path}")
  req = Net::HTTP::Get.new(uri)
  req['Accept']     = 'application/vnd.github+json'
  req['User-Agent'] = 'arcel-fetch'
  res = Net::HTTP.start(uri.hostname, uri.port, use_ssl: true) { |h| h.request(req) }
  unless res.is_a?(Net::HTTPSuccess)
    abort "GET #{uri}: #{res.code} #{res.message}\n#{res.body[0, 400]}"
  end
  JSON.parse(res.body)
end

FileUtils.mkdir_p(HERE)

puts "Listing #{REPO}/#{PATH} ..."
listing = gh_get("contents/#{PATH}")
files   = listing.select { |e| e['name'].end_with?('.textproto') }
puts "  #{files.size} .textproto files"

files.each_with_index do |entry, i|
  name = entry['name']
  out  = File.join(HERE, name)
  if File.exist?(out) && !FORCE
    printf "[%2d/%2d] %-30s skip (exists)\n", i + 1, files.size, name
    next
  end

  blob = gh_get("contents/#{PATH}/#{name}")
  raw  = Base64.decode64(blob['content'])
  if blob['size'] && raw.bytesize != blob['size']
    abort "size mismatch for #{name}: got #{raw.bytesize}, expected #{blob['size']}"
  end
  File.binwrite(out, raw)
  printf "[%2d/%2d] %-30s %d bytes\n", i + 1, files.size, name, raw.bytesize
end

# Pin the upstream commit so reruns are reproducible / so we can tell
# when the suite drifts.
ref = gh_get("commits/master").dig('sha')
File.write(File.join(HERE, 'UPSTREAM.sha'), "#{ref}\n")
puts "upstream commit: #{ref}"
