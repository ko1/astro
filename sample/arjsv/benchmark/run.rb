#!/usr/bin/env ruby
# arjsv vs json_schemer vs rj_schema (Rust-backed) benchmark.
#
# Two scenarios per schema:
#   A) "parsed Hash in"  — input is an already-parsed Ruby value.  Common in
#      Rails / rack apps where middleware has already parsed the body.
#      arjsv + json_schemer take this directly; rj_schema (which only accepts
#      JSON strings) has to re-serialise, paying that cost.
#   B) "JSON string in"  — input is a raw JSON string.  Common at API gateway
#      / streaming entry points.  arjsv + json_schemer have to parse first;
#      rj_schema's native form.

require 'benchmark/ips'
require 'json'
require 'json_schemer'
require 'rj_schema'
$LOAD_PATH.unshift File.expand_path('../lib', __dir__)
require 'arjsv'

# ---- schemas -------------------------------------------------------------

SIMPLE_INT = {
  'type' => 'integer',
  'minimum' => 0,
  'maximum' => 1000,
}.freeze

USER_OBJECT = {
  'type' => 'object',
  'required' => ['name', 'age'],
  'properties' => {
    'name'  => {'type' => 'string', 'minLength' => 1, 'maxLength' => 100},
    'age'   => {'type' => 'integer', 'minimum' => 0, 'maximum' => 200},
    'email' => {'type' => 'string', 'format' => 'email'},
    'role'  => {'enum' => ['admin', 'user', 'guest']},
  },
  'additionalProperties' => false,
}.freeze

# OpenAPI-flavoured response shape: $defs + allOf + nested arrays.
API_RESPONSE = {
  '$defs' => {
    'Address' => {
      'type' => 'object',
      'required' => ['city', 'country'],
      'properties' => {
        'city'    => {'type' => 'string', 'minLength' => 1},
        'country' => {'type' => 'string', 'pattern' => '^[A-Z]{2}$'},
        'zip'     => {'type' => 'string', 'pattern' => '^\d{3,10}$'},
      },
    },
    'User' => {
      'type' => 'object',
      'required' => ['id', 'name'],
      'properties' => {
        'id'      => {'type' => 'integer', 'minimum' => 1},
        'name'    => {'type' => 'string', 'minLength' => 1, 'maxLength' => 50},
        'email'   => {'type' => 'string', 'format' => 'email'},
        'address' => {'$ref' => '#/$defs/Address'},
        'roles'   => {'type' => 'array', 'items' => {'enum' => ['admin','user','guest']}, 'uniqueItems' => true},
      },
      'additionalProperties' => false,
    },
  },
  'type' => 'object',
  'required' => ['ok', 'data'],
  'properties' => {
    'ok'    => {'type' => 'boolean'},
    'data'  => {
      'type' => 'array',
      'items' => {'$ref' => '#/$defs/User'},
      'minItems' => 0,
      'maxItems' => 1000,
    },
    'count' => {'type' => 'integer', 'minimum' => 0},
  },
}.freeze

# ---- payloads ------------------------------------------------------------

USER_OK = {
  'name'  => 'Alice',
  'age'   => 30,
  'email' => 'alice@example.com',
  'role'  => 'admin',
}.freeze

USER_BAD = {
  'name'  => 'Bob',
  'age'   => 'thirty',
  'role'  => 'admin',
}.freeze

def make_user(i)
  {
    'id' => i + 1,
    'name' => "user#{i}",
    'email' => "u#{i}@example.com",
    'address' => {'city' => 'Tokyo', 'country' => 'JP', 'zip' => '1000001'},
    'roles' => i.even? ? ['user'] : ['user', 'admin'],
  }
end

API_OK_SMALL = {
  'ok' => true,
  'data' => Array.new(5) { |i| make_user(i) },
  'count' => 5,
}.freeze

API_OK_LARGE = {
  'ok' => true,
  'data' => Array.new(50) { |i| make_user(i) },
  'count' => 50,
}.freeze

# ---- benchmark cases -----------------------------------------------------

CASES = [
  ['simple int (valid)',         SIMPLE_INT,    42],
  ['user object (valid)',        USER_OBJECT,   USER_OK],
  ['user object (invalid)',      USER_OBJECT,   USER_BAD],
  ['api response x5  (valid)',   API_RESPONSE,  API_OK_SMALL],
  ['api response x50 (valid)',   API_RESPONSE,  API_OK_LARGE],
]

# ---- bench helpers ------------------------------------------------------

def build_arjsv(schema, compile)
  s = Arjsv.schema(schema)
  s.compile! if compile
  s
end

def build_json_schemer(schema)
  JSONSchemer.schema(schema, meta_schema: 'http://json-schema.org/draft-07/schema#')
end

# Preload the schema into a Validator (rj_schema parses + RapidJSON-compiles
# the schema once at construction time; subsequent valid? calls only parse
# the data document).
def build_rj_schema(schema)
  RjSchema::Validator.new('main' => JSON.generate(schema))
end

# ---- sanity check -------------------------------------------------------

CASES.each do |label, schema, data|
  data_json = JSON.generate(data)
  jschemer    = build_json_schemer(schema)
  ours_interp = build_arjsv(schema, false)
  ours_compiled = build_arjsv(schema, true)
  rj          = build_rj_schema(schema)

  expected = jschemer.valid?(data)
  oi = ours_interp.valid?(data)
  oc = ours_compiled.valid?(data)
  rj_result = rj.valid?(:main, data_json)
  unless oi == expected && oc == expected && rj_result == expected
    warn "MISMATCH on #{label.inspect}: js=#{expected} arjsv_i=#{oi} arjsv_c=#{oc} rj=#{rj_result}"
    exit 1
  end
end
puts "[ok] all #{CASES.size} cases agree across all 3 validators"
puts

# ---- ips ----------------------------------------------------------------

CASES.each do |label, schema, data|
  data_json = JSON.generate(data)
  jschemer  = build_json_schemer(schema)
  arjsv_int = build_arjsv(schema, false)
  arjsv_cmp = build_arjsv(schema, true)
  rj        = build_rj_schema(schema)

  puts "==== #{label} ====".ljust(60, '=')

  puts "(A) parsed Ruby value in (typical Rails / rack flow)"
  Benchmark.ips do |x|
    x.config(time: 3, warmup: 1)
    x.report('json_schemer')   { jschemer.valid?(data) }
    x.report('arjsv interp')   { arjsv_int.valid?(data) }
    x.report('arjsv AOT')      { arjsv_cmp.valid?(data) }
    # rj_schema can't take Ruby objects directly; the Hash → JSON cost is
    # part of the comparison.
    x.report('rj_schema (Hash→JSON)') { rj.valid?(:main, JSON.generate(data)) }
    x.compare!
  end

  puts
  puts "(B) JSON string in (gateway flow)"
  Benchmark.ips do |x|
    x.config(time: 3, warmup: 1)
    x.report('json_schemer (JSON.parse+v)') { jschemer.valid?(JSON.parse(data_json)) }
    x.report('arjsv interp (JSON.parse+v)') { arjsv_int.valid?(JSON.parse(data_json)) }
    x.report('arjsv AOT    (JSON.parse+v)') { arjsv_cmp.valid?(JSON.parse(data_json)) }
    # rj_schema's native form: schema preloaded, data JSON parsed natively
    # (RapidJSON, no Ruby-side allocation).
    x.report('rj_schema (preloaded)')       { rj.valid?(:main, data_json) }
    x.compare!
  end
  puts
end
