# ASTro 統一 GC 設計案

ASTro framework のサンプル横断で使える pluggable な precise GC 基盤の設計案。
**実装は未着手**、選択肢を狭めず議論継続中の段階のメモ。

関連: `idea.md` §8.2 (未踏項目), `code_store_quirks.md` (AST NODE 不動制約の出処),
`perf.md` (CTX hot member lift 議論)。

## 概観

### なぜこの設計案を書いているか

ASTro framework には現在 19 個ほどのサンプル言語実装があり、 値の lifetime
管理は **サンプルごとに完全にバラバラ** になっている:

| 現状 | サンプル | 性格 |
|---|---|---|
| GC なし | `calc`, `naruby`, `pascalast`, `castro`, `aforth`, `wastro` | int / static 型のみ |
| `libgc` 直叩き (conservative) | `koruby`, `pystro`, `asom`, `astr`, `baruby` 等 | 動的言語、 ヒープ多用 |
| 自前 mark&sweep | `luastro` | NaN-box + weak table |
| CRuby GC ホスト | `abruby`, `arjsv` | C 拡張、 host VM 委譲 |
| arena / region | `arcel` | activation 単位 reset |

「サンプルごとに 5 種の GC が乱立している」状態を、 **1 つの framework 機構**
に統合したい、 というのがこの設計案の出発点。

### 何を統一して、 何を統一しないか

| 統一する | 統一しない |
|---|---|
| Allocation / mark / safepoint / write barrier の **API 形** | 値の表現 (LSB tag / NaN-box / Flonum など) |
| **Root 列挙の mechanism** (frame descriptor) | 言語ごとの値の構造体定義 |
| AST NODE の扱い (= 絶対動かさない) | node.def の BODY |
| `node.def` declarative codegen の文化 | サンプル固有の builtin / runtime |

Algorithm (non-moving / semispace / generational / realtime) は **backend として
差し替え可能** にする。 同じ言語実装が `make GC=semispace` で moving に切り替わる、
というのを最終的な姿に置く。

### ASTro 特有の制約 2 つ

1. **AST NODE は移動不可**。 Code Store が SD\_\<hash\>.so 内に NODE \* を
   ポインタ literal として焼き込んでいるため、 moving GC backend を選んだとしても
   AST NODE だけは固定アドレスでなければ壊れる
2. **node.def の BODY テキストは触らない**。 これは `idea.md` の根本主張で、
   GC を入れる時も BODY を侵襲しない手段で WB / safepoint を仕込む必要がある

この 2 つの制約が、 「世の中の GC interface 設計を転用するだけでは足りない」
という ASTro 固有の課題になっている。

### 提案の骨子 (1 枚絵)

統一 GC を可能にするために framework が提供するのは以下の 4 つ。 全部が
**declarative** — つまり「言語側がデータとして宣言 → ASTroGen が必要な C を
吐く」形式で揃える:

1. **値の型宣言** (`value.def` 採否は §26 で議論、 §2 に構文案) — kind 別 marker /
   setter / allocator を生成
2. **Root 列挙** (frame descriptor、 §5) — node.def の `@roots` 注釈から
   per-SD `astro_frame_desc_t` を生成
3. **Write barrier 経路** (§6) — `@ref` setter / `WB` macro 経由で
   backend に応じた barrier を挿入
4. **Safepoint 配置** (§7) — `@allocates` / loop back-edge ノードに
   ASTroGen が自動で poll を入れる

backend (algorithm) は **compile-time** で 1 個選ぶ。 `GC=none` を選ぶと全部の
hook が `(void)0` に潰れて現行コードと等価になる、 を **ゼロコスト性の gate**
にする。

### 読み方

| 知りたいこと | 読む順序 |
|---|---|
| 何を解決したいか / 全体像 | §0 設計目標 → §11 サンプル俯瞰 → §17 最初の一手 |
| 抽象化の中身 (mechanism) | §1 4 層 → §2-7 各 API → §8 backend matrix |
| 各サンプルへの当て込み | §11-14 (Tier 別) |
| node.def に何を書くか (interface) | §18-28 (後半、 実装規模含む) |
| 未決事項 (要議論) | §16 + §26 (`value.def` 採否) |

## 0. 設計目標

- 1 個の interface から **non-moving / moving (semispace) / generational / realtime**
  の 4 アルゴリズムを差し替え可能にする
- `node.def` declarative codegen の文化を継承し、言語側の侵襲を最小化する
  (`value.def` で型を宣言 → ASTroGen が marker / setter / allocator / frame
  descriptor を生成)
- AST NODE は Code Store (`SD_<hash>.so`) がポインタを焼き込むため絶対に動かない、
  という ASTro 特有の制約を全 backend で一様に扱う
- CRuby host (`abruby` / `arjsv`) も同じ DSL から marker を生成できる、を抽象化の
  正しさの証拠にする
- wasm ターゲット (koruby を wasm に出したい) と整合する設計にする
  (libgc conservative scan が wasm スタックに届かない問題があるため precise が必須)

## 1. 抽象化の 4 層

「allocate / mark / move / barrier」 という GC primitive 群を言語非依存に
切り出して 4 種類のアルゴリズムを backend として差し替えられるようにする、
というのが核。 ただし

- **値の表現** (LSB tag / NaN-box / Flonum boxing) は **言語の魂** — backend
  では決められないので言語側に残す
- **Root 列挙の中身** (どの局所変数が live か) も言語に固有 — ただし
  *列挙の mechanism* (frame descriptor を辿る) は framework が提供する

この切り分けに沿って 4 層に分けると、 上から下への依存関係が一方向になる:

```
Layer 4  Algorithm:   non-moving / semi-space / generational / realtime
              ↑ implements
Layer 3  GC core API: alloc / safepoint / wb / rb / heap / root_visit
              ↑ consumed via macros (BODY からは API 経由のみ)
Layer 2  Generated:   ASTroGen が value.def + node.def から
                      marker / setter / frame descriptor を生成
              ↑
Layer 1  Language:    value.def (型) + frame iterator (root 列挙)
```

- **Layer 1** = 言語が宣言する: 値の型 (value.def を採用するなら) + frame
  iterator の 1 関数だけ
- **Layer 2** = ASTroGen が自動生成する: kind 別 marker / setter / per-SD
  frame descriptor。 言語実装者は触らない
- **Layer 3** = backend 非依存の C API。 `astro_gc_alloc` / `ASTRO_WB_PTR`
  / `ASTRO_SAFEPOINT` 等の macro 群
- **Layer 4** = 具体的アルゴリズム実装。 `make GC=<name>` で compile-time に
  1 つ選ばれる

「BODY のテキストを触らない」原則は、 この層構造の上では **「BODY からは
Layer 3 の macro 経由でしか GC に触らない」** に再定義される。 これにより
backend 切替 (Layer 4 差替) は BODY を不変のまま行える。

## 2. `value.def`: 型宣言 DSL

```
VALUE_DEF korb_obj                     # 共通ヘッダ + tagged union
  header korb_obj_header               # kind, gc bits, hash 等
  field  obj_kind_t kind
  union(kind) {
    KIND_STR    => { atomic_payload(char) chars; size_t len; }
    KIND_ARY    => { ref_payload(VALUE) items; size_t len; }
    KIND_HASH   => { ref(struct kh_table *) tbl; }
    KIND_PROC   => { ref(NODE *, immortal) body;
                     ref(VALUE) self; ref(VALUE) cref;
                     int32_t arity; }
    KIND_BIGNUM => { atomic_payload(mp_limb_t) limbs;
                     finalizer korb_bn_fin; }
  }
END
```

注釈の意味:

| 注釈 | 意味 |
|---|---|
| `ref(T)` | trace 対象、setter に write barrier を自動挿入 |
| `ref_payload(T)` | 二層オブジェクトの可変長 ref 配列。別ヒープ、一括 move 可 |
| `atomic_payload(T)` | ref を含まない bytes。deep mark 不要、moving 可 |
| `immortal` | 不動 (AST NODE / SD_<hash>.so 内シンボル等) |
| `finalizer F` | 解放時 F 呼出し。realtime では別キュー処理 |
| `ref_weak(T)` | 弱参照、mark queue に積まない (luastro weak table 用) |

ASTroGen が value.def から生成するもの:

- `KORB_ALLOC_<KIND>(...)` — kind 別 type-specialized allocator
- `KORB_MARK_<KIND>(obj)` — precise marker
- `KORB_FORWARD_<KIND>(obj)` — moving 用 forward fixup
- `KORB_SET_<field>(obj, val)` — write barrier 込み setter

`@ref` 経由のノード本体側は既に setter 化されているので接続点は揃う。

## 3. ヒープ分離規約 (backend 非依存)

```c
typedef enum {
  HEAP_NONE,             // GC 機構を使わないサンプル用 (calc, naruby 等)
  HEAP_VALUE,            // 通常オブジェクト。backend が moving なら動く
  HEAP_REF_PAYLOAD,      // 二層 ref 配列
  HEAP_ATOMIC_PAYLOAD,   // bytes、deep mark なし
  HEAP_IMMORTAL,         // 不動・永続。AST NODE はここ
  HEAP_LARGE,            // 単独 mmap (move しても利得薄い)
  HEAP_FINALIZABLE,      // finalizer 持ち
} astro_heap_kind_t;

astro_heap_t astro_gc_heap(astro_heap_kind_t k);
astro_heap_t astro_gc_heap_create(astro_heap_kind_t k,
                                  const astro_heap_config_t *);
```

**`HEAP_IMMORTAL` を framework 規約として固定する**ことが鍵。Code Store が
`SD_<hash>.so` 内に NODE * を焼き込む問題は、AST NODE をこのヒープへ強制配置
することで全 backend で一様に消える (これは `project_astro_value_consts_gap`
で議論される VALUE 焼込み問題とは別件なので注意)。

## 4. アロケーション API

```c
// kind は value.def の type id、size は payload 込み。
// LSB tagging は言語の魂なので戻り値はタグなし。
void *astro_gc_alloc(astro_heap_t h, uint32_t kind, size_t size);

// 二層オブジェクトの payload。attr で atomic / movable を指定
void *astro_gc_alloc_payload(astro_heap_t h, size_t size,
                             astro_payload_attr_t a);
```

サンプル側は `KORB_ALLOC_PROC(self, body, ...)` のような生成 wrapper 経由で呼ぶ
ので、`astro_gc_alloc` を直書きするのは ASTroGen 出力だけ。

## 5. ルート列挙: frame iterator (precise の核)

`project_unified_gc_design` で合意済みの「CTX hot member を register lift →
safepoint で flush」と **同一の mechanism** を使う:

```c
struct astro_frame_desc_t {              // 各 dispatcher で 1 回 static const
  uint16_t  size;
  uint16_t  n_refs;
  uint16_t  ref_offsets[/* n_refs */];   // F[] 内オフセット
  struct {                               // sp[0..sp_off-1] のような可変長 root
    uint16_t base_off;
    uint16_t count_off;
  } ref_array;                           // .count_off==0 なら無し
};

#define ASTRO_FRAME_ENTER(c, desc, frame_ptr)  /* on-stack chain push */
#define ASTRO_FRAME_LEAVE(c)                   /* pop */

// 言語が 1 回だけ実装
void <lang>_gc_iter_roots(astro_root_visitor_t *v);
```

ASTroGen が dispatcher prologue/epilogue を生成するので、各 `SD_<hash>` には
自分用の `static const astro_frame_desc_t SD_<hash>_FD` がリテラルとして焼き込
まれる (frame layout もハッシュ入力に含めれば SD レベルで一意)。

GC 起動時の root 列挙:

```
<lang>_gc_iter_roots() →
  c->fp_chain を辿る → 各 frame の desc.ref_offsets / ref_array を visit
```

global root (function table、symbol table 等) は言語が visitor に直接渡す。

## 6. Write barrier

```c
// holder = 書き込まれる側のヘッダ、val = 新値
#define ASTRO_WB_PTR(holder, field, val) do {                        \
    astro_gc_pre_wb((holder), (void **)&(holder)->field);            \
    (holder)->field = (val);                                         \
    astro_gc_post_wb((holder), (void *)(val));                       \
} while (0)
```

ASTroGen 生成 setter (`KORB_SET_<field>`) と `@ref` 書換が必ずこれ経由。

| Backend | `pre_wb` | `post_wb` |
|---|---|---|
| non-moving M&S | no-op | no-op |
| semi-space | no-op | no-op (STW) |
| generational | no-op | card mark / remset |
| realtime (SATB) | 旧値を mark queue | no-op |
| realtime (incremental update) | no-op | 新値が white なら shade |

`pre_wb` / `post_wb` は inline → no-op backend では C コンパイラが完全に消す。

## 7. Safepoint と read barrier

```c
#define ASTRO_SAFEPOINT(c) do {           \
    if (UNLIKELY(astro_gc_pending)) {     \
        astro_gc_flush_frame(c);          \
        astro_gc_handshake();             \
        astro_gc_reload_frame(c);         \
    }                                     \
} while (0)
```

挿入箇所 (ASTroGen 自動):

- allocation site の直前
- loop back-edge (while/for)
- call 境界の入口 (再帰許容のため)
- 例外ハンドラ境界

preemptive ではなく **cooperative**。signal-based は wasm との両立が悪く、
koruby の setjmp/longjmp 不使用方針とも合う。

Read barrier は **デフォルト identity**、Brooks-style realtime のときだけ
effective:

```c
#define ASTRO_LOAD_REF(p) astro_gc_rb((p))    // 非 RT では p
```

`EVAL_ARG` 系のフィールド読出しは ASTroGen 側でこの macro を経由するように生成。

## 8. backend ↔ API マッピング (差替え可能性の根拠)

| | non-moving | semi-space | generational | realtime |
|---|---|---|---|---|
| `gc_alloc` | freelist bump | to-space bump | young bump | concurrent bump |
| `safepoint` | poll & STW | poll → STW copy | poll → minor/major | poll abort & yield |
| `pre_wb` | no-op | no-op | no-op | SATB enqueue |
| `post_wb` | no-op | no-op | card / remset | shade-on-store |
| `rb` | id | id | id | Brooks forward |
| frame iter | precise | precise | precise+remset | precise concurrent |
| AST NODE heap | shared OK | **pinned 必須** | **pinned 必須** | **pinned 必須** |
| atomic payload | header mark | move payload | move payload | concurrent move |
| AOT 生成 SD | 影響なし | frame desc 必須 | 同左 | 同左 + RB 入り |

`HEAP_IMMORTAL` を共通規約に持ち上げたことで「moving に切り替えると Code Store
が壊れる」事故が起きない。

## 9. 切替粒度

- **compile-time** (`make GC=semispace` 等) を default。`astro_gc.h` 内の `inline`
  で全 hook を確定し、no-op はコンパイラが消す。性能上ここがゼロコスト化の鍵
- runtime dispatch は持たない。`abruby` (CRuby GC) は別 source 群で hooks を
  書く専用 backend 扱い
- wasm ターゲットだけ `make GC=wasm-gc` で WasmGC primitive (`anyref`) を直接
  呼ぶ亜種を予約 (将来枠)

## 10. 移行ステージ (大爆発を避けるため)

1. **S0**: hook macro 化のみ。non-moving libgc backend 維持、`pre_wb` /
   `post_wb` / `safepoint` は no-op。BODY 書換は setter 経由化に限定
2. **S1**: `value.def` 起動、precise mark&sweep backend を libgc の代替として
   実装。AST NODE は `HEAP_IMMORTAL` へ
3. **S2**: semi-space copying に切替。frame descriptor + safepoint flush が
   ここで効力発揮
4. **S3**: generational / SATB realtime はそれぞれ backend 差し替えのみで
   BODY 不変

S0 の時点で「macro を全部通す」状態になっていれば、後段はアルゴリズムの選択
問題に純粋化される。

---

# サンプル別の当て込み案

## 11. 俯瞰: 値特性 × 推奨 backend

各サンプルを「値表現の癖」「現状 GC」「変更不可制約」「アロケーション圧」で分類:

| sample | 値の性格 | 現状 | 制約 | 推奨 default | 補助ヒープ |
|---|---|---|---|---|---|
| calc | int リテラル | 無し | — | **`none`** | — |
| naruby | int64 only | 無し | — | **`none`** | — |
| pascalast | 静的型 + new/dispose | 無し | 明示解放を尊重 | **`none`** | `HEAP_LARGE` (array) |
| castro | C subset | malloc/free | 明示解放 | **`none`** | — |
| aforth | スタックマシン | 無し | — | **`none`** | — |
| wastro | linear memory | host 管理 | wasm linear mem | **`none`** | host 委譲 |
| asml | int + variant + closure | 無し | int1bit tag 固定 | **`semispace`** | — |
| astocaml | int + variant + ref | 無し | int1bit tag 固定 | **`semispace`** | — |
| ascheme | pair / symbol / vec | (libgc?) | — | **`semispace`** | — |
| luastro | NaN-box flonum + GCHead linked | 自前 M&S (`lua_gc.c`) | **値表現変更禁止** | **`marksweep` (precise)** | weak ref subheap |
| asom | block/frame heap-alloc | libgc | frame escape 多 | **`generational`** | escape 軽量解析 |
| koruby | full Ruby 風 (ivar / hash / class / cref / proc) | libgc | optcarrot 動作維持 | **`generational`** | `HEAP_FINALIZABLE` (mpz), `HEAP_IMMORTAL` (class/method) |
| pystro | full Python 風 (dict / class / try) | libgc + GMP | mini-gmp 採用方針 | **`generational`** | `HEAP_FINALIZABLE` (mpz) |
| jstro | JS Object + IC | (libgc?) | prototype chain 不変 | **`generational`** | shape table を `HEAP_IMMORTAL` |
| astr | 数値ベクトル中心 | libgc | vectorized | **`marksweep` + LARGE** | `HEAP_LARGE` (mmap) |
| nuq | filter pipeline 中間値 | (linearity 解析あり) | 線形性ヒント活用 | **`generational`** または **`region`** | パイプ単位 reset 候補 |
| arcel | activation arena 既存 | arena | latency 敏感 (K8s) | **`region` (arena 化)** または **`realtime`** | 既存 arena を backend 化 |
| astrogre | DFA state 短命 + pattern 長命 | (?) | regex は速度優先 | **`generational`** | DFA テーブルを `HEAP_IMMORTAL` |
| arjsv | CRuby C ext | CRuby GC | host VM 侵襲不可 | **`cruby`** | host 委譲 |
| abruby | CRuby C ext | CRuby GC + `node_mark.c` | host VM 侵襲不可 | **`cruby`** | host 委譲 |

**全サンプル共通の規約 2 点**:

- AST NODE は `HEAP_IMMORTAL` 強制 (Code Store の `SD_<hash>.so` がポインタを
  焼き込むため、moving backend でも動かしてはならない)
- LSB tag は言語の魂として framework 不介入 (asml の bit0=int / luastro の
  NaN-box / koruby の Flonum / naruby の生 int64 — それぞれ別)

## 12. Tier 別の当て込み

### 12.1 Tier A — GC を作らないサンプル

対象: `calc`, `naruby`, `pascalast`, `castro`, `aforth`, `wastro`

これらは backend ごと丸ごと no-op (`GC=none`)。提案の **ゼロコスト性検証** として
むしろ重要:

- value.def は省略可 (NodeKind だけで完結)
- `astro_gc_alloc` → 素の `malloc` (or arena bump)
- `ASTRO_WB_PTR` / `ASTRO_SAFEPOINT` → `(void)0`
- frame iterator も生成不要

**naruby を first prototype のターゲットに**。ベンチ (`loop` / `fib` / `call` /
`prime_count`) が確立しているので、API 導入後の regression を gate にできる。

### 12.2 Tier B — 関数型・ML 系

対象: `asml`, `astocaml`, `ascheme`

cons / closure / variant 中心。ref 密度が高くアロケーション短命。
**moving (semispace) の教科書ケース**。

```
union(kind) {
  CONS    => { ref(VALUE) hd; ref(VALUE) tl; }
  CLOSURE => { ref(NODE *,immortal) body; ref_payload(VALUE) env; }
  VARIANT => { symbol_id ctor; ref_payload(VALUE) args; }
  ...
}
```

closure env が `ref_payload` で別ヒープ → 関数型コードの hot path (env 走査) の
キャッシュ局所性が compaction で立つ。

`asml` は HM 型推論完備 → 型情報から `atomic` payload (例: int 配列) を ASTroGen
側で自動推定する高度化が可能。`@type` 注釈で moving GC backend に payload の
atomic ヒントを流せると理想。

→ **`asml` を moving backend の本命試験場に**。HM 型 × precise + safepoint の
組み合わせは ASTro 独自の見せ場になる。

### 12.3 Tier C — 動的言語ヘビー級

対象: `koruby`, `pystro`, `jstro`

ivar / dict / prototype chain など「ほぼ不変 old gen + 短命 young gen」が顕著。
**generational の本命**。

- koruby の `korb_object.ivars`、`korb_array.ptr`、`korb_hash` の bucket チェーン
  は現状 `xmalloc` ベース → value.def 化すると `ref_payload` で別ヒープに分離可能。
  Hash の per-entry malloc は **freelist + ref_payload 配列** に置換すれば moving
  化のメリットが効く
- bignum (mpz_t) は `HEAP_FINALIZABLE` に切り出し: `mpz_clear` を finalizer 登録、
  現在の `GC_register_finalizer` 呼び出しが naturally 統一規約に乗る
- class / method_table / cref は基本 immortal (プログラム生存期間と一致) →
  `HEAP_IMMORTAL` に置けば marker は touch するだけで promotion 不要

koruby は最も改造範囲が広いので **Phase 4 後半**。最大の利得が出るが事故りやすい。
optcarrot ベンチが gate。

pystro はほぼ同戦略。`pystro/runtime.c` の 14k 行を放っておくため value.def の
DSL 表現力が一番試される (try/except のスタック展開時 root 列挙、`__reduce__` 系
protocol オブジェクト等)。

jstro の prototype shape は immortal 化することで IC 無効化が減らせる副次効果。

### 12.4 Tier D — 自前/特殊 GC 既存

#### luastro

`NaN-boxing 禁止` 約束があるので **値表現は不変**。既存 `GCHead.next` linked list
と weak tables の意味論を壊さない非破壊的 mapping が必須。

- `union(kind)` で `LUA_TSTRING / LUA_TTABLE / LUA_TFUNC / LUA_TBOXED` 等を表現
- `LuaBox` (captured local) は特別 ref kind: 値が常に 1 つ、closure からのみ参照
  → 共通 `ref_payload(VALUE,len=1)` で書ける
- weak table は `ref_weak` annotation: backend に「mark queue に含めない」を指示
- **default は precise non-moving**。既存自前 M&S を value.def 経由の生成版に置換
  するだけ。NaN-box 値は変えない、moving しない、で BODY 不侵襲

#### asom

Smalltalk の hallmark = block.frame がヒープに乗る。frame 自体が GC 対象。

- `struct asom_frame` を value.def 化すると、frame が `HEAP_VALUE` の一級住民に
- block escape 解析が将来入れば → moving + scalar replacement で frame 上の局所値
  をレジスタ化できる (これは「frame は言語の魂」原則とぶつかるので慎重)
- 当面は **generational** で十分

#### astr

vector 中心 = ref 少 + atomic payload が大半。

- moving の利得が薄いので default は `marksweep`
- ただし **`HEAP_LARGE`** 経路 (個別 `mmap` + 専用 free list) が最大利得。R 実装
  のキャッシュ性能はここが効く

### 12.5 Tier E — DSL 系

短命中間値が多く、**arena/region GC の試験場**として最適。

#### arcel

一番美味しい。

- 既に **arena (`arcel_arena_handle`) を持っている** → そのまま `GC=region`
  backend の reference 実装に格上げ
- activation 単位 reset = realtime な制約 (K8s admission に 52 ns/op) と直交。
  ここに `realtime` backend (concurrent SATB) を当てると「**100% conformance +
  10× perf + bounded latency**」という三冠の主張になる

#### nuq

`linearity.c` で線形性解析がある。

- 線形値は **arena bump で配置 → activation 末尾で全部破棄** が最適。GC 自体走らない
  ケースを増やせる
- 非線形は通常ヒープ (generational)
- value.def に `@linear` annotation を入れる案 (未決事項に追加)

#### astrogre

pattern 木は immortal、match state は短命。`HEAP_IMMORTAL` + young 領域でほぼ
全戦略が綺麗に決まる。

### 12.6 Tier F — host VM

対象: `arjsv`, `abruby`

interface 設計の **真の汎用性検証**。CRuby GC を backend として実装し、

- `astro_gc_alloc` → `rb_data_typed_object_alloc`
- `pre_wb` → `RB_OBJ_WRITE` の旧値 protect
- `post_wb` → CRuby write barrier
- frame iterator → `rb_gc_mark` callback として実装
- value.def → 既に `node_mark.c` を生成している ASTroGen に extension

abruby は既に `node_mark.c` を生成している = **ASTroGen が GC marker を生成する
文化はこのサンプルで実証済み**。これを framework 標準に格上げするだけ、という
見せ方ができる。同じ value.def から `cruby` backend と `precise` backend の
**両方の marker** が生成できることを示せれば、抽象化の正しさの証拠になる。

## 13. value.def の表現力チェックリスト

11 言語 × 関数型 / 動的型 / 静的型 / DSL を 1 つの DSL でカバーするには:

| 必要表現 | 由来サンプル | 提案構文 |
|---|---|---|
| 共通ヘッダ + tagged union | koruby, asom, asml | `union(kind) { ... }` |
| 二層オブジェクト (header / payload) | koruby array, ascheme str | `ref_payload(T) / atomic_payload(T)` |
| 不動オブジェクト | AST NODE 全般, jstro shape | `immortal` |
| 弱参照 | luastro weak table | `ref_weak(T)` |
| Finalizer | bignum, file handle | `finalizer F` |
| 大型直 mmap | astr vector | `large` payload attr |
| 線形値 (arena 配置) | nuq | `@linear` (未決) |
| プリミティブ tag (LSB / NaN-box) | naruby/asml/luastro | **DSL 不介入** (言語側マクロ) |
| weak hash バリエーション (k/v/kv) | luastro | `weak_mode(k|v|kv)` 注釈 |
| 共有ヒープ (intern table 等) | symbol table | `intern_pool` ヒープ kind |

これだけ揃えば全サンプルを書き直せる。逆にここで不足が見えたら DSL 仕様の段階で
気付ける。

## 14. 段階的展開ロードマップ (sample 視点)

| Phase | サンプル | 検証項目 | gate |
|---|---|---|---|
| 0 | naruby, calc | API 導入のゼロコスト性 (`GC=none` で regression なし) | naruby ベンチ ±1% |
| 1 | luastro | 既存自前 M&S を value.def 経由 precise marker に置換 (値表現不変) | 既存テストグリーン |
| 2 | asml | semispace に振り切る、frame iterator + safepoint の本気検証 | bench (sustained) |
| 3 | astrogre, asom | generational backend を 2 サンプルで横展開、汎用性確認 | regex selftest, SOM 完走 |
| 4 | koruby | optcarrot を gate にしながら generational 化 | optcarrot 完走 ± 性能 |
| 5 | pystro, jstro | koruby と同 backend で済むことを確認 (汎用性 proof) | 既存 PASS 数維持 |
| 6 | arcel | region / realtime backend の reference 実装、CEL bench で latency 主張 | 808-808 conformance + p99 |
| 7 | abruby, arjsv | CRuby backend の実装、value.def → CRuby `dmark` への変換確認 | rb_check |
| 8 | astr | LARGE_HEAP 経路で vector 系 R ベンチ | 既存テスト |

## 15. 研究的に面白い副産物

- **asml + moving**: HM 型情報を ASTroGen に渡し、`atomic` 性を型推論で auto-導出。
  型システムと GC の協調を ASTro 独自の見せ場に
- **arcel + realtime**: 既に「cel-cpp 比 22.4×」 の性能優位 に **bounded GC pause**
  を加えると K8s admission 用途で完全勝利の触れ込みになる
- **luastro 値表現不変 + precise**: 「自前 M&S を NaN-box を保ったまま precise 化」
  は技術的にきれいで、論文の「BODY 不侵襲」主張の物理的証拠になる
- **abruby が cruby backend に乗れる**: 「同じ value.def から CRuby GC marker と
  自前 GC marker の両方が生成できる」= 抽象化が漏れていない proof
- **naruby に GC 一切無し** で API のゼロコスト性が示せる = JIT 性能評価が GC
  ノイズを完全に排除して測れる (PPL2026 の延長として綺麗)

## 16. 未決事項

- (a) `value.def` を `node.def` と同じファイルに同居させるか分離か。declarative
  一元化 vs. 責務分離。**同居推し** (`node.def` の DSL を拡張)
- (b) `astro_gc_alloc` 戻り値の tag。**タグなし**で言語側で被せる案 (naruby の
  int only / koruby の Flonum / pystro の bignum で tag scheme が違うため)
- (c) JIT で生成される SD の frame descriptor の運搬。Layer 2 で `static const`
  を SD_<hash>.c に焼く案
- (d) `HEAP_FINALIZABLE` の semantics: BDW 風 weak / strong / topo-sort、どれを
  default にするか
- (e) `ref_array` の長さ表現を `count_var` (unsigned) で済ますか、`begin/end`
  ペアを許すか
- (f) `@linear` 注釈の DSL 化 (nuq の linearity 解析と統合するか)

## 17. 推奨される最初の一手

**arcel と asml を厳選して掘る** のが費用対効果が高い:

- arcel = arena 既存・realtime 候補 → backend 多様性の証拠
- asml = HM 型 × moving → 関数型の本流ケース

両方とも既存サンプルが小〜中規模で完結しており、koruby / pystro のような巨大
サンプルを巻き込まずに「4 backend が同居する設計」のミニ証明ができる。

---

# node.def 統合インターフェース案

GC を ASTroGen の生成パイプラインに組み込むときに **`node.def` に何を書き、
何を ASTroGen に生成させるか** の interface 案。`value.def` の採否自体は
**未決のまま残す** (§28 で trade-off を整理)。

## 18. 全体方針

§2 の `value.def` を導入するか否かに関わらず、**先に固められる範囲**を
切り出して書く:

- (A) **NODE_DEF オペランド GC 注釈** (§20) — operand に `@ref(value)` /
  `@imm` / `@weak` 等を足し、 abruby の `node_mark.c` 相当を framework 標準化
- (B) **NODE_DEF レベル GC オプション** (§21) — `@allocates` / `@safepoint`
  / `@roots(...)` で frame descriptor + safepoint 配置を declarative 化
- (C) **BODY 内 GC macro** (§22) — `WB` / `KORB_NEW_*` / `PIN` / `SAFEPOINT`
  / `LD`。 backend 切替で `GC=none` のとき identity / `(void)0` に潰せること

(A)+(B)+(C) は **値表現の宣言 DSL なしで成立する**。 frame iterator の
mechanism + write barrier 配置の declarative 化までを node.def 側で完結
させる、というスコープに絞れる。

値表現自体 (struct kind union とその marker) を別途 DSL 化するのが §2 の
`value.def` だが、

- 言語ごとに値表現が極端に違う (LSB tag / NaN-box / pointer-only / etc.)
- abruby/arjsv は CRuby の `VALUE` を変えられない、 luastro は NaN-box を
  変えられない (`feedback_no_nan_boxing`)
- ASTroGen が「root の場所」だけ知っていれば marker は言語側手書きで足りる
  可能性が十分ある

ので、 `value.def` を入れるか入れないかは独立に検討する。決め打ち前提で
node.def 統合を設計しない、というのが本案の立て付け。

重要な原則:

- **後方互換**: GC 関連オプションを足さない既存 NODE_DEF + `make GC=none`
  のとき、生成される C コードは現状とビット同一 (calc / naruby / castro
  regression なし = ゼロコスト性の gate)
- **BODY 非侵襲**: BODY 内で必要な GC 介入は **すべて macro 経由**
- **opt-in**: 既存サンプルは GC オプションを足さない限り従来の動作。
  GC を本気で入れるサンプルだけが新しい注釈を書く

## 19. VALUE_DEF (採用する場合の構文 — §26 で採否を議論)

§2 の例を `node.def` の syntax で素直に書けるよう、`NODE_DEF` と同じ
header + body 形式に揃える:

```
VALUE_DEF korb_obj @header=korb_obj_header @kind_field=kind
{
    KIND_STR    => atomic_payload(char) chars; size_t len;
    KIND_ARY    => ref_payload(VALUE) items; size_t len;
    KIND_HASH   => ref(struct kh_table *) tbl;
    KIND_PROC   => ref(NODE *) @immortal body;
                   ref(VALUE) self;
                   ref(VALUE) cref;
                   int32_t arity;
    KIND_BIGNUM => atomic_payload(mp_limb_t) limbs;
                   @finalizer korb_bn_fin;
}
```

`VALUE_DEF <type-name> [@option ...]` を header、波括弧内を kind 別フィールド
リストとして parse する。`NODE_DEF` の operand parser を ほぼ流用できる
(`@ref` 注釈の認識を `@immortal` / `@finalizer` / `weak` 等に拡張するだけ)。

ASTroGen が出すのは §2 と同じ:

| 生成シンボル | 役割 |
|---|---|
| `KORB_ALLOC_<KIND>(...)` | kind 別 type-specialized allocator |
| `KORB_MARK_<KIND>(obj)` | precise marker (`ref` / `ref_payload` を walk) |
| `KORB_FORWARD_<KIND>(obj)` | moving 用 forward fixup |
| `KORB_SET_<field>(obj, val)` | write barrier 込み setter |
| `<type>_kind_t kind_table[]` | kind → marker/forward function テーブル |

`abruby` が既に `register_gen_task :mark` で marker を出しているのが先例。
これを framework 標準として `:gc` task に格上げする形になる。

**ただし** これは「採用するならこういう形」というスケッチであって、
本当に DSL 化するメリットが言語ごとの値表現の癖を吸収するコストを
上回るかは §28 で改めて議論する。

## 20. NODE_DEF オペランド GC 注釈の拡張

既存の `<type> <name>@ref` (struct 内 inline 化、hash skip) に GC 関連の
意味を持つ注釈を足す:

| 注釈 | 意味 | 使用例 |
|---|---|---|
| `@ref` | 既存。 inline 格納 + hash skip。**GC は touch しない** (mutable な metadata) | `struct ic *cache@ref` |
| `@ref(value)` | `@ref` かつ中身が `VALUE` (or VALUE 配列)。 marker は mark する | `VALUE last_recv@ref(value)` |
| `@imm` | この operand は `HEAP_IMMORTAL` 上の永続オブジェクトを指す。marker は touch せず recurse のみ。 default は AST NODE * | `NODE *body@imm` (実質 default) |
| `@weak` | 弱参照。 backend に「mark queue に積まない」を指示 | `VALUE key@weak` |
| `@atomic` | bytes 列、deep mark 不要。 `const char *` などのプリミティブのデフォルト動作を明示化 | `const char *name@atomic` |

注釈の有無で `node_gc.c` (新規 task) の per-node marker 生成内容が決まる:

```c
// 例: node_call(CTX *c, NODE *n, NODE *recv, NODE *args,
//                struct ic *cache@ref, VALUE last_recv@ref(value))
static void
GC_MARK_node_call(NODE *n)
{
    /* recv, args: @imm な NODE * → 自動再帰 (AST NODE は immortal なので
       実体 touch ではなく to-process queue に push のみ) */
    astro_gc_mark_node(n->u.node_call.recv);
    astro_gc_mark_node(n->u.node_call.args);
    /* cache: @ref のみ → skip */
    /* last_recv: @ref(value) → mark */
    astro_gc_mark_value(n->u.node_call.last_recv);
}
```

`abruby/node_mark.c` のロジックを「`@ref(value)` でない `@ref` は skip、
`@ref(value)` は mark」に正規化したものに相当する。

## 21. NODE_DEF レベルオプション (GC 系)

`NODE_DEF @noinline` と同じ位置にぶら下げる GC 関連オプション:

| オプション | 意味 |
|---|---|
| `@allocates` | この node の BODY は値を allocate する可能性がある。ASTroGen は BODY 直前に `ASTRO_SAFEPOINT(c)` を挿入する権利を持つ |
| `@noalloc` | BODY は allocate しない (静的アサーション)。 backend は frame flush を省略可能。leaf node (literal / 変数参照系) で使う |
| `@safepoint` | 明示的な safepoint 配置 (loop back-edge ノード、call ノード等)。 `@allocates` の自動挿入とは独立に強制する |
| `@roots(name1, name2, ...)` | BODY 内のローカル `VALUE name` を frame descriptor の ref_offsets に追加。 ASTroGen は BODY を frame ENTER/LEAVE で包む |
| `@root_array(base, count)` | ローカル `VALUE *base` と `size_t count` を可変長 root 列として扱う (frame_desc.ref_array) |

例:

```c
NODE_DEF @allocates @roots(r)
node_call1(CTX *c, NODE *n, NODE *recv, NODE *arg, struct ic *ic@ref)
{
    VALUE r = EVAL_ARG(c, recv);    // r が arg 評価をまたいで生存
    VALUE a = EVAL_ARG(c, arg);     // arg 評価中に r が GC のルートになる必要
    return korb_send1(c, r, a, ic);
}
```

ASTroGen が生成する EVAL\_node\_call1 ラッパは概念的にはこうなる
(`@roots(r)` 由来で frame slot を 1 つ確保、ENTER/LEAVE で挟む):

```c
static inline __attribute__((always_inline)) VALUE
EVAL_node_call1(CTX *c, NODE *n, NODE *recv, ndf_t recv_d,
                NODE *arg, ndf_t arg_d, struct ic *ic)
{
    struct { VALUE r; } _f;
    ASTRO_FRAME_ENTER(c, &SD_node_call1_FD, &_f);
    _f.r = EVAL_ARG(c, recv);
    VALUE a = EVAL_ARG(c, arg);
    VALUE _ret = korb_send1(c, _f.r, a, ic);
    ASTRO_FRAME_LEAVE(c);
    return _ret;
}
```

BODY 中の `r` は実体としては `_f.r` だが、ここは macro `#define r _f.r` を
ASTroGen が emit することで BODY のテキストを変えずに済ませる。
**BODY-text untouched 原則の物理的実現**。

frame_desc は `SD_<hash>` 単位で `static const`:

```c
static const astro_frame_desc_t SD_node_call1_FD = {
    .size = sizeof(struct { VALUE r; }),
    .n_refs = 1,
    .ref_offsets = { offsetof(struct { VALUE r; }, r) },
};
```

`@roots` で名前を宣言した変数の型は `VALUE` 固定 (ASTroGen が BODY 内
`VALUE\s+<name>` の宣言を発見できなければ error)。一旦これで十分;
複雑な型は後段で考える。

## 22. BODY 内 API (macro 経由)

`@roots` で済まないケース用に、BODY 内で明示的に書く macro を 5 つ定義する。
backend 切替で `GC=none` のとき全部 no-op に潰せることが条件:

| macro | 役割 | none backend での展開 |
|---|---|---|
| `KORB_NEW_<KIND>(...)` | value.def 由来の type-specialized allocator wrapper | `malloc` ベース |
| `WB(holder, field, val)` | ref 書き込みの write barrier 経由化 | `(holder)->field = (val)` |
| `LD(p)` | read barrier (realtime backend で Brooks forwarding) | `(p)` |
| `SAFEPOINT(c)` | 任意 safepoint poll (`@safepoint` で済まないとき) | `(void)0` |
| `PIN(c, expr)` | 短命に式 1 個を root 化したい時用 (`@roots` の式版) | `(expr)` |

これらは `<lang>_gc.h` で backend 切替して定義する。生成コード側からは
backend 不可知。

BODY での使用例 (koruby `node_iv_set` 相当):

```c
NODE_DEF
node_iv_set(CTX *c, NODE *n, NODE *recv, NODE *val, const char *name@atomic)
{
    VALUE r = EVAL_ARG(c, recv);   // r は val 評価をまたぐが PIN で済む
    WB(r, ivars[id_of(name)], EVAL_ARG(c, val));
    return r;
}
```

`r` を `@roots(r)` するか、書き換えを `WB` macro 経由にするかは設計判断
だが、両方を許して embedder に選ばせる。

## 23. frame descriptor 生成のフロー

ASTroGen が `node_gc.c` (新 task) に集約する artifact:

1. **per-NODE_DEF marker**: `GC_MARK_<name>` — operand 注釈から生成
2. **per-NODE_DEF frame descriptor**: `SD_<name>_FD` — `@roots` / `@root_array`
   から生成 (= specialize 時の `SD_<hash>` ごとに per-hash 化される版が別途必要)
3. **kind table**: `value.def` の各 kind の marker/forward テーブル
4. **frame iter root**: 言語が 1 つ実装する `<lang>_gc_iter_roots()` の C 雛形

NodeKind に `gc_marker` / `frame_desc` を生やすために
`register_gen_task :gc, kind_field: "...; const astro_frame_desc_t *frame_desc;"`
を使う。これは既存 `register_gen_task` インタフェースに収まるので **コア DSL
の枠を破らない**。

## 24. ASTroGen 実装に必要な変更 (規模感)

`lib/astrogen.rb` 側で具体的に追加するもの:

| 変更 | 規模 |
|---|---|
| `NODE_DEF` parse に `@allocates / @noalloc / @safepoint / @roots(...) / @root_array(b,c)` を追加 | `Node#initialize` の `@option` 解釈に分岐数行 |
| `Operand` に `@ref(value) / @imm / @weak / @atomic` 認識 | `Operand#initialize` の正規表現拡張 |
| `VALUE_DEF` parser (`parse_value_def`) | 新規 ~100 行、 `parse` 内で分岐 |
| `:gc` task の build_gc + per-node `build_gc` (marker + frame_desc) | 新規 ~150 行 |
| EVAL\_ ラッパに `@roots` 由来の frame ENTER/LEAVE + `#define name _f.name` を埋め込む | `build_eval_body` 内で 30 行程度 |
| SPECIALIZE 出力に SD\_\<hash\> 別 frame_desc を焼く | `build_specializer` を拡張 |

合計で `astrogen.rb` (現状 787 行) に対し +400 行程度。 数字は荒いが、
abruby が `register_gen_task :mark` 経由で実装した規模感 (~200 行) と
整合する。

## 25. サンプル展開 (interface 視点)

各 phase で `node.def` に何が増えるかを示すミニ例:

**Phase 0 — naruby/calc (GC なし)**: `node.def` は **一切変更しない**。
`make GC=none` で生成コード現状維持を gate にする。

**Phase 1 — asml (semispace)**:

```
VALUE_DEF asml_value
{
    INT     => atomic_payload(int64_t) i;
    CONS    => ref(VALUE) hd; ref(VALUE) tl;
    CLOSURE => ref(NODE *) @immortal body; ref_payload(VALUE) env;
    VARIANT => uint32_t ctor; ref_payload(VALUE) args;
}

NODE_DEF @allocates @roots(hd_v)
node_cons(CTX *c, NODE *n, NODE *hd, NODE *tl)
{
    VALUE hd_v = EVAL_ARG(c, hd);
    VALUE tl_v = EVAL_ARG(c, tl);
    return KORB_NEW_CONS(c, hd_v, tl_v);
}
```

**Phase 4 — koruby (generational)**: 全 allocate 系 NODE_DEF に
`@allocates`、loop back-edge / call に `@safepoint`、 `@ref(value)` を
inline cache に付与。`korb_obj` の `VALUE_DEF` は §2 例ほぼそのまま。

**Phase 7 — abruby (cruby backend)**: 同じ `VALUE_DEF` から
CRuby の `dmark` (rb_gc_mark callback) を生成する別 task を追加。
これは `register_gen_task :cruby_mark` で **既存の `:mark` を置き換え**、
embedder ごとに backend を選ぶ形にする。同じ DSL から 2 backend の
marker が出ることが framework 抽象化の正しさの proof になる (§15 の
副産物項目とも合流)。

## 26. `value.def` 採否の trade-off

ここまで (A)+(B)+(C) = §20-22 は **`value.def` 不要でも成立する** よう設計
してある。ここでは「`value.def` を更に上に乗せるか」だけを独立に検討する。

### 26.1 採用する場合の利得

- **abruby の `node_mark.c` を framework 標準に昇格** できる。 1 つの DSL
  から CRuby backend と precise backend の **両方** の marker が生成される
  ことが抽象化の正しさの proof になる (§15 副産物の中心)
- moving backend で必要な `KORB_FORWARD_<KIND>`、 generational で必要な
  `kind → mark function` テーブル等の boilerplate を 1 箇所に集約できる
- `asml` で HM 型推論結果から `atomic` payload を自動推定する将来拡張が、
  値の DSL 表現を経由してまとまる (§15)

### 26.2 採用しない場合の利得

- **言語側の値表現の癖を DSL に押し込まない**。 luastro NaN-box, naruby
  生 int64, koruby Flonum-tag, asml bit0=int 等を一つの構文に統合する圧力
  が消える (`feedback_no_nan_boxing` の方針と整合)
- abruby の `register_gen_task :mark` はサンプル個別の extension として残し、
  framework は **「root の場所」と「barrier 経路」だけ** 標準化する
- backend 移植時の自由度が高い (CRuby のような **既存 GC 経由 backend** を
  追加するときに DSL 同居制約に縛られない)
- 実装規模が小さい (§24 の `+400 行` のうち `VALUE_DEF` parser + `:gc` task
  だけで ~250 行を占めるので、 不採用なら +150 行程度に収まる)

### 26.3 折衷案: NODE オペランド注釈のみ採用、 VALUE_DEF は当面なし

§20 の operand 注釈 (`@ref(value)` / `@imm` / `@weak`) と §21 の `@roots` /
`@allocates` だけを採用し、 **value 側は当面言語ごとの C ヘッダに手書き
marker を持つ** という路線。

- abruby 流の per-sample `register_gen_task :mark` でも書ける
- root 列挙と barrier 経路は標準化される (= GC 切替の最重要部分は手に入る)
- value DSL の表現力チェックリスト (§13) を埋めずに済む
- ある程度サンプルが揃ってから「やはり共通 DSL が要る」となった時に
  後乗せできる (`VALUE_DEF` はあくまで *追加* 構文で、 既存サンプルの
  影響半径ゼロ)

**現実的にはこれが最も筋が良さそう**。 §17 の「arcel + asml をまず掘る」と
組み合わせると、 値表現の異なる 2 サンプルで「value DSL なしで GC 切替が
回せるか」を確かめてから採否を決められる。

### 26.4 判断保留

`value.def` 採否は arcel + asml で §20-22 を試してから決める。 §19 は
「もし採用するならこういう形」という参考スケッチ扱いに留め、 実装は §20-22
を先行させる。

## 27. 残るオープン項目

§16 で挙げた未決事項に対する、interface 視点での仮回答:

- (a) 同居/分離 → **同居**。本案の前提
- (b) `astro_gc_alloc` 戻り値の tag → **タグなし**、`KORB_NEW_*` macro が
  per-language の tag を被せる (現状の `KORB_PROC_NEW` 等の慣習延長)
- (c) JIT 生成 SD の frame descriptor → **`static const` を SD\_\<hash\>.c に
  焼く**。 `astro_cs_compile` で `SPECIALIZE_<name>` が SD 本体と一緒に
  frame_desc も emit する
- (d) `HEAP_FINALIZABLE` semantics → **BDW 風 topological** をデフォルト、
  `value.def` で `@finalizer F @order=<n>` の数値指定で override 可
- (e) `ref_array` 長さ表現 → **`count_var` 単独**。 begin/end は moving
  時の fixup が複雑になるので採用しない
- (f) `@linear` (nuq) → 当面 **保留**。 nuq の `linearity.c` が独自解析を
  持っており、 ASTro 標準 DSL に持ち上げる前に nuq 単体で `@linear`
  annotation を試す phase を挟む

## 28. interface design のまとめ

1 サンプルが `node.def` に書き足すのは最大で:
- `VALUE_DEF <type> { ... }` 1 ブロック
- 既存 `NODE_DEF` に `@allocates` / `@roots(...)` 系オプション
- `@ref` の sub-annotation (`(value)` 等)

BODY 内は **既存サンプルでは無変更**。GC 化に踏み込むサンプルだけが
`WB` / `KORB_NEW_*` / `PIN` macro を使う。 backend 切替で `GC=none` を
選んだとき全 macro が `(void)0` か identity に潰れることがゼロコスト性の
gate になる。

