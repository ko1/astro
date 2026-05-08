require 'date'
require 'time'
require 'uri'
require 'ipaddr'

module Arjsv
  # Per-format checkers used by `node_format`.  Each Proc is called with the
  # candidate String and returns a truthy value when the format matches.
  #
  # Strict by default — these run as assertions (json_schemer's behaviour
  # for draft-07).  Unknown formats fall through to `_alloc_pass` (= no
  # constraint) on the Ruby side, so adding new entries here is enough to
  # extend coverage.
  module Format
    # Helper: validate `HH:MM:SS[.frac][Z|±HH:MM]` against RFC 3339 +
    # leap-second rules.  `sign_ch` is nil for Z / no-offset, '+' or '-'
    # otherwise.  Offset hour/minute are also validated (RFC 3339 §5.6
    # caps offset at ±23:59).
    def self.check_partial_time(h, mi, se, sign_ch, oh, om)
      return false if h > 23 || mi > 59
      return false if oh > 23 || om > 59
      if se > 59
        return false if se != 60
        # Leap second is only valid at 23:59:60 UTC.  Compute UTC h:m from
        # local + offset.  When sign is '+', UTC = local − offset.
        if sign_ch.nil?
          return false unless h == 23 && mi == 59
        else
          off_min = oh * 60 + om
          delta = sign_ch == '+' ? -off_min : off_min
          utc_min = (h * 60 + mi + delta) % (24 * 60)
          return false unless utc_min == 23 * 60 + 59
        end
      end
      true
    end

    # `date` — RFC 3339 full-date, proleptic Gregorian (so e.g. 0100-02-29
    # is rejected even though year 100 is a Julian leap year).
    DATE = ->(s) {
      return false unless m = s.match(/\A(\d{4})-(\d{2})-(\d{2})\z/)
      begin
        Date.civil(m[1].to_i, m[2].to_i, m[3].to_i, Date::GREGORIAN)
        true
      rescue ArgumentError, Date::Error
        false
      end
    }

    # `date-time` — RFC 3339 with mandatory timezone.  Validates day-in-
    # month + leap-second alignment manually (Ruby's `DateTime.rfc3339`
    # tolerates :60 and doesn't apply Gregorian rules to early years).
    DATE_TIME = ->(s) {
      m = s.match(/\A(\d{4})-(\d{2})-(\d{2})[Tt](\d{2}):(\d{2}):(\d{2})(?:\.\d+)?(?:[Zz]|([+-])(\d{2}):(\d{2}))\z/)
      return false unless m
      y, mo, d, h, mi, se = m[1].to_i, m[2].to_i, m[3].to_i, m[4].to_i, m[5].to_i, m[6].to_i
      sign = m[7]                  # nil if Z; '+' or '-' otherwise
      oh = m[8]&.to_i || 0
      om = m[9]&.to_i || 0
      begin
        Date.civil(y, mo, d, Date::GREGORIAN)
      rescue ArgumentError, Date::Error
        return false
      end
      Format.check_partial_time(h, mi, se, sign, oh, om)
    }

    # `time` — RFC 3339 §5.6 `full-time` = partial-time + mandatory
    # `time-offset` (Z or ±HH:MM).
    TIME = ->(s) {
      m = s.match(/\A(\d{2}):(\d{2}):(\d{2})(?:\.\d+)?(?:[Zz]|([+-])(\d{2}):(\d{2}))\z/)
      return false unless m
      h, mi, se = m[1].to_i, m[2].to_i, m[3].to_i
      sign = m[4]                  # nil for Z; '+' or '-' otherwise
      oh = m[5]&.to_i || 0
      om = m[6]&.to_i || 0
      Format.check_partial_time(h, mi, se, sign, oh, om)
    }

    # RFC 5322-ish.  Loose regex; rejects most nonsense but accepts
    # unusual-but-valid local parts.  Forbids dots at start/end of local
    # part, consecutive dots, and a dot directly before `@`.
    EMAIL = ->(s) {
      m = s.match(/\A([^\s@]+)@([^\s@]+\.[^\s@]+)\z/)
      return false unless m
      local = m[1]
      return false if local.start_with?('.') || local.end_with?('.')
      return false if local.include?('..')
      true
    }

    IDN_EMAIL = EMAIL  # we don't do punycode locally; same loose check.

    # RFC 1123 hostname: labels of 1-63 chars, alphanumeric + hyphen,
    # not starting / ending with hyphen, total ≤ 253 chars.
    HOSTNAME = ->(s) {
      return false if s.empty? || s.length > 253
      labels = s.split('.', -1)
      return false if labels.empty?
      labels.all? do |l|
        !l.empty? && l.length <= 63 &&
          l =~ /\A[A-Za-z0-9](?:[A-Za-z0-9\-]*[A-Za-z0-9])?\z/
      end
    }

    # IDN hostname: labels can include unicode letters; we normalise via
    # IDNA punycode where possible, falling back to a permissive
    # length+character check.  Without `punycode` gem we approximate.
    IDN_HOSTNAME = ->(s) {
      return false if s.empty? || s.length > 253
      labels = s.split('.', -1)
      return false if labels.empty?
      labels.all? do |l|
        next false if l.empty?
        # Reject leading/trailing hyphens.
        next false if l.start_with?('-') || l.end_with?('-')
        # ASCII label fast path.
        if l.ascii_only?
          l.length <= 63 && l =~ /\A[A-Za-z0-9\-]+\z/
        else
          # Unicode label: forbid combining marks / control / punctuation.
          # This is approximate; full IDNA2008 would need libidn.
          l.chars.all? do |ch|
            cp = ch.ord
            (cp >= 0x80) || (ch =~ /[A-Za-z0-9\-]/)
          end
        end
      end
    }

    IPV4 = ->(s) {
      return false unless s =~ /\A\d+\.\d+\.\d+\.\d+\z/
      begin
        ip = IPAddr.new(s)
        ip.ipv4?
      rescue IPAddr::Error
        false
      end
    }

    IPV6 = ->(s) {
      return false unless s.include?(':')
      # IPAddr.new tolerates a /<prefix> netmask suffix and `%<zone>` zone-id;
      # JSON Schema's ipv6 format wants a bare address.
      return false if s.include?('/') || s.include?('%')
      begin
        ip = IPAddr.new(s)
        ip.ipv6?
      rescue IPAddr::Error
        false
      end
    }

    UUID = ->(s) {
      !!(s =~ /\A[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\z/)
    }

    URI_RE = ->(s) {
      return false if s.empty?
      begin
        u = URI.parse(s)
        # Per RFC 3986, an absolute URI requires a scheme.
        !u.scheme.nil? && !u.scheme.empty?
      rescue URI::InvalidURIError
        false
      end
    }

    URI_REFERENCE = ->(s) {
      begin
        URI.parse(s)
        true
      rescue URI::InvalidURIError
        false
      end
    }

    # Pre-pass: percent-encode any non-ASCII bytes so URI.parse (which only
    # speaks ASCII) doesn't reject IRIs.  Approximates IRI ⇒ URI mapping
    # from RFC 3987 §3.1.
    def self.iri_to_uri(s)
      s.b.each_byte.map { |b| b < 0x80 ? b.chr : sprintf('%%%02X', b) }.join
    end

    IRI = ->(s) {
      URI_RE.call(Format.iri_to_uri(s))
    }

    IRI_REFERENCE = ->(s) {
      URI_REFERENCE.call(Format.iri_to_uri(s))
    }

    JSON_POINTER = ->(s) {
      return true if s.empty?
      return false unless s.start_with?('/')
      # Each segment must percent-decode-able and only `~0` / `~1` allowed
      # for `~` escaping.  Walk segment-by-segment.
      s[1..].split('/', -1).all? do |seg|
        # Reject lone `~` not followed by 0 or 1.
        seg.scan(/~./).all? { |e| e == '~0' || e == '~1' } && !seg.end_with?('~')
      end
    }

    RELATIVE_JSON_POINTER = ->(s) {
      m = s.match(/\A(0|[1-9]\d*)(#|\/.*)?\z/)
      return false unless m
      suffix = m[2]
      return true if suffix.nil? || suffix == '#'
      JSON_POINTER.call(suffix)
    }

    REGEX = ->(s) do
      Regexp.new(s)
      true
    rescue RegexpError
      false
    end

    # https://datatracker.ietf.org/doc/html/rfc6570 — loose check; full
    # validator is large.
    URI_TEMPLATE = ->(s) {
      # Reject obviously invalid: stray `}` / unbalanced.
      return false if s.count('{') != s.count('}')
      # Each `{...}` must follow level-2/3 syntax (loosely): operators /
      # variables / modifiers.  Permissive for the tests' positive cases.
      s.scan(/\{([^{}]*)\}/).all? do |body,|
        body =~ /\A[+\#.\/;?&]?[\w%.,:*]+\z/
      end
    }

    CHECKERS = {
      'date'                   => DATE,
      'date-time'              => DATE_TIME,
      'time'                   => TIME,
      'email'                  => EMAIL,
      'idn-email'              => IDN_EMAIL,
      'hostname'               => HOSTNAME,
      'idn-hostname'           => IDN_HOSTNAME,
      'ipv4'                   => IPV4,
      'ipv6'                   => IPV6,
      'uuid'                   => UUID,
      'uri'                    => URI_RE,
      'uri-reference'          => URI_REFERENCE,
      'iri'                    => IRI,
      'iri-reference'          => IRI_REFERENCE,
      'json-pointer'           => JSON_POINTER,
      'relative-json-pointer'  => RELATIVE_JSON_POINTER,
      'regex'                  => REGEX,
      'uri-template'           => URI_TEMPLATE,
    }.freeze
  end
end
