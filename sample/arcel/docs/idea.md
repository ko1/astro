# arcel — design notes

## なぜ ASTro × CEL か

CEL の本番ユースケースは **K8s ValidatingAdmissionPolicy / Envoy authz /
gRPC interceptor / Firebase Security Rules** で、共通点は:

- **policy は固定** (deploy されたら変わらない)
- **入力は大量** (リクエストごと)
- **request hot path にいる** (sidecar の p99 latency 直撃)

これは ASTro が想定する partial evaluation の理想形。policy の AST 全体を
1 つの specialized C 関数に展開できれば、interpreter dispatch overhead を
完全に消せる。

参考: cel-go は今のところ純 interpreter で、cel-cpp も同様。OPA は同じ問題意識
から `opa build --target=wasm` で Wasm への AOT を提供しているが、Wasm という
indirection 層がある分まだ機械語まで降りていない。**arcel が standalone で
ネイティブ AOT** を提供できれば、CEL での新規ポジションになる。

## 既存サンプルからの教訓

### arjsv (JSON Schema validator) — `docs/perf.md`

arjsv は CRuby C 拡張として実装されていて、AOT が interp に対して **ほぼ tied**。
原因は per-node の中身が `rb_hash_lookup2` / `rb_str_hash` という CRuby C API
呼び出しで、ASTro の partial evaluation が手を出せない外側に時間が逃げている。

> Per-node work is a CRuby C API call (~10–80 ns) that the framework can't reach into.

**arcel への教訓**: 入力表現も自前で握る。standalone プロセスとして JSON 入力を
arcel 自身の `arcel_value` (struct) にパースし、field access は struct slot の
直接 load にする。Ruby の `Hash` を経由したら同じ floor を踏む。

### nuq (jq クローン) — `sample/nuq`

CLI として独立、自前 GC、自前 value 表現。jq 公式テスト 524/526 で
jq の **1.3〜50×**。arcel が目指すべき構成のテンプレート。

特に参考になるのは:
- per-run arena + Cheney copying GC (per-eval だと毎回 reset)
- value は 1-bit fixnum タグ + struct 判別共用体 (NaN-boxing なし、user feedback 反映)
- AST 木ウォーカー + parse-time AST fusion + 線形性解析

### astrogre (regex エンジン) — `sample/astrogre/docs/perf.md`

regex の **pattern も入力も** ASTro 配下に置けたケース。pattern 単位 AOT で
Onigmo に対して大幅な勝利。**arcel もこのパターン** (policy + 入力表現の
両方を握る) を踏襲する。

## value 表現

NaN-boxing は user feedback (`feedback_no_nan_boxing.md`) で禁止されている。
代わりに **tagged union** で行く:

```c
typedef enum { AC_ERR, AC_NULL, AC_BOOL, AC_INT, AC_UINT, AC_DOUBLE,
               AC_STRING, AC_BYTES, AC_LIST, AC_MAP, AC_OBJECT,
               AC_TIMESTAMP, AC_DURATION } arcel_tag;

typedef struct {
    arcel_tag tag;
    union { int64_t i; uint64_t u; double d; bool b;
            struct { const char *p; uint32_t len; } s;
            struct arcel_list *list; struct arcel_map *map;
            const char *err;
            struct { const void *obj;
                     const struct arcel_object_desc *desc; } object;
            struct { int64_t s; int32_t ns; } ts;     /* TIMESTAMP / DURATION */
    };
} VALUE;
```

サイズは 24 bytes (16-byte payload + 4-byte tag + padding)。SysV ABI では
2 register 返り。tag がレジスタに乗りやすく、specialize 後は型分岐ごと
コンパイラが consteval して消える。

`AC_OBJECT` は embedder が自前 struct / proto message を渡すための pass-
through で、`arcel_object_desc` (field/has/format_json コールバック群) が
arcel ↔ embedder 間の境界。これにより arcel 本体は **proto / 任意 binary
表現を一切知らない** まま、libprotobuf や C struct を直接舐められる。

## AOT specialization の狙い目

CEL の AST は典型的に小さい (10〜30 ノード) ので、specialize で:

1. **field access のチェーン** を struct offset の直接読みに展開
2. **入力 type** が判明している箇所は dispatch を消す (e.g. `u.age` の `age`
   は int だと分かれば `u + offsetof(age)` の `int64_t` load 1 回)
3. **boolean ladder** の short-circuit を branch 化
4. **`x in [a,b,c]`** を分岐 cascade に展開
5. **`xs.all(x, p(x))`** を loop unroll (xs のサイズが入力表現で分かれば)

これで「policy AST + 入力 layout」から「専用 bool 関数 1 個」への変換が成立する。

## 入力の表現

CEL の field access は重要 hot path なので、JSON parse 時に:

- top-level binding hash を `arcel_value` の `LIST` / `MAP` 構造に展開
- map は **小さければ flat array of (key, value) で linear search**、
  大きければ open-addressing hash table (key の `String#hash` を pre-compute)
- 同じ shape の入力が来たら shape cache でレイアウトを一度だけ解析

policy 側で参照される field path は静的に分かる (parse 時に collect)
ので、入力 layout を policy 寄りに事前最適化することも将来的にあり得る
(JIT で起動時に構造変換するイメージ)。

production 用途では JSON を経由せず、`arcel_object_desc` で **embedder の
ネイティブ表現** (proto message / C struct) を直接舐めるのが正解。
`examples/embed_object.c` (C struct) と `examples/embed_protobuf.cc`
(libprotobuf 経由) がそのテンプレート。

## proto 対応の設計

arcel 本体は protobuf を一切知らない方針を維持しつつ、`AC_OBJECT` 経由で
任意の proto runtime をブリッジできる。実装:

- `examples/arcel_protobuf.h` — header-only / ~100 行の libprotobuf 用
  `arcel_object_desc`。`Reflection::FindFieldByName` で全 .pb.h-生成型
  に対応。scalar / enum / nested message / repeated / map<K,V> をフル
  カバー (Phase 5–6)。
- per-eval arena handle (`arcel_arena_handle`) を field callback に
  渡し、callback 側で `arcel_value_list_new` / `arcel_value_map_new` /
  `arcel_value_string_copy` を呼べる。これで repeated string や map
  も copy が一回で済む。

cel-cpp 互換 API は `compat/celcpp_compat.hpp` の単一 header (header-
only、~340 行) で、`Parse` / `CelExpressionBuilder` / `Activation` /
`Evaluate` 等の typical cel-cpp embedder コードがそのまま動くよう
shim を被せる (Phase 4)。

## non-goal (現状)

| 項目 | 状況 |
|---|---|
| Timestamp / Duration | ✅ 実装済 (Phase 7、conformance 73/73 = 100%) |
| google.protobuf.{Timestamp,Duration,*Value} 型識別子 | ✅ 実装済 (Phase 8) |
| google.protobuf.{Int32Value,...}{value: X} wrapper literal | ✅ 実装済 (Phase 8、auto-unwrap) |
| **proto2/3 user 定義 message literal** (`TestAllTypes{...}`) | 🚫 still non-goal — 本物の型レジストリ + 任意 proto runtime 抱え込みが必要。embedder hook 経由で cel-cpp shim 側に持たせる選択肢はある |
| `optional` 型 (`x.?y`, `optional.of(...)`) | 🚫 |
| `cel.bind` / `cel.block` (ext lib) | 🚫 |
| 任意精度算術 (CEL 仕様にもない) | 🚫 |
| ext.* 拡張 (`strings.replace`, `network.url` 等) | 🚫 (個別追加可) |
| enum 値 (proto enum 名前空間) | 🚫 |
