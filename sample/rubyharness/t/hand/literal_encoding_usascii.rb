# coding: US-ASCII
# frozen_string_literal: true
# US-ASCII ソースで \xNN / \NNN を書いた literal は ASCII-8BIT になる (CRuby)。
p __ENCODING__.to_s
p "abc".encoding.to_s
p "abc".frozen?
p "\xFF".encoding.to_s
p "\377".encoding.to_s
p "\xFF".frozen?
p "a".encoding.to_s
p "\u00e9".encoding.to_s
p("\377" == "\xFF".b)
p "x#{1}y".encoding.to_s
p "x#{1}\xFF".encoding.to_s
p :"\xFF".encoding.to_s
p(/\xFF/.encoding.to_s)
