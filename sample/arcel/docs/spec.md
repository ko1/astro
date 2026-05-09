# arcel — CEL spec coverage

参照仕様: [google/cel-spec](https://github.com/google/cel-spec)
(`UPSTREAM.sha` = `cb51b4176013ad19bd00df94be273c322916a620` time of writing).

## サポート方針

CEL spec のうち arcel が対応するサブセット:

| カテゴリ | MVP | Phase 1 | Phase 2 | スコープ外 |
|---|:-:|:-:|:-:|:-:|
| **scalar literal**: `int`, `uint`, `bool`, `null`, `double`, `string`, `bytes` | int のみ | ✅ | | |
| **算術**: `+ - * / %` | ✅ (int) | + double mixed | + uint, overflow strict | |
| **比較**: `== != < <= > >=` | | ✅ | | |
| **論理**: `&& \|\| !` (short-circuit) | | ✅ | | |
| **三項**: `cond ? a : b` | | ✅ | | |
| **identifier + bindings** | | ✅ | | |
| **field access** (`x.y.z`) | | ✅ | | |
| **index** (`x[i]`, `x["k"]`) | | ✅ | | |
| **list literal** `[a, b, c]` | | ✅ | | |
| **map literal** `{k: v}` | | ✅ | | |
| **`in`** (membership) | | ✅ | | |
| **string fn**: `size`, `startsWith`, `endsWith`, `contains`, `matches` | | | ✅ | |
| **collection fn**: `size`, `[]`, `+` | | | ✅ | |
| **type conv**: `int()`, `string()`, `double()`, `bytes()`, `bool()` | | | ✅ | |
| **macro**: `has(x.y)`, `all`, `exists`, `exists_one`, `map`, `filter` | | | ✅ | |
| **timestamp / duration** | | | | 🚫 部分対応 |
| **protobuf message literal** (`TestAllTypes{...}`) | | | | 🚫 |
| **`google.protobuf.Any`** | | | | 🚫 |
| **enum 値** | | | | 🚫 |
| **`cel.bind` / `cel.block`** (extensions) | | | | 🚫 |
| **`optional` / `Optional`** | | | | 🚫 |

「スコープ外」は cel-go の `ext.*` モジュール扱い、または protobuf ランタイム
依存で arcel として持ちたくない部分。

## conformance 進捗

`make test-ref` で reference impl (cel-go) の上限が見える:

```
file                      pass   fail  error
--------------------------------------------------
basic                       28      4      0     ← core
comparisons                330      0      0     ← Phase 1 全部
conversions                 64     15      0     ← Phase 2
fp_math                     25      2      0     ← Phase 1
integer_math                61      0      0     ← MVP/Phase 1
lists                       36      3      0     ← Phase 2
logic                       21      0      0     ← Phase 1
macros                      37      7      0     ← Phase 2
macros2                      8      0     38     ← cel-go default では
                                                   有効になっていない
parse                       95     16      0     ← parse-only test
string                      46      1      0     ← Phase 2
--------------------------------------------------
TOTAL                      751     48     38  /  837 (89.7% pass)
```

cel-go の 89.7% が現状の天井。arcel は Phase 2 まで完成すれば 600+ pass を
狙えるはず (proto 系を skip してもこの規模)。

## conformance の skip フィルタ

`test/run_conformance.rb` のデフォルトでは、以下のタグの test を skip:

- `proto`: protobuf message literal を含むもの (proto2/3, proto*_ext, enums の
  全部、basic 内の `TestAllTypes{...}`)
- `env`: `container` / `type_env` / `disable_check` を要求するもの
  (dynamic / type_deduction / unknowns / namespace / fields / plumbing)
- `ext`: timestamp / duration / wrappers / optionals /
  bindings_ext / block_ext / encoders_ext / network_ext / math_ext /
  string_ext

`--include proto` などで上書き可能。

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
| eval error | `ERROR: <message>` |

`harness` は得られた stdout 1 行を expected 文字列とそのまま `==` 比較する。
