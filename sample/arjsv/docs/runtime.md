# runtime.md — arjsv の実装詳解

arjsv は CRuby の C 拡張として書かれた JSON Schema validator で、
ASTro フレームワーク上の **schema-as-AST + AOT specialised dispatcher**
モデルで動く。 各 schema が独立した AST に lower され、 必要に応じて
`astro_cs_compile` で SD (specialised dispatcher) になる。

本書は arjsv の内部設計の詳解。 ユーザ向け仕様は [`spec.md`](./spec.md)、
ベンチは [`perf.md`](./perf.md)。

ファイル構成:

| ファイル | 内容 |
|---|---|
| `node.def` | AST ノード定義 (45 種、 全 keyword の EVAL ボディ) |
| `node.h` | NodeHead、 EVAL/HASH/DUMP 宣言、 トラッキング helper、 値型ヘルパ |
| `context.h` | `CTX` 構造体、 `OPTION`、 type bitmask 定数 |
| `node.c` | NODE allocator + `astro_node.c` / `astro_code_store.c` の include + 生成ファイル include |
| `arjsv.c` | CRuby C 拡張エントリ。 `Init_arjsv`、 `Arjsv::Schema` / `Arjsv::Node` の T_DATA 型、 ALLOC ラッパー、 `valid?` / `_compile` |
| `arjsv_gen.rb` | ASTroGen サブクラス: `result_type = "int"`、 `double` operand 対応、 GC mark function 生成 |
| `lib/arjsv.rb` | Schema → AST walker (`Arjsv::Builder`)、 `Schema#validate` / `compile!` Ruby 側 |
| `lib/arjsv/format.rb` | format checker Proc 群 + regex shortcut テーブル |
| `lib/arjsv/content.rb` | `contentEncoding` / `contentMediaType` checker |
| `extconf.rb` / `depend` | CRuby C 拡張 ビルド設定 |

## 1. 値モデル

arjsv は **CRuby の VALUE をそのまま** 使う。 独自の値表現は無し。
data も schema (lower 前) も全て普通の Ruby Hash / Array / 即値。

検証結果は `int` (1 = valid、 0 = invalid)。 ASTroGen の `result_type` を
`int` にして全ノードが int を返す。

## 2. Schema build パイプライン

```
user_schema (Ruby Hash)
        │
        ▼  Arjsv.schema
  Builder.new(formats:, insert_defaults:)
        │
        │  (1) normalize_schema_keys
        │      Symbol key → String、 enum/const 等の data value は触らない
        │  (2) detect_draft
        │      $schema URI で draft 判定 → @assert_content / @ref_keeps_siblings 設定
        │  (3) collect_ids
        │      schema-position を辿って $id / id / $anchor を id_map に登録
        │  (4) reserve_root_slot + preregister_defs
        │      $defs を 2 段階で lower (forward / 再帰 ref 対応)
        │  (5) lower(schema)
        │      keyword ごとに alloc_* を呼んで NODE tree を組み立て
        │
        ▼
  Schema._new(root_node_wrapper, consts_array, entries_array)
        │
        │  ALLOC される全 NODE は Arjsv::Node (T_DATA) で wrap される
        │  Schema は root NODE wrapper + consts (定数) + entries (root
        │  + secondary entries for $ref 等) を保持
        │
        ▼  optional: schema.compile!
  astro_cs_compile(each entry, NULL)   # SD_<hash>.c を吐く
  astro_cs_build(RUBY_CFLAGS)           # gcc で all.so をビルド
  astro_cs_reload()                     # dlclose+dlopen
  astro_cs_load(each entry, NULL)       # head.dispatcher を SD_<hash> に張り替え
        │
        ▼  schema.valid?(data) — hot path
  CTX 初期化 → EVAL(c, root) → indirect call to head.dispatcher
        │
        │  AOT 後: SD_<hash> (1 関数に inline 展開済) が走る
        │  Interp: DISPATCH_node_validate_root から static inline で
        │           子ノードへ降りる
        │
        ▼
  int → Qtrue / Qfalse
```

## 3. Builder の主要状態

```ruby
class Builder
  @consts          # Array<VALUE> — schema-level の定数 (frozen string,
                   # Symbol、 Regexp、 enum/const 値、 default 値、 ref
                   # 先 NODE wrapper、 format checker Proc 等)
  @entries         # Array<NODE wrapper> — astro_cs_compile に登録する
                   # 全エントリ。 root + $defs targets + 任意の
                   # 「runtime-dispatch される NODE」
  @defs_idx        # Hash<String, idx> — $defs name → @consts のスロット位置
  @id_map          # Hash<String, sub_schema> — $id / id / $anchor で
                   # 名前付けられた schema 断片
  @path_cache      # Hash<JSON-pointer, idx> — 同じ pointer に対する
                   # 多重 lower を避けるキャッシュ
  @top_schema      # 全体ルート (general JSON-pointer ref で参照される)
  @assert_format   # 常に true (json_schemer 互換)
  @assert_content  # draft-07: true / 2019-09+: false
  @ref_keeps_siblings  # draft-07: false / 2019-09+: true
  @user_formats    # ユーザが渡した formats: Hash
  @insert_defaults # insert_property_defaults: のフラグ
  @strict_object   # 現在 lowering 中の schema が type:"object" 単一
                   # 指定 → 子の property/required で unsafe 変種を使う
  @strict_array    # 同 type:"array"
end
```

## 4. CTX (実行コンテキスト)

```c
struct CTX_struct {
    VALUE data;             // 現在検証中のデータ。 サブスキーマ再帰時に
                            // save/restore する
    VALUE root_data;        // トップレベル data ($ref 解決の出発点用、
                            // 現状未使用)
    const VALUE *consts;    // schema._consts の RARRAY_CONST_PTR

    int one_of_count;       // oneOf 中の累積 match 数
    int one_of_active;      // oneOf 内ガード
    VALUE one_of_match_keys;  // exactly-1 match 時の eval_keys スナップショット
    int  one_of_match_items;  // 同 eval_items

    VALUE eval_keys;        // unevaluatedProperties 用の "evaluated key" set
                            // Qnil = scope 外、 トラッキング無効
    int   eval_items;       // unevaluatedItems 用の "evaluated prefix" 長
                            // -1 = scope 外
};
```

`data` 以外は基本的に 0 / Qnil / -1 で初期化される。 `eval_*` は
`node_eval_scope` に入った時だけ非 Qnil/-1 になる (= unevaluated_*
keyword を含む schema-level に入った時のみ)。

## 5. 主要ノード分類

### 構造ノード

- `validate_root(body)` — schema のエントリ。 SD specialise の対象。
- `seq(head, tail)` — AND チェーン。 head が 0 を返したら短絡。
- `pass()` / `fail()` — 終端。

### 型 / 値チェック

- `type_check(mask)` — bitmask で `data` の型を判定。 mask は
  `ARJSV_T_NULL | ARJSV_T_INTEGER | …` の OR。
- `const(canonical, idx)` / `enum(canonical, idx, next)` — `consts[idx]`
  との `rb_equal`。
- `minimum(threshold, exclusive)` / `maximum` / `multiple_of(divisor)`
  — `arjsv_value_to_double` 経由で数値比較。
- `min_length(n)` / `max_length(n)` — `rb_str_strlen` で character count。
- `pattern(c_str, regex_idx)` — `rb_reg_match`。 `format` の regex
  shortcut もここを通る。
- `format(name, checker_idx)` — `consts[checker_idx]` の Proc を
  `rb_funcall` で呼ぶ。 stdlib 解析が必要な format 用。
- `content_check(tag, checker_idx)` — 同様、 base64 / JSON 等。

### Hash 走査ノード (object 系)

`property` / `property_unsafe` / `property_with_default` / `required` /
`required_unsafe` / `pattern_property` / `additional_properties_schema` /
`no_additional_properties` / `property_names` / `min_properties` /
`max_properties` / `dependency` / `unevaluated_properties_schema` /
`no_unevaluated_properties`。

`property` 系は **String → Symbol fallback** で hash 引きをする:

```c
VALUE prop = rb_hash_lookup2(c->data, c->consts[key_idx], Qundef);
if (prop == Qundef) prop = rb_hash_lookup2(c->data, c->consts[sym_idx], Qundef);
```

`*_unsafe` バリアントは parent schema が `type: "object"` を保証してい
る場合に Builder が emit するもので、 `RB_TYPE_P(c->data, T_HASH)` の
per-call ガードを省略する。 `type_check` ノードが seq の手前に居て先に
ガードしている前提。

### Array 走査ノード

`items_uniform` / `items_uniform_unsafe` / `items_tuple` /
`additional_items` / `no_additional_items` / `min_items` / `max_items` /
`unique_items` / `contains` / `unevaluated_items_schema` /
`no_unevaluated_items`。

各々データ配列を走査して各要素を save/eval/restore する。
`items_uniform_unsafe` は `type: "array"` 保証時用。

### Combinator

`not` / `if_then_else` / `any_of` (chain 形式) / `one_of` + `one_of_step`。

### Ref

`ref(defs_name, consts_idx)` — `c->consts[consts_idx]` から target NODE
wrapper を取り出して `head.dispatcher` 越しに dispatch する (= EVAL_ARG
ではなく runtime ポインタ読み = SPECIALIZE が cycle で固まらないように)。

### Scope / unevaluated_*

`eval_scope(body)` — 実行前に `c->eval_keys = rb_hash_new()`、
`c->eval_items = 0` を立てて body を実行、 後で restore。 これが
立っている時だけ property/items 系が `arjsv_track_key` /
`arjsv_track_item` を呼んで eval state に追加していく。

`unevaluated_properties_schema(schema)` / `no_unevaluated_properties()`:
`rb_hash_foreach` で data Hash を走査、 各 key を `c->eval_keys` で
チェックし、 含まれていない (unevaluated な) key だけ schema で検証
(or fail)。

`unevaluated_items_schema(schema)` / `no_unevaluated_items()`: data
配列の `[c->eval_items, len)` レンジを処理。

## 6. アノテーション伝播ルール (unevaluated_* 用)

| Combinator | 評価 | 集計 |
|---|---|---|
| `seq` (= AND chain) | 順番に走る | 自然累積 (CTX 共有) |
| `allOf` (= seq) | 全部走る | 自然累積 |
| `$ref` | target を CTX 共有で呼ぶ | 自然累積 |
| `anyOf` | 全 branch 走る (短絡しない) | match した branch のみ contributions 残す (失敗 branch は dup から復元) |
| `oneOf` | 各 branch を独立 state で走る | exactly-1 match 時のみその branch の state を復元 |
| `not` | inner は別スナップショット下で走る | inner contributions は伝播しない |
| `if/then/else` | if 成功 → then 走る (if + then 累積) / if 失敗 → ロールバックして else | 成否で振り分け |

各 sub-schema 再帰 (property の値検証など) では `c->eval_keys = Qnil`
にリセットする。 inner schema が独自の `eval_scope` を持っていれば
そこで再 setup される。 持っていなければ inner の property nodes は
トラッキングを skip するので、 inner contributions が outer eval_keys を
汚染しない。

## 7. AOT specialization

`schema.compile!` を呼ぶと `Arjsv::Schema._compile(cflags)` が走る:

1. `s->entries` (= root + secondary entries) の各 NODE について:
   `astro_cs_compile(node, NULL)` で `code_store/c/SD_<hash>.c` を出力
2. `astro_cs_build(cflags)` で `gcc` を起動して `all.so` を作る
   - `cflags` は `-I<rubyhdrdir> -I<rubyarchhdrdir>` (Ruby ヘッダ用)
3. `astro_cs_reload()` で `all.so` を `dlclose` + `dlopen`
4. 各 entry の `astro_cs_load(node, NULL)` で `head.dispatcher` を
   `SD_<hash>` シンボルに張り替え

SD ファイルの構造 (例: user_object schema):

```
SD_<root>(c, n)        ← entry、 public extern
  inline → EVAL_node_validate_root
    inline → EVAL_node_seq(c, n, head, SD_<head_hash>, tail, SD_<tail_hash>)
      inline → SD_<head_hash> (= type_check)
      inline → SD_<tail_hash> (= seq...)
        ...
```

コンパイラは `SD_*` シンボルを `static inline` として扱うので、 entry
SD 関数の中に AST tree 全体が 1 関数に inline 展開される。 `objdump
code_store/all.so` で確認すると、 残る `call` は `rb_hash_lookup2` /
`rb_str_strlen` 等の CRuby C API 呼び出しのみ。

## 8. ASTroGen 拡張

`arjsv_gen.rb` の `ArjsvNodeDef` で:

- `result_type = "int"` (各 dispatcher / EVAL の戻り値型)
- `double` operand サポート (`hash_uint64(arjsv_double_bits(d))` で
  bit-pattern hash、 SD specializer は `%.17g` で literal 化)
- `int64_t` operand サポート (整数閾値用)
- `register_gen_task :mark` で GC mark 関数を生成 (NODE は T_DATA で
  wrap されているので子ノードを `rb_gc_mark` する)

`@ref` operand は使っていない (property names 等は consts 経由なので
副情報として埋め込む必要がない)。

## 9. パフォーマンス特性

`docs/perf.md` 参照。 要点:

- 主たる時間消費は CRuby C API (`rb_hash_lookup2`、 `rb_str_strlen`
  等) と Ruby method dispatch (`vm_exec_core`)。 ASTro 側 dispatch は
  `objdump` で 1 関数に inline 済、 ほぼゼロ。
- AOT vs interp の差は ~1-2× 程度に留まる (CRuby C API が支配的なため)。
  ベンチでは AOT が時々わずかに速いが、 interp も branch predictor が
  hot path を完全に学習するので大差にならない。
- vs json_schemer: 100-200× の優位 (我々の AST + native dispatcher
  チェーンに対し、 json_schemer は per-validate で `deep_stringify_keys`
  + Ruby method dispatch chain)。
- vs rj_schema (Rust + RapidJSON、 FFI 経由): 4-19×。 rj_schema は
  per-call FFI 境界 + JSON parsing が固定オーバーヘッド。

## 10. デバッグ

```ruby
schema._dump
# stderr に AST を 1-line 表示。
# (node_validate_root (node_seq (node_type_check 4) ...))
```

```sh
# AOT 後の SD assembly を見る
nm code_store/all.so | grep " T SD_"
objdump -d code_store/all.so | less
```

公式テストスイート:

```sh
ruby -Ilib test/run_official_suite.rb        # default = draft-07
DRAFT=draft2020-12 ruby -Ilib test/run_official_suite.rb
SHOW_FAILS=1 ruby -Ilib test/run_official_suite.rb   # 失敗ケース全表示
```

`/tmp/jsts/json-schema-org-*/tests/<draft>` を expect。 別 path にしたい
場合は `SUITE_PATH=` を立てる。

## 11. ベンチ

```sh
make bench    # vs json_schemer / rj_schema
```

`benchmark/run.rb` が 5 schema × 2 シナリオを回す。 詳細は
[`perf.md`](./perf.md)。
