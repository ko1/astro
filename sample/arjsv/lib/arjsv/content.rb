require 'base64'
require 'json'

module Arjsv
  # Checkers for `contentEncoding` and `contentMediaType` (draft-07
  # assertion mode).  Returned as Procs that the runtime calls per-string.
  module ContentChecker
    BASE64 = ->(s) {
      # RFC 4648: alphabet [A-Za-z0-9+/=], length multiple of 4.
      return false unless s =~ /\A[A-Za-z0-9+\/]*={0,2}\z/
      return false unless s.length % 4 == 0
      true
    }

    def self.encoding(name)
      case name
      when 'base64' then BASE64
      else nil  # unknown encodings are annotation-only
      end
    end

    # When an encoding is set, decode through it before applying the media
    # check.  When no encoding is set, the raw string is the document.
    def self.media_type(media, encoding)
      decoder =
        case encoding
        when 'base64' then ->(s) { Base64.strict_decode64(s) rescue nil }
        when nil      then ->(s) { s }
        else nil
        end
      return nil if decoder.nil?
      checker =
        case media
        when 'application/json'
          ->(decoded) {
            return false if decoded.nil?
            begin
              JSON.parse(decoded)
              true
            rescue JSON::ParserError
              false
            end
          }
        else
          ->(_) { true }
        end
      ->(s) {
        decoded = decoder.call(s)
        decoded.nil? ? false : checker.call(decoded)
      }
    end
  end
end
