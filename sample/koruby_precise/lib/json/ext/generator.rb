# frozen_string_literal: true

# Pure-Ruby port of json 3.0.2's ext/json/ext/generator/generator.c (and of
# vendor/fpconv.c for Float formatting) for koruby, which has no C
# extensions.  It defines the same JSON::Ext::Generator::State API as the C
# extension, so json/common.rb and json/ext/generator/state.rb work unchanged
# on top of it.
#
# Configuration lives in instance variables; nil means "C default" so that
# State.allocate behaves like the C allocator.  json/ext/generator/state.rb
# is patched to leave these out of State#to_h.
module JSON
  module Ext
    module Generator
      # Float -> JSON text exactly like fpconv_dtoa() (Grisu2, not always the
      # shortest representation, so Float#to_s cannot be used).
      module FPConv # :nodoc:
        M64 = (1 << 64) - 1
        HIDDEN = 1 << 52
        POWERS_TEN = [
          [18054884314459144840, -1220], [13451937075301367670, -1193],
          [10022474136428063862, -1166], [14934650266808366570, -1140],
          [11127181549972568877, -1113], [16580792590934885855, -1087],
          [12353653155963782858, -1060], [18408377700990114895, -1034],
          [13715310171984221708, -1007], [10218702384817765436, -980],
          [15227053142812498563, -954],  [11345038669416679861, -927],
          [16905424996341287883, -901],  [12595523146049147757, -874],
          [9384396036005875287,  -847],  [13983839803942852151, -821],
          [10418772551374772303, -794],  [15525180923007089351, -768],
          [11567161174868858868, -741],  [17236413322193710309, -715],
          [12842128665889583758, -688],  [9568131466127621947,  -661],
          [14257626930069360058, -635],  [10622759856335341974, -608],
          [15829145694278690180, -582],  [11793632577567316726, -555],
          [17573882009934360870, -529],  [13093562431584567480, -502],
          [9755464219737475723,  -475],  [14536774485912137811, -449],
          [10830740992659433045, -422],  [16139061738043178685, -396],
          [12024538023802026127, -369],  [17917957937422433684, -343],
          [13349918974505688015, -316],  [9946464728195732843,  -289],
          [14821387422376473014, -263],  [11042794154864902060, -236],
          [16455045573212060422, -210],  [12259964326927110867, -183],
          [18268770466636286478, -157],  [13611294676837538539, -130],
          [10141204801825835212, -103],  [15111572745182864684, -77],
          [11258999068426240000, -50],   [16777216000000000000, -24],
          [12500000000000000000,   3],   [9313225746154785156,   30],
          [13877787807814456755,  56],   [10339757656912845936,  83],
          [15407439555097886824, 109],   [11479437019748901445, 136],
          [17105694144590052135, 162],   [12744735289059618216, 189],
          [9495567745759798747,  216],   [14149498560666738074, 242],
          [10542197943230523224, 269],   [15709099088952724970, 295],
          [11704190886730495818, 322],   [17440603504673385349, 348],
          [12994262207056124023, 375],   [9681479787123295682,  402],
          [14426529090290212157, 428],   [10748601772107342003, 455],
          [16016664761464807395, 481],   [11933345169920330789, 508],
          [17782069995880619868, 534],   [13248674568444952270, 561],
          [9871031767461413346,  588],   [14708983551653345445, 614],
          [10959046745042015199, 641],   [16330252207878254650, 667],
          [12166986024289022870, 694],   [18130221999122236476, 720],
          [13508068024458167312, 747],   [10064294952495520794, 774],
          [14996968138956309548, 800],   [11173611982879273257, 827],
          [16649979327439178909, 853],   [12405201291620119593, 880],
          [9242595204427927429,  907],   [13772540099066387757, 933],
          [10261342003245940623, 960],   [15290591125556738113, 986],
          [11392378155556871081, 1013],  [16975966327722178521, 1039],
          [12648080533535911531, 1066]
        ].each(&:freeze).freeze
        TENS = [
          10000000000000000000, 1000000000000000000, 100000000000000000,
          10000000000000000, 1000000000000000, 100000000000000,
          10000000000000, 1000000000000, 100000000000,
          10000000000, 1000000000, 100000000,
          10000000, 1000000, 100000,
          10000, 1000, 100,
          10, 1
        ].freeze

        # +d+ is finite.
        def self.dtoa(d)
          neg = d < 0.0 || (d == 0.0 && 1.0 / d < 0.0)
          return(neg ? '-0.0' : '0.0') if d == 0.0
          d = -d if neg

          # build_fp(): d == frac * 2**exp with the raw IEEE mantissa/exponent
          if d >= 2.2250738585072014e-308
            m, e = Math.frexp(d)
            frac = Math.ldexp(m, 53).to_i
            exp = e - 53
          else
            frac = Math.ldexp(d, 1074).to_i
            exp = -1074
          end

          # get_normalized_boundaries()
          ufrac = (frac << 1) + 1
          uexp = exp - 1
          while (ufrac & (HIDDEN << 1)) == 0
            ufrac <<= 1
            uexp -= 1
          end
          ufrac <<= 10
          uexp -= 10
          lshift = frac == HIDDEN ? 2 : 1
          lfrac = ((frac << lshift) - 1) << ((exp - lshift) - uexp)

          # normalize()
          while (frac & HIDDEN) == 0
            frac <<= 1
            exp -= 1
          end
          frac <<= 11
          exp -= 11

          # find_cachedpow10()
          approx = (-(uexp + 87) * 0.30102999566398114).to_i
          q = approx + 348
          idx = q >= 0 ? q / 8 : -(-q / 8)
          while true
            current = uexp + POWERS_TEN[idx][1] + 64
            if current < -60
              idx += 1
            elsif current > -32
              idx -= 1
            else
              break
            end
          end
          k = -348 + idx * 8
          cfrac, cexp = POWERS_TEN[idx]

          # multiply(): upper 64 bits of the 128-bit product, rounded
          wfrac = (frac * cfrac + 0x8000000000000000) >> 64
          ufrac = ((ufrac * cfrac + 0x8000000000000000) >> 64) - 1
          lfrac = ((lfrac * cfrac + 0x8000000000000000) >> 64) + 1
          uexp = uexp + cexp + 64

          digits = +''
          kk = generate_digits(digits, wfrac, ufrac, lfrac, uexp, -k)
          str = emit_digits(digits, kk, neg)
          neg ? "-#{str}" : str
        end

        # Appends the digits to +digits+ and returns the decimal exponent K.
        # Values that cannot exceed 64 bits skip the & M64 of the C uint64_t
        # arithmetic (bignum ops are costly): part2 < 2**shift <= 2**60, and
        # delta / wfrac * unit stay below 2**64 until the loop terminates.
        def self.generate_digits(digits, frac, ufrac, lfrac, uexp, kk)
          wfrac = ufrac - frac
          delta = ufrac - lfrac
          shift = -uexp
          one = 1 << shift
          part1 = ufrac >> shift
          part2 = ufrac & (one - 1)
          if delta < one
            # Closed form of the integer-digit loop below: with delta < 2**shift
            # it can only stop once the remaining part1 is 0, i.e. after the
            # last non-zero digit of part1 (part1 < 2**32, so <= 10 digits).
            str = part1.to_s
            if part2 <= delta
              n = str.bytesize
              n -= 1 while str.getbyte(n - 1) == 48
              tz = str.bytesize - n
              digits << str.byteslice(0, n)
              round_digit(digits, delta, part2, (TENS[19 - tz] << shift) & M64, wfrac)
              return kk + tz
            end
            digits << str
            kappa = 0
          else
            kappa = 10
            i = 10
            while kappa > 0
              div = TENS[i]
              digit = part1 / div
              digits << (48 + digit) if digit != 0 || !digits.empty?
              part1 -= digit * div
              kappa -= 1
              tmp = (part1 << shift) + part2
              if tmp <= delta
                round_digit(digits, delta, tmp, (div << shift) & M64, wfrac)
                return kk + kappa
              end
              i += 1
            end
          end

          unit = 18
          mask = one - 1
          while true
            part2 *= 10
            delta *= 10
            kappa -= 1
            digit = part2 >> shift
            digits << (48 + digit) if digit != 0 || !digits.empty?
            part2 &= mask
            if part2 < delta
              round_digit(digits, delta, part2, one, wfrac * TENS[unit])
              return kk + kappa
            end
            unit -= 1
          end
        end

        def self.round_digit(digits, delta, rem, kappa, frac)
          last = digits.bytesize - 1
          while rem < frac && delta - rem >= kappa &&
                (((rem + kappa) & M64) < frac || frac - rem > ((rem + kappa - frac) & M64))
            digits.setbyte(last, digits.getbyte(last) - 1)
            rem += kappa
          end
        end

        def self.emit_digits(digits, k, neg)
          ndigits = digits.bytesize
          exp = (k + ndigits - 1).abs

          if k >= 0 && exp < 15
            return "#{digits}#{'0' * k}.0"
          end

          if k < 0 && (k > -7 || exp < 10)
            offset = ndigits - k.abs
            if offset <= 0
              return "0.#{'0' * -offset}#{digits}"
            else
              return "#{digits.byteslice(0, offset)}.#{digits.byteslice(offset, ndigits - offset)}"
            end
          end

          max = neg ? 17 : 18
          ndigits = max if ndigits > max
          sign = k + ndigits - 1 < 0 ? '-' : '+'
          if ndigits > 1
            "#{digits.byteslice(0, 1)}.#{digits.byteslice(1, ndigits - 1)}e#{sign}#{exp}"
          else
            "#{digits.byteslice(0, 1)}e#{sign}#{exp}"
          end
        end
      end

      class State
        INTERNAL_IVARS = %i[
          @indent @space @space_before @object_nl @array_nl @as_json @max_nesting @depth
          @buffer_initial_length @allow_duplicate_key @allow_nan @ascii_only
          @script_safe @strict @sort_keys
        ].freeze # :nodoc:

        EMPTY = ''.freeze
        private_constant :EMPTY

        class << self
          def from_state(opts)
            if opts.is_a?(self)
              opts
            elsif opts.is_a?(::Hash)
              new(opts)
            else
              State.new
            end
          end

          def default_sort_keys_proc=(proc)
            raise TypeError, 'sort_key_proc must be a Proc' unless Proc === proc
            @default_sort_keys_proc = proc
          end

          def generate(obj, opts, io)
            state = State.allocate
            state.__send__(:configure_state, opts)
            Gen.new(state).run(obj, io, true)
          end

          def _generate_no_fallback(obj, opts, io)
            state = State.allocate
            state.__send__(:configure_state, opts)
            Gen.new(state).run(obj, io, false)
          end
        end

        def indent = @indent || EMPTY
        def space = @space || EMPTY
        def space_before = @space_before || EMPTY
        def object_nl = @object_nl || EMPTY
        def array_nl = @array_nl || EMPTY
        def indent=(v); __check_frozen; @indent = State.string_config(v); nil; end
        def space=(v); __check_frozen; @space = State.string_config(v); nil; end
        def space_before=(v); __check_frozen; @space_before = State.string_config(v); nil; end
        def object_nl=(v); __check_frozen; @object_nl = State.string_config(v); nil; end
        def array_nl=(v); __check_frozen; @array_nl = State.string_config(v); nil; end

        def as_json = @as_json || false
        def as_json=(v); __check_frozen; @as_json = State.proc_config(v); nil; end

        def max_nesting = @max_nesting.nil? ? 100 : @max_nesting
        def max_nesting=(v); __check_frozen; @max_nesting = State.long_config(v); nil; end
        def check_circular? = max_nesting != 0

        def script_safe = @script_safe ? true : false
        alias_method :script_safe?, :script_safe
        def script_safe=(v); __check_frozen; @script_safe = v ? true : false; nil; end
        def strict = @strict ? true : false
        alias_method :strict?, :strict
        def strict=(v); __check_frozen; @strict = v ? true : false; nil; end
        def allow_nan? = @allow_nan ? true : false
        def allow_nan=(v); __check_frozen; @allow_nan = v ? true : false; nil; end
        def ascii_only? = @ascii_only ? true : false
        def ascii_only=(v); __check_frozen; @ascii_only = v ? true : false; nil; end

        def depth = @depth || 0
        def depth=(v); __check_frozen; @depth = State.depth_config(v); nil; end

        def buffer_initial_length = @buffer_initial_length || 1024
        def buffer_initial_length=(v); __check_frozen; State.buffer_initial_length_config(self, v); nil; end

        def sort_keys = @sort_keys || false
        def sort_keys=(v); __check_frozen; @sort_keys = State.normalize_sort_keys(v); nil; end

        def generate(obj, io = nil)
          Gen.new(self).run(obj, io, true)
        end

        def _generate_no_fallback(obj, io = nil)
          Gen.new(self).run(obj, io, false)
        end

        private

        def allow_duplicate_key? = @allow_duplicate_key ? true : false

        def __check_frozen
          raise FrozenError.new("can't modify frozen #{self.class}: #{inspect}", receiver: self) if frozen?
        end

        def _configure(opts)
          __check_frozen
          configure_state(opts)
          self
        end

        def configure_state(config)
          return unless config
          unless ::Hash === config
            raise TypeError, "wrong argument type #{config.class} (expected Hash)"
          end
          return if config.empty?

          unknown = nil
          config.each do |key, val|
            case key
            when :indent then @indent = State.string_config(val)
            when :space then @space = State.string_config(val)
            when :space_before then @space_before = State.string_config(val)
            when :object_nl then @object_nl = State.string_config(val)
            when :array_nl then @array_nl = State.string_config(val)
            when :max_nesting then @max_nesting = State.long_config(val)
            when :allow_nan then @allow_nan = val ? true : false
            when :ascii_only then @ascii_only = val ? true : false
            when :depth then @depth = State.depth_config(val)
            when :buffer_initial_length then State.buffer_initial_length_config(self, val)
            when :script_safe then @script_safe = val ? true : false
            when :strict then @strict = val ? true : false
            when :allow_duplicate_key then @allow_duplicate_key = val ? true : false
            when :as_json then @as_json = val ? State.proc_config(val) : false
            when :sort_keys then @sort_keys = State.normalize_sort_keys(val)
            else (unknown ||= []) << key
            end
          end

          if unknown
            if unknown.size == 1
              raise ArgumentError, "unknown keyword: #{unknown[0]}"
            else
              raise ArgumentError, "unknown keywords: #{unknown.join(', ')}"
            end
          end
        end

        class << self
          def string_config(v) # :nodoc:
            return false unless v
            raise TypeError, "wrong argument type #{v.class} (expected String)" unless ::String === v
            v.empty? ? false : (v.frozen? ? v : v.dup.freeze)
          end

          def proc_config(v) # :nodoc:
            return v if Proc === v || Method === v
            unless v.respond_to?(:to_proc)
              raise TypeError, "no implicit conversion of #{type_name(v)} into Proc"
            end
            p = v.to_proc
            raise TypeError, "can't convert #{v.class} to Proc (#{v.class}#to_proc gives #{p.class})" unless Proc === p
            p
          end

          # NUM2LONG()
          def to_long(v) # :nodoc:
            case v
            when Integer then v
            when Float then v.to_i
            else
              raise TypeError, "no implicit conversion of #{type_name(v)} into Integer" unless v.respond_to?(:to_int)
              v.to_int
            end
          end

          def long_config(v) # :nodoc:
            v ? to_long(v) : 0
          end

          def depth_config(v) # :nodoc:
            return 0 unless v
            d = to_long(v)
            raise ArgumentError, "depth must be >= 0 (got #{d})" if d < 0
            raise ArgumentError, "depth is too large (got #{d})" if d > 2147483647
            d
          end

          def buffer_initial_length_config(state, v) # :nodoc:
            unless Integer === v
              raise TypeError, "wrong argument type #{v.nil? ? 'nil' : v.class} (expected Integer)"
            end
            state.instance_variable_set(:@buffer_initial_length, v) if v > 0
          end

          def normalize_sort_keys(v) # :nodoc:
            if Proc === v
              v
            elsif v == true
              State.instance_variable_get(:@default_sort_keys_proc)
            elsif v
              raise TypeError, 'The `sort_keys` argument must be a boolean or a Proc'
            else
              false
            end
          end
        end

        # One generate() call: parser.c's generate_json_data.
        class Gen # :nodoc:
          UTF_8 = ::Encoding::UTF_8
          US_ASCII = ::Encoding::US_ASCII
          BINARY = ::Encoding::BINARY
          BASIC_ESCAPE = /["\\\x00-\x1f]/
          SCRIPT_SAFE_ESCAPE = /[\/"\\\x00-\x1f  ]/
          ASCII_ONLY_ESCAPE = /[^\x20-\x21\x23-\x5b\x5d-\x7e]/
          ASCII_SCRIPT_SAFE_ESCAPE = /[^\x20-\x21\x23-\x2e\x30-\x5b\x5d-\x7e]/
          MAP = {
            '"' => '\\"', '\\' => '\\\\', '/' => '\\/',
            "\b" => '\\b', "\f" => '\\f', "\n" => '\\n', "\r" => '\\r', "\t" => '\\t',
            " " => '\\u2028', " " => '\\u2029',
          }.tap { |m| 0x20.times { |i| m[i.chr] ||= format('\\u%04x', i).freeze } }.freeze

          def initialize(state)
            @state = state
            @indent = state.instance_variable_get(:@indent)
            @space = state.instance_variable_get(:@space)
            @space_before = state.instance_variable_get(:@space_before)
            @object_nl = state.instance_variable_get(:@object_nl)
            @array_nl = state.instance_variable_get(:@array_nl)
            @as_json = state.instance_variable_get(:@as_json)
            mn = state.instance_variable_get(:@max_nesting)
            @max_nesting = mn.nil? ? 100 : mn
            @depth = state.instance_variable_get(:@depth) || 0
            @allow_duplicate_key = state.instance_variable_get(:@allow_duplicate_key)
            @allow_nan = state.instance_variable_get(:@allow_nan)
            @ascii_only = state.instance_variable_get(:@ascii_only)
            @script_safe = state.instance_variable_get(:@script_safe)
            @strict = state.instance_variable_get(:@strict)
            @sort_keys = state.instance_variable_get(:@sort_keys)
            @escape =
              if @ascii_only
                @script_safe ? ASCII_SCRIPT_SAFE_ESCAPE : ASCII_ONLY_ESCAPE
              else
                @script_safe ? SCRIPT_SAFE_ESCAPE : BASIC_ESCAPE
              end
            @vstate = nil
          end

          def run(obj, io, fallback)
            buf = String.new(encoding: UTF_8, capacity: 1024)
            gen(buf, obj, fallback)
            if io
              io.write(buf)
              io.flush
              io
            else
              buf
            end
          end

          private

          # generate_json_general(); +as_json_called+ is set once the object
          # came back from the as_json callback.
          def gen(buf, obj, fallback, as_json_called = false)
            while true
              if nil.equal?(obj)
                buf << 'null'
              elsif false.equal?(obj)
                buf << 'false'
              elsif true.equal?(obj)
                buf << 'true'
              elsif ::Integer === obj
                s = obj.to_s
                raise TypeError, "no implicit conversion of #{State.__send__(:type_name, s)} into String" unless ::String === s
                buf << s
              elsif ::String === obj
                return general(buf, obj, fallback, as_json_called) if fallback && !obj.instance_of?(::String)
                if valid_string?(obj)
                  emit_string(buf, obj)
                elsif as_json_called
                  generator_error(obj, 'source sequence is illegal/malformed utf-8')
                else
                  obj = ensure_valid_encoding(obj, false, false)
                  as_json_called = true
                  next
                end
              elsif ::Hash === obj
                return general(buf, obj, fallback, as_json_called) if fallback && !obj.instance_of?(::Hash)
                object(buf, obj)
              elsif ::Array === obj
                return general(buf, obj, fallback, as_json_called) if fallback && !obj.instance_of?(::Array)
                array(buf, obj)
              elsif ::Float === obj
                float(buf, obj)
              elsif ::Symbol === obj
                if @strict
                  obj = obj.name
                  next
                end
                fallback_gen(buf, obj)
              elsif Fragment === obj && obj.instance_of?(Fragment)
                json = obj.json
                raise TypeError, "wrong argument type #{State.__send__(:type_name, json)} (expected String)" unless ::String === json
                append_raw(buf, json)
              else
                return general(buf, obj, fallback, as_json_called)
              end
              return
            end
          end

          def general(buf, obj, fallback, as_json_called)
            if @strict
              if @as_json && !as_json_called
                gen(buf, @as_json.call(obj, false), fallback, true)
              else
                generator_error(obj, "#{obj.class} not allowed in JSON")
              end
            else
              fallback_gen(buf, obj)
            end
          end

          def fallback_gen(buf, obj)
            if obj.respond_to?(:to_json)
              tmp = obj.to_json(vstate)
              raise TypeError, "wrong argument type #{State.__send__(:type_name, tmp)} (expected String)" unless ::String === tmp
              append_raw(buf, tmp)
            else
              tmp = obj.to_s
              raise TypeError, "wrong argument type #{State.__send__(:type_name, tmp)} (expected String)" unless ::String === tmp
              raw_string(buf, ensure_valid_encoding(tmp, false, false))
            end
          end

          # A State for to_json callbacks, carrying the current depth.
          def vstate
            vs = @vstate
            unless vs
              vs = @vstate = State.allocate
              @state.instance_variables.each do |iv|
                vs.instance_variable_set(iv, @state.instance_variable_get(iv)) if INTERNAL_IVARS.include?(iv)
              end
            end
            vs.instance_variable_set(:@depth, @depth)
            vs
          end

          def append_raw(buf, str)
            if str.encoding == UTF_8 || str.ascii_only?
              buf << str
            else
              buf.append_as_bytes(str)
            end
          end

          def generator_error(obj, message)
            raise GeneratorError.new(message.dup.force_encoding(UTF_8), obj)
          end

          def valid_string?(str)
            return true if str.ascii_only?
            enc = str.encoding
            (enc == UTF_8 || enc == US_ASCII) && str.valid_encoding?
          end

          # convert_invalid_encoding()
          def ensure_valid_encoding(str, as_json_called, is_key)
            return str if valid_string?(str)

            if !as_json_called && @strict && @as_json
              coerced = @as_json.call(str, false)
              unless coerced.equal?(str)
                if ::String === coerced
                  generator_error(str, 'source sequence is illegal/malformed utf-8') unless valid_string?(coerced)
                elsif is_key
                  generator_error(coerced, "#{coerced.class} not allowed as object key in JSON")
                end
                return coerced
              end
            end

            if str.encoding == BINARY
              utf8 = str.dup.force_encoding(UTF_8)
              if utf8.valid_encoding?
                unless utf8.ascii_only?
                  # rb_warn() reports the innermost Ruby-level caller.
                  up = caller_locations.index { |l| l.path != __FILE__ } || 0
                  warn('JSON.generate: UTF-8 string passed as BINARY, this will raise an encoding error in json 3.0', uplevel: up + 1)
                end
                return utf8
              end
            end

            begin
              str.encode(UTF_8)
            rescue => e
              generator_error(str, e.message)
            end
          end

          # raw_generate_json_string()
          def raw_string(buf, str)
            generator_error(str, 'source sequence is illegal/malformed utf-8') unless str.valid_encoding?
            emit_string(buf, str)
          end

          # +str+ has passed valid_string? (7-bit, or valid UTF-8/US-ASCII).
          def emit_string(buf, str)
            buf << '"'
            if @escape.match?(str)
              buf << escape(str)
            elsif str.encoding == UTF_8 || str.ascii_only?
              buf << str
            else
              buf.append_as_bytes(str)
            end
            buf << '"'
          end

          def escape(str)
            str = str.dup.force_encoding(UTF_8) unless str.encoding == UTF_8
            if @ascii_only
              str.gsub(@escape) do |c|
                if (m = MAP[c]) && (c != '/' || @script_safe)
                  m
                else
                  cp = c.ord
                  if cp > 0xFFFF
                    cp -= 0x10000
                    format('\\u%04x\\u%04x', 0xD800 + (cp >> 10), 0xDC00 + (cp & 0x3FF))
                  else
                    format('\\u%04x', cp)
                  end
                end
              end
            else
              str.gsub(@escape, MAP)
            end
          end

          def increase_depth
            depth = @depth += 1
            if @max_nesting != 0 && depth > @max_nesting
              @depth -= 1
              raise NestingError, "nesting of #{@depth} is too deep. Did you try to serialize objects with circular references?"
            end
            depth
          end

          def object(buf, obj)
            if @sort_keys
              obj = @sort_keys.call(obj)
              raise TypeError, "wrong argument type #{State.__send__(:type_name, obj)} (expected Hash)" unless ::Hash === obj
            end
            depth = increase_depth
            if obj.empty?
              buf << '{}'
              @depth -= 1
              return
            end

            buf << '{'
            object_nl = @object_nl
            indent = @indent
            first = true
            first_type = nil
            mixed = false
            obj.each_pair do |key, val|
              if first
                first = false
                first_type = key_type(key)
              else
                buf << ','
              end
              buf << object_nl if object_nl
              buf << indent * depth if indent

              if first_type == :string && key.instance_of?(::String) && valid_string?(key)
                # common case of the key dispatch below
                emit_string(buf, key)
                buf << @space_before if @space_before
                buf << ':'
                buf << @space if @space
                gen(buf, val, true)
                next
              end

              as_json_called = false
              while true
                if ::String === key
                  if first_type != :string && !mixed
                    mixed = true
                    JSON.send(:on_mixed_keys_hash, obj) unless @allow_duplicate_key
                  end
                  if key.instance_of?(::String)
                    key_str = key
                  else
                    key_str = key.to_s
                    unless ::String === key_str
                      raise TypeError, "can't convert #{key.class} to String (#{key.class}#to_s gives #{key_str.class})"
                    end
                  end
                elsif ::Symbol === key
                  if first_type != :symbol && !mixed
                    mixed = true
                    JSON.send(:on_mixed_keys_hash, obj) unless @allow_duplicate_key
                  end
                  key_str = key.name
                elsif @strict
                  if @as_json && !as_json_called
                    key = @as_json.call(key, true)
                    as_json_called = true
                    next
                  else
                    generator_error(key, "#{key.class} not allowed as object key in JSON")
                  end
                else
                  key_str = key.to_s
                  unless ::String === key_str
                    raise TypeError, "can't convert #{key.class} to String (#{key.class}#to_s gives #{key_str.class})"
                  end
                end
                break
              end

              key_str = ensure_valid_encoding(key_str, as_json_called, true)
              if key_str.instance_of?(::String)
                raw_string(buf, key_str)
              else
                gen(buf, key_str, true)
              end
              buf << @space_before if @space_before
              buf << ':'
              buf << @space if @space
              gen(buf, val, true)
            end

            depth = @depth -= 1
            if object_nl
              buf << object_nl
              buf << indent * depth if indent
            end
            buf << '}'
          end

          def key_type(key)
            if ::String === key
              :string
            elsif ::Symbol === key
              :symbol
            else
              :other
            end
          end

          def array(buf, obj)
            depth = increase_depth
            if obj.empty?
              buf << '[]'
              @depth -= 1
              return
            end

            buf << '['
            array_nl = @array_nl
            indent = @indent
            buf << array_nl if array_nl
            i = 0
            n = obj.size
            while i < n
              if i > 0
                buf << ','
                buf << array_nl if array_nl
              end
              buf << indent * depth if indent
              gen(buf, obj[i], true)
              i += 1
              n = obj.size
            end
            depth = @depth -= 1
            if array_nl
              buf << array_nl
              buf << indent * depth if indent
            end
            buf << ']'
          end

          def float(buf, obj)
            if obj.nan? || obj.infinite?
              unless @allow_nan
                if @strict && @as_json
                  casted = @as_json.call(obj, false)
                  unless casted.equal?(obj)
                    increase_depth
                    gen(buf, casted, true)
                    @depth -= 1
                    return
                  end
                end
                generator_error(obj, "#{obj} not allowed in JSON")
              end
              buf << obj.to_s
            else
              buf << FPConv.dtoa(obj)
            end
          end
        end

        def self.type_name(obj) # :nodoc:
          case obj
          when nil then 'nil'
          when true then 'true'
          when false then 'false'
          else obj.class.to_s
          end
        end
        private_class_method :type_name
      end
    end
  end
end

require 'json/ext/generator/state'
