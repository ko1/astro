# ASTro サンプル横断分析

ASTro リポジトリ配下の `sample/*` を **言語特性** と **そこから導かれる
node.def 構成** を中心に横断分析した文書。各サンプル個別の詳細は
`sample/<lang>/README.md` および `sample/<lang>/docs/{done,todo,perf,runtime}.md`
を参照。本書は「17 サンプル並べて何が分かるか」を整理する
(汎用言語 16 + JSON フィルタ DSL の `nuq`)。

§6 でサンプルバイナリの **コマンドラインオプション** を横断比較、
§7 で各サンプルの `docs/perf.md` から **定量的な性能まとめ** を出し、
最後の §8 で、これだけサンプルが揃ったところで見えてきた
**ASTro フレームワーク自身の Pros / Cons** をまとめる。

---

## 1. 言語ラインナップ

サンプルが扱うソース言語の系統と、その規模感を 1 表で。

| sample | 元言語 | パラダイム | 静/動 | 数値 | 備考 |
|---|---|---|---|---|---|
| `calc` | (独自) | 算術式のみ | — | int32 | end-to-end 最小デモ (6 ノード) |
| `pascalast` | Pascal | 命令型, 静的 | 静 | int+real+set | record / 1D・2D 配列 / file I/O / try/except / OOP |
| `naruby` | Ruby サブセット | 命令型, 動的 | 動 (整数のみ) | int64 | **論文評価用** — 1 バイナリで 4 モード切替 |
| `abruby` | Ruby サブセット | OO, 動的 | 動 | CRuby 互換 | **CRuby C 拡張** (VALUE / Prism / GC を流用) |
| `koruby` | Ruby サブセット | OO, 動的 | 動 | int + GMP bignum + float | スタンドアロン全機能 Ruby、**optcarrot 完走** |
| `aforth` | Forth | スタックマシン, 静的 | 動 (cell 単位) | int64 | **すべての word (組み込み + ユーザ定義) が AST NODE** — 伝統的な threaded code を使わず ASTro 流に AST で表現 |
| `ascheme` | R5RS Scheme | 関数型 | 動 | 完全数値タワー (GMP 含む) | call/cc, multi-value, port, 完全な末尾呼出最適化 |
| `asom` | SOM (Smalltalk) | 純 OO, 動的 | 動 | int+double+bignum | AreWeFastYet 16 本完走 / SOM TestSuite 100% |
| `astocaml` | OCaml サブセット | 関数型, 静的 | 静 | int+float | variant / record / class / module / lazy / 末尾呼出最適化 |
| `astr` | R サブセット | 関数型, ベクタ | 動 | int+double+vec+str | tagged VALUE + libgc + ベクタ broadcast |
| `luastro` | Lua 5.4 | 命令型, 動的 | 動 | int + float | metatable / coroutine (ucontext) / weak table / `__gc` |
| `pystro` | Python 3 サブセット | OO, 動的 | 動 | int + GMP bignum + float | class / try-except / for-in / f-string / lambda |
| `jstro` | JavaScript (ES2023+) | OO, 動的 | 動 | small integer (SMI) + inline flonum | **V8 風 hidden class + inline cache (IC)**, Map/Set/Symbol/Proxy/Promise(sync) |
| `castro` | C サブセット | 命令型, 静的 | 静 | int64 / double / pointer | tree-sitter-c で型解決 → 1 slot=8byte レイアウト |
| `wastro` | WebAssembly 1.0+ | スタックマシン, 静的 | 静 | i32/i64/f32/f64 | WAT/WASM 両対応 / spec-test ハーネス |
| `astrogre` | (Onigmo 互換 regex) | DSL — 正規表現 | — | — | **マッチエンジン自体が AST**、`are` grep CLI 付属 |
| `nuq` | (jq 互換) | DSL — JSON フィルタ | — | tagged fixnum + `nuq_obj` | pipe / comma fan-out / `try-catch` / `reduce` / `foreach` / 70+ builtin |

パラダイム軸での広がり:
- **教育用最小**: `calc`
- **命令型 (古典)**: `pascalast` / `castro`
- **動的言語のメインストリーム**: `naruby` / `abruby` / `koruby` / `luastro` / `pystro` / `jstro`
- **関数型**: `ascheme` / `astocaml`
- **OO 純化**: `asom`
- **データ解析系**: `astr`
- **スタックマシン**: `aforth` / `wastro`
- **DSL / エンジン応用**: `astrogre` / `nuq`

直交する軸として **型システム** で切ると:
- **静的型** (parser-time に型確定): `pascalast` / `castro` / `astocaml` / `wastro`
- **動的型**: 動的言語勢 6 つ + Scheme + Smalltalk + R + Forth (cell 単位 untyped)
- **型なし / DSL**: `calc` / `astrogre` / `nuq`

静的型 4 つはそれぞれ違う方向 — Pascal (古典手続き型 + variant record),
C (低レベル ABI + ポインタ), OCaml (Hindley-Milner + variant + class),
Wasm (型付きスタックマシン) — を扱っており、§3.1 で見るように
**node.def 上の算術ノード分裂パターン** が型ごとに異なる形で揃う。

「ツリー解釈による言語実装フレームワーク」という前提に対して、
**スタックマシン (aforth/wastro) と DSL 応用 (astrogre)** が乗ったのが面白い。
特に astrogre は「regex のマッチング過程そのものを AST にして、外側の
スキャンループも 1 つのノードに包んで特化器でひとまとめに焼く」という、
ASTro が想定外でも嵌まる例になっている。

---

## 2. node.def 規模 と コードボリューム

| sample | NODE_DEF 数 | node.def 行数 | C コード行数 (生成除く) | テスト | ベンチ |
|---|---:|---:|---:|---:|---:|
| `calc`      |   6 |    36 |    227 |   - |   - |
| `naruby`    |  36 |   571 |  3,175 |   - |  37 |
| `astr`      |  46 |   578 |  2,028 |  18 |   4 |
| `astrogre`  |  53 | 1,612 |  3,678 |   - |  11 |
| `ascheme`   |  54 |   778 |  4,341 |  34 |  19 |
| `nuq`       |  57 |   407 |    -   | 338 |   - |
| `aforth`    |  68 |   639 |    851 |   7 |  10 |
| `luastro`   |  74 | 1,448 |  5,086 |   9 |  24 |
| `asom`      |  80 | 1,262 |  5,327 |  29 |   - |
| `koruby`    |  90 | 1,402 |  8,603 | 175 |  27 |
| `pystro`    |  91 | 1,390 |  4,737 |  55 |   6 |
| `astocaml`  |  91 | 1,196 |  6,540 |  70 |  26 |
| `castro`    | 101 | 1,019 |  1,245 |   - |   - |
| `jstro`     | 101 | 1,991 |  9,680 |   5 |  14 |
| `abruby`    | 107 | 3,910 |  2,888 |  43 | 193 |
| `pascalast` | 159 | 1,968 |  5,290 |  99 |  17 |
| `wastro`    | 212 | 2,032 |  5,861 |   - |   - |

(C コード行数列とテスト/ベンチ列は表作成時のスナップショットで、最新値とずれている
ことがある — node.def の数値だけ保守。)

観察:
- **「ノード数 ≒ 言語の表現力」ではない**。動的言語は call/算術を 1 つに
  畳む傾向にあり (`naruby` の 32 ノードで Ruby サブセットが書ける)、
  逆に静的型 (`pascalast`, `wastro`) や型ごとに per-op を持つ
  バイトコード写経 (`wastro`) はノード数が膨れる。
- `pascalast` の 159 ノードは、`int / real / string / 1D 配列 /
  2D 配列 / set / file I/O / OOP / variant record` を独立に opcode 化
  したため。逆に `castro` は同じ静的型でも 101 で済む — 値が `union { i, d, p }`
  に収まり、配列もポインタ型 1 つで処理できるから。
- `wastro` 212 はほぼ wasm の opcode 表と一対一。スタックマシン
  バイトコードを「ノード = opcode」と見れば自然な数。
- `aforth` 68 ノードで C 851 行、`castro` 101 ノードで C 1,245 行と
  **実装サイズが小さい言語** が成立しているのは、ASTro が EVAL ロジック
  以外を全部生成側に持っていく結果。

---

## 3. node.def の構成パターン (本題)

「言語の特質」と「node.def での表現」のマッピングを軸別に整理する。

### 3.1 算術ノードの分裂 — 動的型 と 静的型 の分岐

すべての言語に「足し算」はあるが、その表現は **言語の型システム** で
ほぼ決まる。

#### (a) 単型 — `node_add(NODE *l, NODE *r)` 1 個

`calc` / `naruby` / `aforth` (Forth は cell 単位 64-bit 整数で型なし) /
`astr` (整数は fixnum タグ + libc に流す) のように、値が **実質 1 種類**
の言語ではノードも 1 つで終わる。最も素朴。

#### (b) 静的型 — 型ごとに別ノード

`castro` の `node_add_i / node_add_d`、`pascalast` の `node_add / node_radd`、
`astocaml` の `node_add / node_add_int / node_fadd`、
`wastro` の `node_i32_add / i64_add / f32_add / f64_add` ... のように、
**parser-time に既に型が決まっている** 場合は別ノードに分ける。

利点: EVAL body が 1 行 (`return l + r`) になり、特化後は C コンパイラの
**SROA** (Scalar Replacement of Aggregates — struct/union/小配列を、
アドレスを取らない範囲で個別のスカラ変数に分解する最適化パス) が
各メンバを浮動小数 (xmm) / 整数 (汎用) レジスタに昇格させられる。
`wastro perf.md` が報告する mandelbrot/nbody の inner-loop 性能は
このパターンに依存。

#### (c) 動的型 — fast/slow 二段構え + IC 駆動の kind swap

ここで言う **kind swap** は、AST ノードのディスパッチャ関数ポインタを
別のノード kind のものに **その場で差し替える** 操作 (`n->head.kind = &kind_node_xxx;`
+ dispatcher 更新)。AST 構造はそのままで、評価ロジックだけが切り替わる。
動的言語の inline cache 機構の核として全動的言語サンプルが採用している。

`abruby`, `jstro`, `luastro` は値が動的型なので、parser-time には
「整数だろう」と仮置きし、ミスったら slow path にフォールバックする。

`abruby` の例 (107 ノード中 35 個が `fixnum_*` プレフィックス):

```
node_plus
node_fixnum_plus            ← parser が IC で吐く fast path
node_fixnum_plus_overflow   ← __builtin_add_overflow チェック付き
node_fixnum_plus_slow       ← Bignum 昇格 / 例外の重い経路
node_integer_plus           ← Bignum 同士
node_flonum_plus
```

`jstro` は parser 後に `node_smi`/`node_smi_add_ii`/`node_smi_lt_ii` など
の SMI 専用ノードに kind swap で置き換える (V8 の inline cache 様式)。
miss するとノード kind が `node_add` (汎用) に戻る。

`luastro` は `node_int_add_ii` (整数×整数) / `node_flt_add_ff` (float×float)
/ `node_int_arith_ii_miss` (フォールバック) の三役を **kind swap 1 命令** で
切り替える。`luastro/node.def` の `@canonical=node_int_add` 注釈で
**Merkle hash を ii / ii_miss で同一化** しているので、特化キャッシュが
共有される。

→ §3.4 「`@canonical=` の使いどころ」を参照。

#### (d) Smalltalk 流 selector-typed send

`asom` は `send_N` を **selector で再特化** してしまう独自路線:

```
node_send1                  ← 一般 1 引数 send
node_send1_intplus          ← レシーバ Integer + selector # +
node_send1_intminus
node_send1_intlt
node_send1_dblplus          ← レシーバ Double + selector # +
node_send1_arrayat          ← Array #at:
node_send2_arrayatput       ← Array #at:put:
```

(全 `send1_*` は `@canonical=node_send1`。これも §3.4。)

純粋 OO 言語では「`+` も `<` も `Array#at:` も全部 send」なので、
`send_N` ファミリを selector × 受け手型で広げて静的特化する。
**算術と添字アクセスを同じ機構に乗せる** SOM 流の特徴がそのままノード型に
表れている。

### 3.2 関数呼び出し — arity-specialized `call_N`

| 言語 | 形 |
|---|---|
| `naruby` | `call_0..3` |
| `ascheme` | `call_0..4` + `call_n` |
| `astr` | `call_0..3` + `call_n` |
| `pystro` | `call_0..3` + `call_n` |
| `pascalast` | `pcall_0..3` + `pcall_n` (+ `_baked` 変種) |
| `wastro` | `call_0..4` + `call_var` (関数 import 用 `host_call_*` も同形で 0..3+var) |
| `asom` | `send_0..8` (Smalltalk の selector arity 上限が小さいので thinner tail) |

**理由は ASTro の特化機構との相性**: 引数を NODE * オペランドとして
固定 arity で受けると、特化時に各引数評価がディスパッチャごと SD に
インライン展開される。可変引数 (`call_n`) は side-array 経由になるので
特化の利益が一段下がる。だからホットな低 arity だけ別ノードに切り出す。

`abruby` は `node_call0/1/2` + `dispatch_method_*` ヘルパで似たことを
やるが、`@canonical=node_call0` などで hash を統合し、
**動的に IC 状態 (with_block / no_block / pgo) で kind swap** している。

### 3.3 制御フロー融合 — asom と wastro の対極

#### asom の "fused if/while + pool" シリーズ

```
node_iftrue
node_iffalse
node_iftrue_iffalse         ← if-then-else
node_iffalse_iftrue
node_iftrue_pool            ← block レシーバ がリテラルブロックの場合 (#whileTrue:)
node_iftrue_iffalse_pool
node_whiletrue              / node_whiletrue_pool
node_whilefalse             / node_whilefalse_pool
node_times_repeat           / node_times_repeat_pool
node_to_do                  / node_to_do_pool
node_to_by_do               / node_to_by_do_pool
```

Smalltalk は構文レベルでは制御フローを持たない (`#ifTrue:` も
`#whileTrue:` も普通のメッセージ) ので、ナイーブに書くと 1 イテレーションで
何度も `send` が走る。asom は parser-pass で **「メッセージ送信だが
レシーバはブロックリテラル」** と検出した時、**専用ノードに lower** する。
`_pool` 接尾辞は「ブロックフレームを per-bucket free-list で再利用する」
バリアント。

#### wastro の "block / loop / br / br_if / br_table"

wasm は構造化制御 (`block/loop/if/end`) と相対 br depth でジャンプを
表現するため、`node_block`, `node_loop`, `node_br`, `node_br_if`,
`node_br_table`, `node_return` の **6 ノードで全制御フロー** が乗る。
return を持つ block は `_v` (with-value) サフィックスで分ける。

```
node_block / node_loop
node_br / node_br_v
node_br_if / node_br_if_v
node_br_table / node_br_table_v
node_return / node_return_v
node_select
```

両者の対比:
- **asom**: 制御フローを持たない言語 → parser が文脈を見て **大量の融合
  ノードに lower** する
- **wastro**: 制御フローが構造化されている言語 → ノードは少なく、
  実行時ペイロード (depth / index) で分岐先を決める

### 3.4 `@canonical=` で hash を統合 — 特化キャッシュ共有

`@canonical=other_node` 注釈は「**この変種は other_node と Merkle hash を
共有する**」というマーク。ASTro の SD コードストアはハッシュ keyed なので、
これで **kind swap で性質を切り替えても、特化バイナリを同じものを
使い回せる**。

usage の上位:
- `abruby`: 42 件 (整数/浮動小数の fast/slow 切替、call との互換)
- `asom`: 19 件 (`send_N` の receiver-type 別バリアントすべて)
- `luastro`: 10 件 (int_/flt_/_miss の三役)
- `jstro`: 3 件 (smi_add_ii ↔ add ペア)

→ **動的型言語の "型 IC" 機構の核**。ASTro の Merkle ハッシュは構造的に
同じ AST を識別するためのものだが、`@canonical` で「**論理的に同じ
構造**」のマーキングをユーザがオーバライドできるのが効く。

### 3.5 `@always_inline` / `@noinline` — 特化バイナリの形を制御

| sample | @always_inline | @noinline |
|---|---:|---:|
| `aforth`   | 22 | 4 |
| `jstro`    | 67 | 1 |
| `luastro`  | 46 | 9 |
| `wastro`   |  8 | 0 |
| `astocaml` |  0 | 11 |
| `koruby`   |  0 | 16 |
| `pystro`   |  0 | 11 |

`@always_inline` を多用するのは **個々のノードが極小** で、特化チェーンを
最後まで畳んで初めて利益が出るタイプ (Forth, JS, Lua の SMI 算術)。
逆に `@noinline` を多用するのは **メソッド境界で SD を切りたい** タイプ
(関数型: 関数本体が SD 単位、Ruby/Python: メソッド本体が SD 単位)。

このメリハリは **partial evaluation の単位選択** をユーザが node.def で
コントロールしている、という ASTro 設計の特徴がよく出ている。

### 3.6 `@ref` — IC 状態など mutable 副情報

| sample | @ref ノード数 |
|---|---:|
| `ascheme`  | 25 |
| `abruby`   | 22 |
| `naruby`   | 10 |
| `jstro`    | 10 |
| `luastro`  | 10 |
| `pystro`   |  8 |
| `astocaml` |  7 |

`@ref` は **ノード struct の inline 領域** に struct を埋め込みつつ、
EVAL にはそのアドレスを渡す (`T *foo@ref` 宣言 → struct 内は `T foo` で
領域確保、EVAL 引数は `&n->u.xxx.foo` の pointer) という持ち方をする
オプション。記憶域はノードに付随するが、**Merkle hash には 0 を寄与
させる** (per-sample の `hash_call` で `return "0" if ref?`) ので、
IC のフィールドが更新されても特化キャッシュは無効化されない。
alloc 時には `memset(0)` で初期化される。

→ ascheme の lref キャッシュ、abruby のメソッドキャッシュ、
jstro の hidden class shape ID、luastro の closure call IC など、
**「ノード自身に紐づくが、構造ハッシュからは隠したい」** mutable 副情報の
置き場として全動的言語が利用。

### 3.7 「分岐木」ではなく「継続チェーン」の AST — astrogre 一例

実は ASTro の通常ノードも `EVAL_ARG(c, child)` で次の評価先を指定して
いるので、広い意味では全部 continuation-passing と呼べる。`node_if(c, t, e)`
は「`c` を評価し、結果で `t` か `e` のどちらに継続するかを決める」と
解釈できる。

`astrogre` が特殊なのは、**AST 形が分岐木ではなく一本鎖** なところ。
正規表現 `ab` は構造的には `seq(lit("a"), lit("b"))` でも書けるが、
astrogre は parser-pass で **right-to-left lower** して

```
lit("a", next = lit("b", next = succ))
```

という **`next` ポインタ 1 本の chain** に変換する:

```
node_re_lit    (lit, next)            ← マッチしたら next を呼ぶ
node_re_dot    (next)
node_re_alt    (l, r, next)
node_re_rep    (body, min, max, greedy, next)
node_re_cap_start (id, next)
node_re_cap_end   (id, next)
node_re_succ                           ← 終端: マッチ成功
```

何が嬉しいか:
- 通常の AST だと `seq` ノードを介して 2 段降りるので、特化チェーンが
  `seq → lit → seq → lit → ...` になる
- chain 形だと `lit(next=lit(next=...))` で 1 段ずつなので、特化器が
  **next を末尾まで一気に inline** して 1 つの SD に畳める
- さらにスキャナ層 (`node_grep_search_*`) も `body` operand で chain を
  包むので、**スキャンループ自体まで同じ SD に融合** される

regex / VM dispatch / parser combinator のような「**アクション列を上から
線形に実行する**」エンジンを ASTro に乗せるときの定石。AST 木を
あえて linked-list に潰すことで、特化単位を稼ぐ。

### 3.8 typed-slot union frame — wastro 一例

`wastro/context.h` の `union wastro_slot { int32_t i32; int64_t i64;
float f32; double f64; uint64_t raw; }`。

各ローカルスロットを `union` で持ち、`node_local_get_i32` / `_i64` /
`_f32` / `_f64` で別ノードを使い分ける。

→ EVAL body は `return frame[idx].i32;` の 1 行。SD 化すると **gcc の
SROA (§3.1(b) で説明済) が各 slot を分解してネイティブ型のレジスタに
promote** する。AST 解釈なのにスタックマシン JIT 風の速度が出るのは
この構造のおかげ。

`uint64_t[]` + memcpy reinterpret では gcc の alias 解析が躊躇うことが
あるので、**union を明示する** のがポイント (`docs/perf.md §1`)。

---

## 4. 言語別「ランタイム機構」分布

ノード以外の部分。各サンプルが内製している runtime コンポーネント。

| sample | 値表現 | GC | パーサ | 特殊機構 |
|---|---|---|---|---|
| `calc` | `int64_t` | なし | 自前再帰下降パーサ | — |
| `naruby` | `int64_t` | leak | Prism (CRuby パーサ) | **L0/L1/L2 JIT デーモン** |
| `abruby` | CRuby `VALUE` | CRuby GC (Ruby 拡張) | Prism (lib/abruby.rb) | Fiber / require / 完全ライブラリ |
| `koruby` | CRuby 互換 (FIXNUM/FLONUM/SYMBOL) | libgc (Boehm) | Prism | state-propagation 例外, 共有 fp closure |
| `astr` | tagged `int64` (low-bit fixnum) | libgc | 自前再帰下降パーサ | ベクタ broadcast |
| `pystro` | tagged `int64` (low-bit fixnum) | libgc | 自前 lexer + parser | GMP bignum, class, try-except |
| `ascheme` | tagged `int64` | libgc | S 式 reader → 構文ツリー → AST | 完全な末尾呼出最適化トランポリン, call/cc, 多値, port |
| `astocaml` | tagged `int64` | libgc | 自前 lexer + parser | クロージャ環境チェイン, lazy, class/module |
| `asom` | tagged `intptr_t` | libgc | 自前再帰下降 (`asom_parse.c`) | per-bucket free-list frame pool |
| `aforth` | `int64_t` (data stack cell) | なし | 自前 tokenizer | DO-loop frame stack 並列, vars[] エリア |
| `luastro` | tagged `LuaValue` (uint64_t) | 自前 mark-sweep GC | 自前 lexer + 再帰下降 + Pratt 式パーサ | metatable, **ucontext coroutine**, weak table, `__gc` |
| `jstro` | tagged `JsValue` (uint64_t) | 自前 mark-sweep GC + safepoint | 自前 lexer + parser | **hidden class IC, shape transition, closure box** |
| `castro` | union `{i, d, p}` | leak | tree-sitter-c (Ruby 側で IR 構築) | 1 slot = 8 byte の slot メモリモデル, goto-dispatch |
| `pascalast` | `int64_t` | libgc | 自前 lexer + parser | display 配列 (nested proc), variant record |
| `wastro` | `uint64_t` (raw bits) | leak | WAT tokenizer + 2 系統 (S 式 / stack-style) + .wasm decoder | typed slot union frame, spec-test runner |
| `astrogre` | `int64_t` (内部表現) | leak | 自前 regex parser | Aho-Corasick prefilter, Boyer-Moore-like memmem, scanner ノード |

注目点:
- **値表現は 4 系統**: 純 `int64`, low-bit fixnum tagged, CRuby 互換 (3-bit tag),
  `union {i, d, p}`。tagged が一番多い (動的言語の標準解)。
- **GC は libgc が主流** (5 サンプル)。**自前 mark-sweep** は luastro と jstro
  だけ — どちらも weak table / shape table のような "GC が見るべきだが
  conservative scan を逃したい" 構造を持つ言語で、自前にする動機が立つ。
- **パーサ**: Prism (`naruby`, `abruby`, `koruby`) を使うのは Ruby 系
  3 つだけ。残り全部は **手書き再帰下降** か (`castro` だけ)
  **tree-sitter-c**。ASTro 自体はパーサに何の制約も置かない。
  なお `luastro` は中置式の優先順位処理に **Pratt パーサ** (Vaughan Pratt
  方式の top-down operator-precedence parsing — 各演算子トークンに左/右の
  結合力を持たせて再帰下降の中で混在させる手法、Lua 本家 `lparser.c` と
  同じやり方) を併用している。
- **特殊な runtime**: jstro の hidden class IC (V8 風)、luastro の coroutine、
  asom の frame pool、castro の slot メモリモデル、pascalast の display
  配列 ... と、**フレームワーク本体には何も入っていない言語固有機構** が
  各サンプルで自由に作れている。

---

## 5. 実行モード対応表

ASTro は plain interpreter / AOT / Profile-Guided / JIT の 4 モードを
想定するが、**全モード対応は naruby だけ** (paper の評価対象として
そう作った)。

| sample | interp | AOT (`-c`) | PG | JIT |
|---|:-:|:-:|:-:|:-:|
| `calc`      | ✓ | ✓ |   |   |
| `naruby`    | ✓ | ✓ | ✓ | ✓ |
| `abruby`    | ✓ | ✓ | ✓ |   |
| `koruby`    | ✓ | ✓ |   |   |
| `aforth`    | ✓ | ✓ |   |   |
| `ascheme`   | ✓ | ✓ | ✓ |   |
| `asom`      | ✓ | ✓ | ✓ |   |
| `astocaml`  | ✓ | ✓ |   |   |
| `astr`      | ✓ | ✓ |   |   |
| `luastro`   | ✓ | ✓ | ✓ |   |
| `pystro`    | ✓ | ✓ |   |   |
| `jstro`     | ✓ | ✓ | ✓ |   |
| `castro`    | ✓ | ✓ |   |   |
| `pascalast` | ✓ | ✓ |   |   |
| `wastro`    | ✓ | ✓ |   |   |
| `astrogre`  | ✓ | ✓ |   |   |

PG をやっているのは `abruby` / `ascheme` / `asom` / `luastro` / `jstro` / `naruby`。
JIT は `naruby` のみ (L0/L1/L2 デーモンの試作)。

→ **ASTro のキー機構 (Merkle hash 起点の AOT bake) は全サンプルが使う**
が、PG は「IC 情報を特化器に渡す」発想が必要な言語、JIT は naruby に
集中している、という分布。

---

## 6. コマンドラインオプション横断

各サンプルバイナリの実行モード切替・dump・quiet 等を横並びで見る。
**フレームワーク標準が無いのでフラグ名は割と不統一**。これも 1 つの
発見。

### 6.1 共通カテゴリ — どのフラグでどのモードに入れるか

| sample | code store 無効 | AOT bake → run | AOT bake → exit | PG bake | code store クリア | AST dump | quiet |
|---|---|---|---|---|---|---|---|
| `calc`      | `--no-compile` | (default) | — | — | — | — | `-q` |
| `naruby`    | `-i` / `--plain` | `-c` / `--aot` / `--aot-compile-first` | `--aot-compile` | `-p` / `--pg` | `--ccs` | — | `-q` |
| `abruby`    | `--plain` | `-c` / `--aot-compile-first` | `--aot-compile` (mode) | `-p` / `--pg` | `--clear-code-store` / `--ccs` | `--dump[=MODE]` / `-d` | (none) |
| `koruby`    | (default 起動で SD 無し) | `--aot-compile` | `-c` (= node_specialized.c 出力) | — | (env のみ) | `--dump` | `-q` |
| `aforth`    | `--no-compile` | `--aot-compile` (compile→run) | — | — | — | `--dump-ast` | `-q` |
| `ascheme`   | (default plain) | `-c` / `--compile` | — | `--pg-compile` / `--pg` | `--clear-cs` | — | `-q` |
| `asom`      | `--plain` | `-c` / `--aot-compile-first` | — | `-p` / `--pg` | (none) | `--dump-ast` | `-q` |
| `astocaml`  | `--no-compile` | `-c` / `--compile` | — | — | — | — | `-q` |
| `astr`      | `-i` / `--plain` | `-c` / `--aot` | `--aot-compile` | — | `--ccs` | `--dump-ast` | `-q` |
| `luastro`   | `--no-compile` | `-c` / `--aot-compile-first` | `--aot-compile` | `-p` / `--pg-compile` | — | `--dump-ast` | `-q` |
| `pystro`    | `--no-compile` | `-c` | `--aot-compile` | — | — | `--dump-ast` | `-q` |
| `jstro`     | `--no-compile` | `-c` / `--aot-compile-first` | `--aot-compile` | `-p` / `--pg-compile` (≡ `-c`) | — | `--dump` | `-q` |
| `castro`    | `--no-compile` | `-c` / `--compile-all` | — | — | — | `--dump` | `-q` |
| `pascalast` | `--no-compile` | (`-c` のみで bake、走らせない) | `-c` | — | — | `--dump-ast` | `-q` |
| `wastro`    | `--no-compile` | `-c` (compile→run) | `--aot` / `--aot-compile` | — | `--clear-cs` / `--ccs` | — | `-q` |
| `astrogre/are` | (default; opt-in `--aot`) | `--aot` | — | — | — | `--dump PATTERN` | `-q` (grep `-q`) |

### 6.2 命名の不統一が見える

同じ意味のフラグがサンプルごとに名前違い:

- **「code store を引かない」**:
  - `--no-compile` 系: `aforth` / `astocaml` / `castro` / `jstro` / `luastro` / `pascalast` / `pystro` / `wastro` / `calc`
  - `--plain` 系: `abruby` / `asom`
  - `-i` / `--plain` 系: `naruby` / `astr`
  - default off: `ascheme` / `astrogre/are` (opt-in が `--aot`)
- **`-c` の意味が違う**:
  - 「bake してから run」: `naruby` / `abruby` / `ascheme` / `asom` / `astocaml` / `astr` / `luastro` / `pystro` / `jstro` / `castro` / `wastro`
  - 「bake のみで exit」: `pascalast` / `koruby` (後者は `node_specialized.c` を吐く別機構)
  - 「count マッチ件数」(grep 流): `astrogre/are`
- **`--aot-compile`** も「compile→run」(aforth) / 「compile→exit」(jstro/luastro/pystro/wastro/abruby) と分かれる
- **PG モードの呼び名**: 持っているのは 6 サンプル (abruby / ascheme / asom / luastro / jstro / naruby) だけだが、その中でも `--pg-compile` / `--pg` / `-p` のどれが alias でどれが別物かは差がある。`jstro` の `-p` は **現状 `-c` と同等の no-op** とコメントされている

これは「lib/astrogen.rb 側で標準オプションパーサを生やす」案の動機。
今は各サンプル main.c で argv loop を自前で書いていて、規約が揺れる。

### 6.3 サンプル固有オプション

各サンプルにしかない / その言語の都合で固有に増えたフラグ:

| sample | 固有オプション | 意味 |
|---|---|---|
| `naruby` | `-j` | **JIT モード** (L1 デーモンへの UDS 接続) |
| `naruby` | `-s` | static-lang モード (parse-time call resolution) |
| `naruby` | `-b` | AOT/PG どちらの bake もスキップ |
| `asom` | `-cp PATH` / `--classpath` | SOM の `.som` 検索パス |
| `asom` | `--preload=...` | bake 前に追加クラスを eager load |
| `asom` | `--pg-threshold=N` | PG bake のディスパッチ回数閾値 (env `ASOM_PG_THRESHOLD`) |
| `abruby` | `--pg-threshold=N` | 同上 (env `ABRUBY_PG_THRESHOLD`) |
| `abruby` | `--code-store=DIR` | code store 場所 (env `ABRUBY_CODE_STORE`) |
| `abruby` | `--compiled-only` | デフォルトディスパッチャに落ちたら abort |
| `abruby` | `--aot-only` | PGC index 引かず AOT のみロード |
| `abruby` | `--run` | `--aot-compile` 後に走らせるファイルを区切る |
| `astocaml` | `-T` / `--check` / `--no-check` | 型検査の on/off |
| `aforth` | `--no-codegen` | 特化 SD を生成しない (load のみ) |
| `castro` | `--no-spec` | 同上 |
| `castro` | `--sx` | **入力を S 式 IR で受ける** (parse.rb をスキップ) |
| `pascalast` | `--no-run` | parse 後に実行しない (bake 専用パス) |
| `wastro` | `--test FILE.wast` | wasm spec-test ハーネス |
| `jstro` | `--show-result` | top-level の最終式を print |
| `jstro` | `--dump-ic` | exit 時に IC / GC カウンタを出す |
| `astrogre/are` | `--engine=astrogre|onigmo` | バックエンド差し替え (head-to-head 比較用) |
| `astrogre/are` | `--encoding=utf-8|ascii` | regex エンコーディング |
| `astrogre/are` | grep フラグ群 (`-i -n -c -v -w -F -l -L -H -h -o -A -B -C -m -e -f -t -T --include --exclude --hidden --no-ignore -a --no-recursive -j N --color`) | grep 互換 |

### 6.4 入力経路のバリエーション

ファイル/コードの渡し方も微妙にバラバラ:

- **`-e <code>` で文字列実行**: `abruby` / `ascheme` / `koruby` / `luastro` / `pystro` (REPL/one-liner で重宝)
- **stdin から読む** (`-`): `ascheme` のみ
- **REPL モード**: `calc` (引数無しで起動)、`ascheme` (script 無しで起動)
- **クラス名 / モジュール名指定** (file path ではない): `asom` (`asom <ClassName>`)、`wastro` (`wastro module.wat <export> [args...]`)
- **`--` 末尾 sentinel**: `ascheme` / `luastro` / `pystro` (option 終端、以降は script ARGV)

### 6.5 環境変数

CLI に出ないが環境変数で挙動を変えるもの:

- `ABRUBY_CODE_STORE` / `ASOM_CODE_STORE` / `KORUBY_CODE_STORE` —
  code store 配置場所
- `KORUBY_SRC_DIR` — koruby の source 検索場所
- `ABRUBY_PG_THRESHOLD` / `ASOM_PG_THRESHOLD` — PG bake 閾値
- `CCACHE_DISABLE=1` — bake 時の ccache 回避 (各サンプル共通の罠)

### 6.6 標準化への余地

サンプル横断で見ると、**フレームワーク側で `--no-compile` / `-c` /
`-p` / `--clear-cs` / `--dump-ast` / `-q` / `-v` / `-h` は共通フラグ
として lib 側で生やす** のが筋。今は各 main.c の手書き argv loop で
微妙にずれている。`koruby gen.rb` 等の per-sample subclass で追加
フラグだけ載せる形に揃えると、`abruby --plain` と `naruby --plain`
で意味が一致したり、`-c` の意味揺れが消えたりして、サンプル間の
学習コストが下がる。

---

## 7. 性能まとめ — どこで何にどれだけ勝てているか

各サンプルの `docs/perf.md` から最新の代表値を抜き出して横並びにする。
**注意点を先に**:

- 比較対象が言語ごとに違う (host JIT / bytecode VM / 同言語の native
  AOT / 自前 interp baseline)。「速い」が指す相手は表ごとに別もの
- bench セットも揃っていない (3〜15 本、種類もまちまち)。サンプル間で
  「何倍」を直接比べるのは意味が薄い
- 単位混在 (`aforth` `astrogre` `castro` は ms、他は秒)
- 詳細・methodology は各 `sample/<lang>/docs/perf.md` を参照

### 7.1 ベンチ vs リファレンス エンジン まとめ

| sample | リファレンス | bench 数 | AOT vs リファレンス |
|---|---|---:|---|
| `aforth` | gforth (成熟した direct-threaded Forth) | 9 | **9/9 勝**、最大 15× (gcd) / 8× (factorial) / 7× (collatz) |
| `ascheme` | chibi-scheme 0.12 / guile 3.0 (JIT) | 7 + 11 | **vs chibi 18/18 勝** (1.5–7.5×)、**vs guile 17/18 勝** (matmul のみ 1.2× 負け、最大 27×) |
| `pystro` | CPython 3.12 | 6 | **6/6 勝**、1.13× (dict) — 19× (while loop) |
| `asom` | SOM++ (g++ -O3 bytecode VM) / TruffleSOM (Graal JIT) | 12 | **vs SOM++ 11/12 勝**、PG なら **12/12 勝**、最大 10× (Sieve)。Truffle warm peak には 4–28× 負け |
| `abruby` | CRuby 4.0.2 / YJIT | 15 + optcarrot | **vs CRuby 整数ループ系 4–8×**、optcarrot で +90% (PGC で 86.5 fps vs CRuby 45.6 fps)。YJIT は recursive で 1.5–2× 先 |
| `koruby` | CRuby 4.0 / YJIT | optcarrot | AOT-PGO で **112 fps** (CRuby 42 fps の 2.65×、YJIT 178 fps の 0.63×) |
| `naruby` | gcc -O0..-O3 (同等 C) + CRuby/YJIT | 15 | **gcc -O3 と ≤1.1× が 5/15** (gcd, compose, collatz, early_return, prime_count)。fib/ackermann/tak のみ 4–13× 差 |
| `astocaml` | OCaml 4.14 (toplevel/bytecode/native) | 5 | **toplevel 5/5 勝**、**bytecode 3/5 勝**、native (ocamlopt) は 3.5–20× 先 |
| `castro` | gcc -O0/-O1/-O3 (同 source) | 11 | **-O0 を 3 本上回り**、crc32 で **-O1 と 1.11× タイ**。-O3 への残ギャップ 3–5× |
| `wastro` | native gcc -O2 / wasmtime (Cranelift JIT) | 3 | native に 3–6× 負け、wasmtime とはループでタイ・call で負け |
| `jstro` | node v18 (V8 TurboFan) | 13 | **try/catch 45×、cold-start 53×、Redux 系 2.25×、large sieve 2.45×** で勝ち。fib/mandel/nbody は 3–14× 負け |
| `astrogre` | ripgrep / GNU grep / Onigmo | 17 | **vs Onigmo 8/8 勝** (3–15×)、**vs grep 6/8 勝** (最大 10×)、ripgrep には 3/8 + 1 タイ (識別子系で 10× 負け) |
| `pascalast` | (外部 Pascal なし、自 interp との比較のみ) | 4 | interp → AOT で 2× (recursive) 〜 25× (tight loop) |
| `aforth` ~ `wastro` の interp 列は省略。各 perf.md の表参照。 |

### 7.2 速度をどこで稼げているか (パターン別)

定量数値を縦に見ると、**ASTro の AOT が大きく勝てる場面 / 勝てない場面**
がはっきりする。

#### (a) 大勝するパターン

- **tight inner loop で型が parser-time に確定**: aforth (gcd 15×),
  pystro (while_loop 19×), ascheme (sumloop 20× vs guile),
  asom (Sieve 10×), naruby (loop で C コンパイラがループ自体を消す)
- **インタプリタ起動が遅い処理系との比較**: jstro vs node は
  cold-start で **53×** 勝つ (V8 の tier-up が間に合わない領域)
- **バイトコード VM 全般**: SOM++ / chibi-scheme / gforth のような
  "成熟したが JIT を持たない" 処理系には全勝に近い
- **Onigmo に対する astrogre**: AC prefilter + AOT で 12-way alt が
  特化されると Onigmo を 7× 引き離す
- **CRuby (no-JIT)**: 整数ループ系で 4–8× — abruby/koruby/naruby
  全部に共通

#### (b) 互角〜辛勝のパターン

- **ocamlopt / gcc -O3 系 native AOT** との比較: 同じ C ベース AOT 同士
  なので大きな差は出ない。castro `crc32` が gcc -O1 タイ、naruby が
  gcc -O3 と 5/15 タイ、astocaml が ocamlc bytecode に 3/5 勝
- **native code をバイパスできない部分** (heap allocation 集中、
  GC pause、文字列処理): abruby/koruby は string/binary_trees で
  CRuby に 1.5–2× 負け

#### (c) 大きく負けるパターン

- **TurboFan の数値最適化**: jstro vs node の mandelbrot/nbody/fib は
  V8 の type feedback + escape analysis が支配的で 3–14× 差
- **Graal の partial escape analysis**: asom warm peak は TruffleSOM に
  4–28× 負ける。Truffle の "全部 inline + escape elision" には届かない
- **recursive call の deep nest**: 関数呼び出し境界の indirect dispatch が
  残るので、純 recursive (fib/ack/tak) は naruby/koruby/abruby ともに
  gcc -O3 / YJIT に水を空けられる
- **ripgrep の Aho-Corasick / Teddy multi-literal**: アルゴリズム差で
  astrogre が識別子パターンで 10× 負け (ASTro 関係なく engine 設計の問題)

### 7.3 「ASTro AOT がどこに位置づくか」 1 行サマリ

実測ベンチを横断すると、ASTro AOT の性能ポジションは概ね:

> **「成熟した bytecode VM や non-JIT インタプリタを 2–10× 突き放し、
> 同言語の C ベース native AOT (ocamlopt / gcc) には 1.5–5× 残ギャップ。
> 動的言語の本気 JIT (V8 TurboFan / Graal) には 3–28× 負ける」**

そう聞くと地味に響くが:

- **「処理系作成コスト 1/10 で V8/Graal のクラスを目指せる」フレームワーク**
  ではなく、**「処理系作成コスト 1/10 で bytecode VM クラスの倍速を出せる」
  フレームワーク** という現実的な位置取り
- 教科書的な "tree-walking interpreter は遅い" の壁は確実に超えている
  (interp 比 5–25× が普通)
- bytecode VM を書いた場合の最適化労力 (peephole / inline cache /
  super-instruction / 命令ディスパッチ最適化…) と比べると、`node.def`
  + AOT bake で同等以上が出るのは破格

### 7.4 補足: 実測で気付かれている注意

各 perf.md からの拾い物で、サンプル横断で意識しておきたい点:

- **bench は ~1 秒スケールで取る**。ms 級だと setup-bound でノイズが
  支配する (memory にも保存済の規律)
- **`/dev/null` への出力は grep 系の比較を破壊する** (GNU grep の
  `af6af28` 最適化、380,000× の不公平。astrogre perf.md §A 参照)
- **dlopen / ccache / sandbox の罠** (`CCACHE_DISABLE=1`、初回 bake と
  cached run の分離計測、`code_store/` 削除リセット) はサンプル間で
  共通に踏むので perf.md の前置きを最初に読むこと
- **JVM bootstrap fixed cost** (~1.5 s): TruffleSOM との比較で warm peak
  だけ取るために `ITERS=N outer × best-of-3 trials × warmup discard` の
  方法論が asom にある。長時間 bench と短時間 bench で評価が逆転するので
  両方並べる必要がある (asom: warm peak は Truffle 圧勝、wall-clock は
  asom 勝ち)

---

## 8. ASTro 自身の Pros / Cons (冷静な分析)

サンプルが揃ったところで、ASTro フレームワーク自体を評価する。
論文 (`docs/idea.md`) の主張と、サンプル実装で見えた現実を突き合わせる。

### 8.1 効いている設計 (Pros)

- **EVAL と DISPATCH の分離が、ノード追加コストを劇的に下げている**。
  どのサンプルも「言語の表現力 × 数十行 / ノード」程度で実装できている
  (calc 36 行, naruby 516 行, koruby は本格 Ruby で 1,380 行)。
  Truffle のように DSL を覚えなくても、C の関数 1 つ書けば特化が効く。
- **Merkle hash + `.so` キャッシュは強い**。複数プロセス・複数マシン間で
  特化バイナリが共有でき、起動時に dlopen 1 発で AOT 状態に入れる。
  AOT は naive な「コンパイル待ち」感がない。
- **C コンパイラを backend にする選択は移植性とデバッグ性で当たり**。
  サンプル全部 `gcc/clang -O2/-O3` 1 行で動く。LLVM/Cranelift backend を
  メンテしなくていい。`-flto` や PGO を素直に重ねられる (aforth で全 bench
  3-10% gain)。
- **`@canonical=` / `@ref` / `@always_inline` / `@noinline` が型 IC・
  動的特化に綺麗に使える**。動的言語 5 つ (abruby/asom/luastro/jstro と
  ascheme の lref キャッシュ) が同じ機構で「論理同型ノードのキャッシュ
  共有 + 動的 mutable 情報」をやれている。
- **生成コード以外を全部ユーザに開く設計** が、想定外言語にも嵌まる。
  astrogre の "regex マッチを CPS チェーン AST で表現してスキャナ層と
  融合" や、wastro の "wasm の構造化制御をそのまま 6 ノードで全網羅"
  は ASTro 設計時に想定していたとは思えないが、フレームワークが
  邪魔せず通る。

### 8.2 効きが弱い・運用上のコスト (Cons)

- **動的 dispatch を消す手段が部分評価では不足**。動的型最適化 (jstro,
  luastro, abruby) は結局ユーザが kind swap + IC + `@canonical=` で
  自力実装。フレームワーク提供の「型に基づく自動特化」は無く、
  Truffle/Graal の self-optimizing AST に比べると **オートマティズムは低い**。
  `koruby` の README が認める通り「AST 特化はノードグラフのインライン化
  までで、メソッド呼出ディスパッチ自体は完全には消せない」。
- **JIT 機構 (L0/L1/L2 デーモン) はオーバヘッドが見合う言語が限定的**。
  naruby しか実装がないのは、構成が重い (Unix socket + Ruby デーモン +
  リモート C コンパイラ) のと、AOT で十分速いケースが多いから。短い
  ワークロードで JIT を立ち上げる利益は出にくい。
- **特化バイナリの呼び出し品質がリンクで死にやすい**。`.so` 経由の
  dlsym 呼びは GOT 経由の間接呼びになるため、`-rdynamic` /
  `-Wl,-Bsymbolic` / wrapper post-pass を入れないと「特化したのに速くならない」
  になる (`docs/perf.md §0` が大原則として真っ先に挙げる)。
  これはユーザに **C リンカ知識を要求する** 障壁。
- **AOT bake パイプラインの罠**。ccache・dlopen キャッシュ・サンドボックス
  で書けないディレクトリ等で bake が落ちる。`CCACHE_DISABLE=1` が
  プロジェクト共通の memory 化されているのが象徴。
- **生成 .c が 7 本** (`node_eval.c` / `node_dispatch.c` / `node_alloc.c` /
  `node_hash.c` / `node_dump.c` / `node_specialize.c` / `node_replace.c`)、
  プラス `node_head.h`。サンプルディレクトリが膨れがち。Makefile が
  ASTroGen 呼び出しを毎回回す。
- **`EVAL body は触らない` という設計上の規律をユーザが破りやすい**。
  EVAL に手書き fast path を積むと "見せかけ早いが特化器が活かせない"
  になる。`docs/perf.md §0` が原則として明記しているが、規律はユーザ責任。
- **AST 解釈モデルなので、本来 control-flow ベースの最適化が難しい**。
  loop hoisting / strength reduction / ループ展開は C コンパイラに丸投げで、
  AST レベルでは触れない。castro の sieve が gcc -O1 に 3.7× 負けるのは
  AST 解釈のオーバヘッドが残る局面 (`castro perf.md`)。
- **冷スタートではコンパイル時間が見える**。AOT bake は all.so 1 個に
  集めれば dlopen 1 発に圧縮できるが、初回 bake 自体は秒〜数十秒
  かかる (大きいサンプルだと方分単位)。短命スクリプトには向かない。
- **PG の IC 情報を特化器に渡す経路は言語ごとにアドホック**。abruby 流
  (`hopt_index.txt`)・ascheme 流・jstro 流とそれぞれ実装している。
  共通フレームワーク化されていない。

### 8.3 適性マトリクス (どんな言語に向くか)

| 言語タイプ | ASTro との相性 | 根拠サンプル |
|---|---|---|
| 静的型 + 単純 ABI (Pascal/C/Wasm) | **◎** | castro, pascalast, wastro が gcc -O0/-O1 に肉薄 |
| 関数型 (末尾呼出最適化が必須) | **○** | ascheme/astocaml が成立。トランポリン or RESULT 経由でやる |
| 動的型 OO (Ruby/Lua/JS/Python/Smalltalk) | **△ → ○** | 型 IC ぶん殴り実装は要るが、koruby が optcarrot 完走、jstro が node v18 に勝つケース有 |
| 純動的 (Scheme) | **○** | ascheme が chibi/guile に並ぶ AOT 性能 |
| スタック VM (Forth/Wasm) | **○** | aforth が gforth を 8/9 で上回る |
| DSL (regex) | **○** | astrogre が onigmo の隣に並ぶマッチ性能 |
| イベント駆動 / async | **未検証** | サンプル無し |
| GC の精度が要る (precise GC) | **△** | jstro/luastro が自前 mark-sweep を書いており、フレームワークは助けない |
| 短命スクリプト (CLI ツール) | **△** | bake コストと dlopen キャッシュの初期化が見える。プレ bake 推奨 |

### 8.4 まとめ

ASTro は **「AST 解釈で書きやすい × 部分評価 + C コンパイラで AOT 速度」**
という二点を両立する設計で、サンプル群を見るかぎり想定通り回っている。
ただし **「Truffle 並みの動的型最適化」は得られない**。動的言語の高速化
分は kind swap + `@canonical=` + IC でユーザが組み立てる必要がある。
言い換えれば、ASTro は「**動的言語の最適化機構をユーザが書きやすい**
土台」 — 自動最適化基盤ではなく、**最適化機構の DIY キット** に近い。

実証された範囲では:
- 静的型・スタック VM → AOT で gcc -O1 級 (castro/aforth/wastro)
- 動的言語 → 既存処理系 (CRuby, lua5.4, node v18) と同水準〜部分的勝利
- 関数型 → 既存トリ系処理系 (chibi/guile) と並ぶ
- DSL → 専用エンジン (onigmo) と肉薄

** AOT に頼った "tree-walking interpreter の限界突破" としては成功**
していると言える。一方で **JIT の枠組みは試作段階** (naruby のみ) で、
ここを充実させると ASTro の射程が広がる。
