# Vendored from the json gem 3.0.2 (ext/json/ext/vendor/)

koruby's native JSON parser / generator (builtins/json_native.c) uses the same number
conversions as CRuby's json extension, so results are bit-identical to CRuby.

| file | used for | license (see the file's own header) |
|---|---|---|
| `fast_float_parser.h` | decimal → double (Eisel-Lemire, from fast_float) | MIT |
| `fpconv.c` | double → shortest decimal (from night-shift/fpconv, Grisu) | BSL-1.0 |

Copied unchanged from `ext/json/ext/vendor/` of json 3.0.2; update together with lib/json.
