# arcel — CEL spec coverage

参照仕様: [google/cel-spec](https://github.com/google/cel-spec)
(`UPSTREAM.sha` = `cb51b4176013ad19bd00df94be273c322916a620` time of writing).

## サポート方針

CEL spec のうち arcel が対応するサブセット:

| カテゴリ | 状況 |
|---|:-:|
| **scalar literal**: `int`, `int64`, `uint`, `bool`, `null`, `double`, `string`, `bytes` | ✅ |
| **算術**: `+ - * / %` (int/uint/double + overflow strict + cross-type 抑止) | ✅ |
| **比較**: `== != < <= > >=` (cross-type 数値, bool ordering, string/bytes lex) | ✅ |
| **論理**: `&& \|\| !` (short-circuit + commutative-on-error) | ✅ |
| **三項**: `cond ? a : b` | ✅ |
| **identifier + bindings** | ✅ |
| **field access** (`x.y.z`) / **index** (`x[i]`, `x["k"]`) | ✅ |
| **list literal** `[a, b, c]` (constant fold あり) | ✅ |
| **map literal** `{k: v}` | ✅ |
| **`in`** (membership: list / map keys) | ✅ |
| **string fn**: `size`, `startsWith`, `endsWith`, `contains`, `matches` | ✅ |
| **collection fn**: `size`, `[]`, `+` | ✅ |
| **type conv**: `int()`, `string()`, `double()`, `bytes()`, `bool()`, `uint()`, `dyn()` | ✅ |
| **macro**: `has(x.y)`, `all`, `exists`, `exists_one`, `map`, `filter` | ✅ |
| **macro2** (cel-spec 二引数形式 + `transformList` / `transformMap`) | ✅ |
| **timestamp / duration** (constructor + arithmetic + selectors w/ tz) | ✅ |
| **type identifier** (`int`, `string`, `google.protobuf.Timestamp`, …) | ✅ |
| **wrapper-message literal** (`google.protobuf.Int32Value{value: X}` 等) | ✅ |
| **AC_OBJECT pass-through** (embedder の native struct / proto を直接読む) | ✅ |
| **proto2/3 user 定義 message literal** (`TestAllTypes{...}`) | 🚫 |
| **`google.protobuf.Any` (構築 / unmarshal)** | 🚫 |
| **enum 値** (proto enum 名前空間) | 🚫 |
| **`cel.bind` / `cel.block`** (ext lib) | 🚫 |
| **`optional` / `Optional`** (`x.?y` 等) | 🚫 |
| **ext.*** (`strings.replace`, `network.url`, `lists.range`, …) | 🚫 |

「🚫」は cel-go の `ext.*` モジュール扱いか、protobuf ランタイム依存で
arcel として持ちたくない部分。

## conformance 結果

`make test-ref` で reference impl (cel-go) の上限が見える。
`ruby test/run_conformance.rb` がデフォルトで走る core suite:

```
file                      pass   fail  error
--------------------------------------------------
basic                       32      0      0
comparisons                303      0      0
conversions                 80      0      0
fp_math                     26      0      0
integer_math                61      0      0
lists                       39      0      0
logic                       21      0      0
macros                      44      0      0
macros2                     46      0      0
parse                      111      0      0
string                      47      0      0
--------------------------------------------------
TOTAL                      808      0      0  /  808 (100.0% pass)
```

**core: 808/808 = 100%** (cel-go reference は同 harness で 89.7%)。

`--include ext` で `:ext` タグの suite を追加:

```
basic                       32      0      0
bindings_ext                 0      0      5
block_ext                    0      0     22
comparisons                303      0      1
conversions                 80      0      2
encoders_ext                 0      0      1
fp_math                     26      0      0
integer_math                61      0      0
lists                       39      0      0
logic                       21      0      0
macros                      44      0      0
macros2                     46      0      0
math_ext                    17      0     99
network_ext                  8      0     60
optionals                    3      0     56
parse                      111      0      0
string                      47      0      0
string_ext                   9      0     78
timestamps                  73      0      0     ← Phase 7+8 で 0→73 (100%)
--------------------------------------------------
TOTAL                      920      0    324  /  1244 (74.0% pass)
```

`error` 列は arcel が当該 ext 機能を未実装 (0% 対応)。`pass` 列が
core 808 + timestamps 73 + comparisons/conversions 等で増えた分。

## conformance の skip フィルタ

`test/run_conformance.rb` のデフォルトでは、以下のタグの test を skip:

- `proto`: protobuf message literal を含むもの (proto2/3, proto*_ext, enums の
  全部、basic 内の `TestAllTypes{...}`)
- `env`: `container` / `type_env` / `disable_check` を要求するもの
  (dynamic / type_deduction / unknowns / namespace / fields / plumbing)
- `ext`: timestamp / duration / wrappers / optionals /
  bindings_ext / block_ext / encoders_ext / network_ext / math_ext /
  string_ext

Phase 7+8 以降は **timestamps / wrappers が完動** (timestamps 73/73、
dynamic の wrapper literal 5/6) なので、harness 側で `:ext` から
これらだけ外して core に取り込んでも良い。`--include proto,env,ext` の
ワンライナーでも全タグを開けられる。

## 評価式の output format

cel-go と同じ JSON 表現を採用:

| CEL 値 | 出力 |
|---|---|
| `int64_value: 42` | `42` |
| `uint64_value: 42` | `42u` |
| `double_value: 3.14` | `3.14` |
| `string_value: "foo"` | `"foo"` |
| `bool_value: true` | `true` |
| `null_value: NULL_VALUE` | `null` |
| `list_value: { values: ... }` | `[1, 2, 3]` |
| `map_value: { entries: ... }` | `{"a": 1, "b": 2}` (キーで sort) |
| `bytes_value` | (未対応; harness 上は :unsupported で skip) |
| `type_value: "int"` (cel-spec の type literal) | `"int"` (string として返す。`type(x) == int` が string 比較に折り畳まれる) |
| `Timestamp(s, ns)` | `"2009-02-13T23:31:30Z"` (RFC3339, fractional は最小桁) |
| `Duration(s, ns)` | `"1000000s"` 形式 |
| eval error | `ERROR: <message>` |

`harness` は得られた stdout 1 行を expected 文字列とそのまま `==` 比較する。

## 拡張点 — embedder hook

arcel 本体は proto / 任意 binary 表現を一切知らない方針を維持しつつ、
以下の経路で外部表現を読める:

1. **`AC_OBJECT` + `arcel_object_desc`**: embedder が `field` /
   `has` / `format_json` の 3 callback を渡し、arcel は AST 評価中に
   それらを呼ぶ。proto / C struct / capnproto 等、なんでも繋がる。
2. **`arcel_arena_handle`** が field callback に渡されるので、
   callback 側で `arcel_value_list_new` / `arcel_value_map_new` /
   `arcel_value_string_copy` を呼んで per-eval arena に list / map /
   owned string を構築できる。
3. **cel-cpp shim** (`compat/celcpp_compat.hpp`): cel-cpp の
   `Parse` / `CelExpressionBuilder` / `Activation` / `Evaluate` API
   形に `arcel_*` を被せた header-only ライブラリ。

例:

```cpp
// libprotobuf adapter (examples/arcel_protobuf.h)
arcel_activation_set_object(act, "u", &my_proto_msg, &arcel::pbf::descriptor);
arcel_value r = arcel_eval(prg, act);    // u.age, u.tags.all(...), u.labels["k"] が動く
```
