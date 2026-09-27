# Hash#== / Array#== ask ANY other value for #to_hash / #to_ary (a class object
# included), and JSON's duplicate-key check survives an on_load that maps keys to
# nil / false.
K = Class.new { def self.to_hash = {}; def self.==(o) = o == {} }
p({} == K, {a: 1} == K, [] == nil, {} == 1, [1] == "x", {} == "x")
A = Class.new { def self.to_ary = [1]; def self.==(o) = o == [1] }
p([1] == A, [2] == A)
require 'json'
cb = ->(x) { x.is_a?(String) ? nil : x }
p((JSON.parse('{"a":1,"b":2}', on_load: cb, allow_duplicate_key: false) rescue [$!.class, $!.message]))
cb2 = ->(x) { x.is_a?(String) ? false : x }
p((JSON.parse('{"a":1,"b":2}', on_load: cb2, allow_duplicate_key: false) rescue [$!.class, $!.message]))
p JSON.parse('{"a":1,"b":2}', on_load: cb, allow_duplicate_key: true)
p((JSON.parse('{"a":1,"a":2}', allow_duplicate_key: false) rescue [$!.class, $!.message]), JSON.parse('{"a":1,"b":2}'))
