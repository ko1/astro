# JSON::GeneratorError's message is a fresh String: vcr appends a note to it.
require 'json'
begin
  JSON.generate("\xff".b.force_encoding("UTF-8"))
rescue JSON::GeneratorError => e
  e.message << " (noted)"
  p e.message
end
