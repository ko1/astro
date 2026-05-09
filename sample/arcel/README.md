# arcel — CEL on ASTro

ASTro 上に乗せた **CEL (Common Expression Language) 互換評価器**。
Google の `cel-go` のドロップイン代替を目指す standalone CLI で、ASTro
の partial evaluation をエンジンに採用している。

## 現状

**conformance**: arcel **808 / 808 = 100.0%** vs cel-go (同 harness) 751 / 837 = 89.7% — **+10.3 pp**。

**bench (2M iter × 3 試行のメディアン, x86_64 Ubuntu 24.04 gcc 13.3)**:

| case | arcel/plain | arcel/AOT | celgo | **celcpp** | AOT vs celgo | **AOT vs celcpp** |
|---|---:|---:|---:|---:|---:|---:|
| arith_const            |   71 ns/op |    60 ns/op |   137 ns/op |   116 ns/op |  2.28× |  **1.93×** |
| bool_ladder            |   27 ns/op |     5 ns/op |    56 ns/op |   213 ns/op | 11.20× | **42.60×** |
| field_access_shallow   |   56 ns/op |    24 ns/op |   207 ns/op |   549 ns/op |  8.62× | **22.90×** |
| field_access_deep      |   70 ns/op |    28 ns/op |   195 ns/op |   791 ns/op |  6.96× | **28.25×** |
| predicate_user         |   80 ns/op |    44 ns/op |   522 ns/op |   670 ns/op | 11.86× | **15.23×** |
| list_all_small (5)     |  128 ns/op |   111 ns/op |  1204 ns/op |   977 ns/op | 10.85× |  **8.80×** |
| list_all_med (100)     | 2361 ns/op |  2049 ns/op | 19723 ns/op | 14676 ns/op |  9.63× |  **7.16×** |
| list_exists (100)      | 1069 ns/op |  1130 ns/op | 10467 ns/op |  7475 ns/op |  9.26× |  **6.62×** |
| string_starts          |   14 ns/op |     7 ns/op |   101 ns/op |   223 ns/op | 14.43× | **31.86×** |
| string_contains_ladder |   73 ns/op |    58 ns/op |   201 ns/op |   353 ns/op |  3.47× |  **6.09×** |
| **k8s_admission_ish**  |  118 ns/op |    52 ns/op |   601 ns/op |  1167 ns/op | 11.56× | **22.44×** |

幾何平均 (arcel-AOT vs): cel-go ≈ **9×**、cel-cpp ≈ **14×**。
realistic K8s ValidatingAdmissionPolicy で cel-go 比 **11.6×**、cel-cpp 比 **22.4×**。
`bool_ladder` は constant-fold で `return 1;` 1 命令まで畳み込み、cel-cpp 比 **42.6×**。
`string_starts` は memcmp が AOT で literal 化されて cel-cpp 比 **31.9×**。

ベンチは benchmark-style: 1M 反復、bindings は再パースせず、interpreter / AOT /
cel-go の 3-way 比較。realistic K8s ValidatingAdmissionPolicy 式
(`object.spec.replicas <= maxReplicas && object.metadata.labels["team"] in allowedTeams && ...`)
で **4× の高速化**。

## サポート機能

CEL 仕様 (`docs/spec.md` 参照) のうち以下を実装済:

- リテラル: int / int64 / uint / double / bool / null / string / bytes / list / map
- 算術: `+ - * / %` (overflow 検査付き、cross-type promotion 含む)
- 比較: `== != < <= > >=` (cross-type 数値、bool ordering、string/bytes lex)
- 論理: `&& || !` (short-circuit + commutative-on-error)
- 三項: `cond ? a : b`
- 識別子・field access (`.`)・index (`[]`)
- list / map literal
- 標準関数: `size`, `type`, `int`, `uint`, `double`, `string`, `bool`, `bytes`, `dyn`,
  `startsWith`, `endsWith`, `contains`, `matches`
- マクロ: `has(...)`, `all`, `exists`, `exists_one`/`existsOne`, `filter`, `map`
- マクロ2 (cel-spec): `xs.<macro>(idx, val, body)` の二引数形式、
  `transformList`, `transformMap`

未対応 (スコープ外): protobuf message literal (`TestAllTypes{...}`),
google.protobuf.Any, timestamp / duration の strict spec, optional 型,
Wrapper 型 (proto)。

## インストール

### 前提パッケージ (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby
```

`make` 中で ASTroGen を呼ぶため Ruby 3.x が必要。本体はリンクが
`-ldl -lm` のみで GMP / GC / readline は不要。conformance / bench で
cel-go と比較する場合は別途 `golang-go` も入れておく (`make celgo` で
`go install` する)。

## 試す

```sh
make            # arcel バイナリ
make celgo      # cel-go reference (proxy.golang.org から DL、初回 ~30 s)
make smoke      # arcel の自己テスト
make test       # CEL 公式 conformance suite (proto/ext を除く 808 ケース)
make test-ref   # 同 suite を cel-go reference に流す (89.7% で天井)
make bench      # arcel-interp / arcel-AOT / cel-go を 11 ケース比較

# 単発 eval
./arcel eval -e '1 + 2 * 3'
./arcel eval -e 'x.foo + x.bar' -i '{"x":{"foo":10,"bar":32}}'
./arcel eval -e '[1,2,3].all(x, x > 0)'
./arcel eval -e 'has(x.foo) && x.foo.startsWith("hi")' -i '{"x":{"foo":"hi"}}'

# bench 1 ケース
./arcel bench -e '1 + 2 * 3' -n 1000000
```

## ドロップイン代替

cel-go と同じ JSON envelope を喋る `repl` サブコマンドを持つので、harness
からは binary を入れ替えるだけで切り替えられる:

```sh
echo '{"e":"1+2","i":null}' | ./arcel              repl
echo '{"e":"1+2","i":null}' | ./test/celgo_ref/celgo_ref repl
# どちらも `3` を出力
```

`run_conformance.rb` の `--bin` / `--use-ref` で切り替えて使う。

## アプローチ

CEL の本番ユースケース (K8s admission, Envoy authz, gRPC interceptor) は
**ポリシー固定 × 入力大量**というパターンで、ASTro の partial evaluation
の理想形。cel-go は純 interpreter (公式に AOT パスなし)、cel-cpp も同様、
OPA は Wasm AOT を持つが indirection 層が残る — arcel は standalone
ネイティブで「policy AST + 入力 layout」を specialize できる位置に
いる。

詳しくは [`docs/idea.md`](./docs/idea.md) (設計方針)、
[`docs/spec.md`](./docs/spec.md) (CEL 仕様マッピング)、
[`docs/perf.md`](./docs/perf.md) (ベンチ + ベンチ手順)、
[`docs/todo.md`](./docs/todo.md) (今後)。

## ディレクトリ

```
sample/arcel/
├── Makefile
├── README.md / docs/{idea,spec,perf,todo}.md
├── context.h            — VALUE 表現 (tagged union)、CTX、arena
├── value.{h,c}          — 演算 / 比較 / format / arena / ident lookup
├── input.{h,c}          — JSON 入力パーサ (per-eval value tree builder)
├── parser.{h,c}         — CEL 文法パーサ + マクロ書き換え
├── node.def             — 全 AST ノード (40+ 種、ASTroGen 入力)
├── node.h / node.c      — runtime + ASTroGen 接続
├── main.c               — CLI (eval / bench / repl サブコマンド)
├── test/
│   ├── conformance/     — google/cel-spec/tests/simple/testdata (30 file, 837 case)
│   ├── celgo_ref/       — cel-go ベース reference 評価器
│   ├── textproto.rb     — cel-spec textproto fixture parser
│   └── run_conformance.rb
└── benchmark/run.rb     — 3-way ベンチ (arcel-i / arcel-A / celgo)
```

## 残タスク

`docs/todo.md` 参照。CEL コア言語は出揃ったので、次フェーズは:

- AOT specialization のさらなる活用 (現状 interp と AOT がほぼ tied;
  field access 連鎖の struct offset 化、loop 展開、guard scope 最小化)
- `xs.matches(re)` の正規表現を `astrorge` backend で pre-compile
- map lookup の hash table 化 (現状は flat array linear scan)
- timestamp / duration / Wrapper の cel-spec 部分対応
