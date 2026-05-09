# spec.md — arjsv 言語仕様

`arjsv` (ASTro Ruby JSON Schema Validator) は **JSON Schema validator** を
ASTro に乗せた CRuby C 拡張。 [json_schemer](https://github.com/davishmcclurg/json_schemer)
と互換 API を持つドロップイン代替を目指す。

本書は arjsv の user-facing リファレンス。 内部実装は
[`runtime.md`](./runtime.md)、 ベンチは [`perf.md`](./perf.md)、 残作業は
[`todo.md`](./todo.md)、 完成済機能の要約は [`done.md`](./done.md)。

JSON Schema 自体の規範文書は [json-schema.org](https://json-schema.org/) を参照。

## 対応 draft

| draft | 自動検出 | 公式テストスイート |
|---|---|---|
| draft-04 | `$schema` に `draft-04` を含む | 906 / 917 = **98.80%** |
| draft-06 | 同 `draft-06` | 1196 / 1209 = **98.92%** |
| draft-07 | **default** (no `$schema` / 該当 URI) | 1513 / 1584 = **95.52%** |
| 2019-09 | 同 `2019-09` | (2020-12 と同種カバー) |
| 2020-12 | 同 `2020/12` | 1957 / 2069 = **94.59%** |

`$schema` がない場合は draft-07 として扱う。 一部 keyword は draft 間で
意味が違うので、 アサーション/アノテーション切替や keyword renaming は
内部で transparent に実施。

残失敗の内訳は [`done.md`](./done.md) §「failure breakdown」参照。 短く
言うと **外部依存 (libidn / HTTP fetch) が要るもの** と **`$dynamicRef`
の dynamic-scope 解決** がほぼ全部。

## Public API

```ruby
require 'arjsv'
```

### `Arjsv.schema(schema_obj, **opts) → Arjsv::Schema`

スキーマを ASTro AST に lower した Schema オブジェクトを返す。 引数:

| 引数 | 説明 |
|---|---|
| `schema_obj` | 解析済 Hash (String / Symbol key OK)、 `true` (always-valid)、 `false` (always-invalid) |
| `formats:` | `Hash<String, #call>` で独自 format を登録 |
| `insert_property_defaults:` | `true` で `default` 値の自動補完 (data hash を mutate) |

呼び出し例:

```ruby
# 標準的な使い方
schema = Arjsv.schema('type' => 'integer', 'minimum' => 0)

# Symbol key も OK
schema = Arjsv.schema(type: 'integer', minimum: 0)

# 独自 format
phone = ->(s) { s =~ /\A\+?\d{10,15}\z/ }
schema = Arjsv.schema({'format' => 'phone'}, formats: { 'phone' => phone })

# default 補完
schema = Arjsv.schema(
  { 'type' => 'object',
    'properties' => { 'role' => { 'default' => 'user' } } },
  insert_property_defaults: true,
)
data = {}
schema.valid?(data)
data    # => { 'role' => 'user' }   ← mutated
```

### `schema.valid?(data) → Bool`

データが schema を満たすかチェック。 fast path、 副作用なし。

入力 data はパース済の Ruby 値 (`JSON.parse` の結果など)。 String key /
Symbol key どちらの Hash も透過に処理 (String 入力に最適化、 Symbol
入力にもフォールバック)。

### `schema.validate(data) → Enumerator`

検証エラーの Enumerator を返す。 各要素は json_schemer 互換の error
Hash (`data` / `data_pointer` / `schema` / `schema_pointer` / `type` /
`error` / `details` 等を含む)。

実装としては成功時 `[].each`、 失敗時のみ json_schemer に委譲。 hot path
は arjsv 速度を維持。

### `schema.compile! → self`

Schema の AST を AOT specialise。 同じ schema を hot loop で何度も呼ぶ
場合に推奨。 1 回目の `compile!` のみ重い (`astro_cs_compile` →
`astro_cs_build` → `dlopen`)、 以降の `valid?` は specialised
dispatcher を直接叩く。

```ruby
schema = Arjsv.schema(...)
schema.compile!
1_000_000.times { schema.valid?(data) }    # 各呼び出しは specialised SD
```

### `schema.valid_schema? → Bool` / `Arjsv.valid_schema?(schema_obj) → Bool`

スキーマ自身が well-formed JSON Schema かどうかを meta-schema で検証。
json_schemer に delegate。

## Schema 言語

### 値モデル

JSON 由来の 7 型:

| 型 | 例 |
|---|---|
| null     | `nil` |
| boolean  | `true` / `false` |
| integer  | `42`、 `-7` (Ruby `Integer`、 整数値の `Float` も `integer` 型扱い) |
| number   | `3.14`、 `1` (`Numeric` 全般) |
| string   | `"hello"` |
| array    | `[1, 2]` |
| object   | `{"a" => 1}`、 `{a: 1}` 両対応 |

データ側は `JSON.parse(symbolize_names: false/true)` どちらでも OK。
`enum` / `const` の値は user の data 慣習に合わせて Symbol/String を
区別する (= 比較は `Object#==` 経由)。

### Boolean schema

- `true`: 何でも valid
- `false`: 何でも invalid

### Object schema

| keyword | 受け取る形 | 備考 |
|---|---|---|
| `type`               | string or array of strings | `null bool int num str arr obj` |
| `properties`         | Hash<key, schema> | |
| `required`           | Array<String> | |
| `additionalProperties` | bool / schema | 既知 key 以外の制約 |
| `patternProperties`  | Hash<regex, schema> | regex は draft 7 で Ruby Regexp、 2020-12 では ECMA-262 風 |
| `propertyNames`      | schema | 各 key に schema を適用 |
| `dependencies` (draft-07 / draft-06 / draft-04) | Hash<key, Array<String> \| schema> | trigger キー存在時の追加制約 |
| `dependentRequired` (2019-09+) | Hash<key, Array<String>> | dependencies の Array form を分離 |
| `dependentSchemas` (2019-09+) | Hash<key, schema> | dependencies の schema form を分離 |
| `minProperties` / `maxProperties` | integer | |
| `unevaluatedProperties` (2019-09+) | bool / schema | 兄弟 keyword で「評価」 されなかった key への制約 |

### Array schema

| keyword | 形 | 備考 |
|---|---|---|
| `items`              | schema (uniform) または Array<schema> (tuple, draft-07 以下) | 2020-12 では schema のみ |
| `prefixItems` (2020-12) | Array<schema> | tuple 形式 |
| `additionalItems`    | bool / schema | tuple 形 items の余り |
| `contains`           | schema | 1 個以上マッチが必要 |
| `minContains` / `maxContains` (2019-09+) | integer | contains の数制約 |
| `minItems` / `maxItems` | integer | |
| `uniqueItems`        | bool | true で全要素一意 |
| `unevaluatedItems` (2019-09+) | bool / schema | 「評価」 されなかった index への制約 |

### Number schema

| keyword | 形 | 備考 |
|---|---|---|
| `minimum` / `maximum` | number | inclusive |
| `exclusiveMinimum` / `exclusiveMaximum` | number (draft-07+) or bool (draft-04) | |
| `multipleOf`         | number > 0 | divisor |

### String schema

| keyword | 形 | 備考 |
|---|---|---|
| `minLength` / `maxLength` | integer | character count、 encoding-aware |
| `pattern`            | string (regex) | Ruby Regexp で評価。 ECMA-262 \s に近づくよう char-class outside の \s / \S を Unicode 化 |
| `format`             | string | 下表参照 |

### サポート format

`format` は draft-07 では assertion mode (default)、 2019-09+ では
annotation-only がデフォルト。 arjsv は **常に assertion mode**
(json_schemer と同じ挙動)。

| format | 検証 |
|---|---|
| `date`               | RFC 3339 full-date、 proleptic Gregorian (Julian leap year を拒否) |
| `date-time`          | RFC 3339 + leap second 制約 |
| `time`               | RFC 3339 §5.6 full-time (timezone offset 必須) |
| `duration` (2019-09+) | ISO 8601 ABNF (Y/M/D 順序、 W 単独、 fractional `S` は禁止) |
| `email` | RFC 5322 dot-atom + quoted-string local part / RFC 5321 domain (label or `[<IPv4>]` / `[IPv6:<IPv6>]`) |
| `idn-email` | 同 RFC 5322 構造、 ただし local-part / domain label に Unicode `\p{L}\p{N}\p{M}` を許可 |
| `hostname` / `idn-hostname` | RFC 1123 ベース。 IDNA contextual rules は libidn 無しの近似 |
| `ipv4` / `ipv6`      | `IPAddr` 経由、 ipv6 の zone-id / netmask は拒否 |
| `uuid`               | 8-4-4-4-12 hex |
| `uri` / `uri-reference` | `URI.parse`、 absolute URI は scheme 必須 |
| `iri` / `iri-reference` | non-ASCII を percent-encode で URI に変換して `URI.parse` |
| `json-pointer` / `relative-json-pointer` | RFC 6901 |
| `regex`              | `Regexp.new` 試行 |
| `uri-template`       | RFC 6570 loose check (`{var}` の balance + 文字種) |

未知の format keyword はデフォルトで pass (annotation-only)。 `formats:`
オプションで上書き / 追加可能。

### Combinator

| keyword | 形 |
|---|---|
| `allOf`              | Array<schema> 全部 valid |
| `anyOf`              | 1 個以上 valid (annotation 用に全 branch を評価) |
| `oneOf`              | ちょうど 1 個 valid |
| `not`                | inner が invalid なら valid |
| `if` / `then` / `else` | if-schema が valid なら then、 invalid なら else |

### Reference

| keyword | 形 |
|---|---|
| `$id` / `id` (draft-04) | この sub-schema を識別する URI / 名前 |
| `$ref`               | 参照解決 (下記参照) |
| `$defs` / `definitions` | サブスキーマ定義 |
| `$anchor` / `$dynamicAnchor` | local fragment ID |
| `$dynamicRef`        | 現在 `$ref` 同等扱い (full dynamic scoping は未対応) |

`$ref` の解決パス (上から優先):
1. `#` / `#/` → 自分自身 (root pointer、 ただし enclosing `$id` が
   無いときのみ; 入子 `$id` 配下では下記 4 経由で resolve される)
2. `#/$defs/<name>` / `#/definitions/<name>` (single segment) → 事前登録
   slot 経由 (forward / 再帰 ref OK)
3. **RFC 3986 base-URI resolution**: 現在の `$id` を base に `$ref` を
   absolute URI へ resolve し、 `<$id>`-map を引く
4. `#/<json-pointer>` (multi-segment) → 現在 resource (= 現在 `$id` の
   schema、 無ければ top schema) を起点に pointer を歩く
5. それ以外 (外部 HTTP URI / URN) → annotation 化 (always-valid に
   フォールバック、 `$VERBOSE` で警告)

`$id` は declared 時点の `$base_uri` に対し RFC 3986 で resolve され、
absolute / relative どちらの形でも `id_map` に登録される。 `$anchor`
/ `$dynamicAnchor` は `<base>#name` と `#name` の両方で登録される。

`$ref` の sibling 扱い:
- draft-04 / draft-06 / draft-07: sibling 無視 (json_schemer と一致)
- 2019-09 / 2020-12: sibling 適用

### `$id`-based reference

```ruby
schema = Arjsv.schema(
  '$schema' => 'http://json-schema.org/draft-04/schema#',
  'definitions' => {
    'Pos' => { 'id' => 'positive', 'type' => 'integer', 'minimum' => 0 },
  },
  '$ref' => 'positive',
)
schema.valid?(5)    # => true
```

### 再帰 `$ref`

```ruby
tree = {
  '$defs' => {
    'Tree' => {
      'type' => 'object',
      'properties' => {
        'value'    => {'type' => 'integer'},
        'children' => {'type' => 'array', 'items' => {'$ref' => '#/$defs/Tree'}},
      },
    },
  },
  '$ref' => '#/$defs/Tree',
}
Arjsv.schema(tree).valid?({ 'value' => 1, 'children' => [{ 'value' => 2 }] })
```

### Metadata-only keywords

下記は arjsv では annotation 扱い (バリデーション結果に影響しない):
`title` / `description` / `$comment` / `default` / `examples` /
`readOnly` / `writeOnly` / `deprecated` / `$schema` / `$vocabulary`。

## オプション 詳細

### `formats:`

```ruby
Arjsv.schema(
  { 'format' => 'phone' },
  formats: {
    'phone' => ->(value) { value.match?(/\A\+?\d{10,15}\z/) },
  },
)
```

- 値は `#call(string) → truthy/falsy` を満たす Proc / Method など
- 既存 format を override 可能 (`formats: { 'email' => -> { ... } }`)
- `nil` / `false` 値で format を無効化 (annotation-only)
- 引数は Symbol/String どちらでも OK (内部で String にノーマライズ)

### `insert_property_defaults:`

```ruby
schema = Arjsv.schema(
  { 'type' => 'object',
    'properties' => {
      'role' => { 'default' => 'user' },
      'tags' => { 'default' => [] },
    } },
  insert_property_defaults: true,
)

data = { 'name' => 'Alice' }
schema.valid?(data)
data    # => { 'name' => 'Alice', 'role' => 'user', 'tags' => [] }
```

- `valid?` の中で data Hash を **mutate** する (副作用あり)
- 既に存在するキーは上書きしない
- Symbol-key データに対しては String key で挿入される (canonical form)
- 挿入された default 値も sub-schema で validate される (default が
  schema を満たさない場合は validation fail + 値は挿入済み残る)

## 制限事項

「外部依存なし」 の方針で意図的に未対応にしている / できない項目:

- **外部 `$ref` (HTTP fetch / URN base URI)**: ネットワーク不要を維持。
  公式メタスキーマ自体を fetch する必要があるテスト
  (`refRemote.json` / `definitions.json` の metaschema チェック /
  `cross-draft.json` / `vocabulary.json`) が落ちる。
- **`$dynamicRef` の dynamic-scope 解決**: 現状は static `$ref` 同等
  (matching `$dynamicAnchor` を id_map から探す)。 dynamic scope に
  よって解決先が変わるケース (2020-12 `dynamicRef.json` の 16 件) は
  落ちる。
- **`format: hostname` / `idn-hostname` / `idn-email` の IDNA-2008
  punycode + Unicode contextual rules**: libidn / ICU テーブル相当が
  必要。 ASCII / UTF-8 ベースの近似のみ実装。
- **ECMA-262 strict pattern**: `pattern` を Onigmo で評価 (char-class 外
  の `\s` / `\S` は Unicode whitespace に書き換え)。 ECMA が拒否する
  `\a` 等の制御エスケープを arjsv は受け付ける (1 件落ち)。
- **`unevaluatedItems` + `contains` の sparse 追跡**: `c->eval_items` が
  prefix-count (int) なので、 contains で matched 個別 index だけを
  track することができない (2 件落ち)。 sparse-set 表現にすれば
  fix できる。

実装した範囲 (RFC 3986 base resolution、 quoted-string email、 Rational
fallback multipleOf、 unevaluated_* annotation propagation、
`unevaluatedProperties:true` / `unevaluatedItems:true` の全評価マーク等)
は [`done.md`](./done.md) と [`todo.md`](./todo.md) を参照。
