# koruby v2 ブロック設計 — block / Proc / closure を slots ABI に載せる

Status: **設計ドラフト** (2026-06-13)。実装未着手。[v2_design.md](./v2_design.md)
の M1 スコープのうち block/yield/Proc/closure を扱う。確定度マーカーは
v2_design と同じ: ✅ = 設計から従う/合意済み、🤔 = 提案 (要レビュー)、❓ = 未決。

関連:

- [closure_sp_model.md](./closure_sp_model.md) — v1 の block 実装の総括。
  特に §2 (sp の A/B divergence)、§4-6 (in-place env 共有と clone/writeback)、
  §10.7 (sp 一本の正体)
- [v2_design.md](./v2_design.md) §7.8 (lvar 一本)、§13 #6 (iterator と ref 規約)
- [v2_m0_status.md](./v2_m0_status.md) — 土台 (M0) の現状

---

## 0. 何が問題だったか — v1 の block の構造 ✅

v1 の block は「**定義元 frame の stack slots を in-place 共有する env**」だった
(closure_sp_model.md §3-6)。そこから芋づる式に出たもの:

| v1 の機構 | 何のためにあったか |
|---|---|
| 役割 A/B の sp 二本 (A < B) | block body が iterator の frame の**下**で走るため |
| `creates_proc` flag + fresh-env clone | iteration ごとに独立 env が要るケース |
| `method_overlaps_env` 検出 + clone + writeback | in-place 実行が active frame を破壊するケース |
| `korb_proc_snapshot_env_if_in_frame` | method return 後も Proc が生きる (escape) ため stack→heap 昇格 |
| `env_size` 算定 (method locals + block locals の同居) | curry / nested lambda で破綻した (env_size バグ) |

**v2 の結論は「env を stack に置くのをやめる」**。captured 変数を最初から
heap に置けば、上の表は全部消える。これは V8 の context allocation /
Lua の upvalue close と同系の、確立された設計。

---

## 1. 一枚で分かる全体像 🤔

```
  方針: 「捕捉されるか」を parse 時に決め、捕捉される変数だけ heap の
        KorbEnv レコードに住まわせる。slots 上の lvar 機構は無傷。

  def m(a)              # a は block に捕捉される → env 行き
    x = 0               # x も捕捉される → env 行き
    y = 1               # y は捕捉されない → 普通の slot (M0 と同じ)
    each_n(3) { |i| x += a + i }
    x
  end

  m の frame:   [ a(slot:据置), x(slot:据置), y, …, env* ]   ← env* = 隠し slot
                                                  │
                       KorbEnv (heap, moving) ◄───┘
                       { parent=nil, vals[0]=a, vals[1]=x }
                          ▲
  block 値 (alloc なし): { iseq†, env, home‡ }  ← 呼び先 frame の隠し cell 3 つ
                          ▲
  block frame: [ i, …block locals…, outer_env*, … ]   ← yield が積む「普通の frame」
```

† iseq = parse 時に作る immortal な block メタ (NODE\* + arity 等)。
‡ home = 非局所 return / break の戻り先トークン (fixnum、§6)。

コア決定は 5 つ:

- **D1**: captured 変数は **frame 進入時に alloc する heap KorbEnv** に住む
  (単一の住所。slot との二重生活・writeback はしない)
- **D2**: block は**非 alloc**。`{iseq, env, home}` の 3 cell が呼び先 frame の
  隠し slot に乗って運ばれる。オブジェクト化 (`proc{}` / `&b`) した時だけ
  KorbProc を alloc
- **D3**: **yield は「callee が block iseq な call」と完全同型**。staged 引数窓 =
  block params 窓、frame は常に top に積む (v1 A4 の「常に fresh」を正式化)
- **D4**: 非局所脱出は RESULT.state の追加 (NEXT / BREAK) + fixnum トークン
- **D5**: Proc / lambda は KorbEnv を指す heap オブジェクト。escape 対応は
  「env が最初から heap」なので**何もしなくてよい**

---

## 2. D1: capture 解析と KorbEnv ✅(機構)/ 🤔(細部)

### 2.1 parse 時 capture 解析

scope (toplevel / def / block) ごとに、**その scope の local が内側の block から
参照されるか** (prism の depth > 0 参照) を判定する。transduce 前に当該 scope の
prism 部分木を 1 回スキャンする pre-pass を入れる (AST walk のみ、安い)。

- captured 変数 → **env index** を採番。lget/lset は env アクセスに bake
- captured でない変数 → M0 と同じ slot アクセス (一本モデル無傷)
- **param が captured の場合**: 呼出規約上 staged 窓 (slot) に届くので、
  frame prologue で env へ 1 回 copy する。以後 slot 側は読まない (単一住所)

### 2.2 KorbEnv — heap レコード

```c
typedef struct KorbEnv {            /* KORB_OBJ_ENV */
    AroObjectHeader head;
    uint32_t cnt;
    VALUE ARO_GC_EDGE parent;       /* 外側 scope の KorbEnv | nil */
    VALUE ARO_GC_EDGE vals[];       /* captured 変数の実体 */
} KorbEnv;
```

- SCAN_EDGES: parent + vals[*] を visit。moving 対応は自動
  (KorbEnv への参照はすべて VALUE として slot / 他オブジェクト経由)
- 割付けは capture を持つ frame の **prologue で 1 回** (`korb_alloc`)。
  alloc 時点で params は staged 窓に rooted 済み → alloc 後に env へ copy
  (`ARO_STORE`、gen backend の WB に乗る)
- env への参照は **frame の隠し slot** (`slots[env_off]`、offset は lvar と同じ
  bake 機構) に置く。**KorbEnv は movable なので C local の `KorbEnv *` を
  GC point 跨ぎで持たない** — アクセスごとに slot から読み直す
  (v2_design §7.5 の通常規則そのもの)

### 2.3 lvar 解決の拡張 (parse) 🤔

prism の (depth, index) を parser が **(hops, env_idx) または (slot_off)** に
解決する:

- depth == 0 & 非 captured → `node_lget(off)` (M0 のまま)
- depth == 0 & captured → `node_eget(env_off, idx)` (自 frame の env)
- depth > 0 → `node_eget_outer(outer_off, hops, idx)`。hops は「**env を持つ
  scope だけ**を数えた hop 数」(capture が無い中間 scope は env を作らない /
  chain に挟まらないので、prism depth からの読み替え表を parser が持つ)

`(1..3).each { |i| procs << proc { i } }` の v1 殺し (creates_proc):
i は block scope の captured 変数 → **block frame が iteration ごとに新しい
KorbEnv を alloc** → 内側 proc はそれぞれ自分の iteration の env を指す。
**clone も writeback も要らない**。これが D1 の最大の配当。

---

## 3. D2: block 値の表現 — 3 隠し cell、alloc なし 🤔

### 3.1 block iseq (immortal メタ)

```c
struct korb_biseq {                 /* parse 時 malloc、immortal (NODE と同格) */
    NODE *body;                     /* code_repo 登録済み = 自分の AOT entry */
    uint32_t params_cnt;
    uint32_t locals_cnt;            /* 隠し slot 込み frame サイズは別途 */
    uint32_t hidden_off;            /* outer_env / home 等の隠し cell 開始 */
    /* arity 詳細 (autosplat 等) は M1 後半で追加 */
};
```

### 3.2 値としての block = 3 cell

block は first-class 値ではないので、**呼び先 frame の隠し slot 3 つ**として
運ぶ (CRuby が cfp に block handler を載せるのと同じ発想):

| cell | 内容 | GC からの見え方 |
|---|---|---|
| blk_iseq | `(VALUE)((uintptr_t)biseq \| 1)` — **奇数タグの immortal ポインタ** | 奇数 = fixnum 扱いで skip ✅ |
| blk_env | 定義元 scope の KorbEnv \| nil | 普通の VALUE edge (scan される) |
| blk_home | 非局所脱出トークン (fixnum、§6) | fixnum で skip |

- block を渡さない呼出しは blk_iseq = nil (= `block_given?` が false)
- 隠し cell を持つのは「body が yield / block_given? / `&param` を使う
  method」だけ (parser が判定して locals に追加)。**使わない method は
  1 cycle も払わない**
- 奇数タグの immortal ポインタは**この隠し cell 専用の内部表現**とし、
  ユーザ値空間 (fixnum) とは決して混ざらない (yield 機構しか decode しない)
  ことを context.h に明記する。audit build では decode 時に
  「code_repo に実在する biseq か」を検査できる ❓
  - 代替案: KorbBlock を heap alloc して 1 cell にする。GC 的にはより素直
    だが **block 渡し呼出しごとに 1 alloc** (each ループの本丸に乗る)。
    タグ案で開始し、audit で不安が出たら差し替え 🤔

### 3.3 呼出規約

```
node_call1_blk(mid, line, cc@ref, biseq, a0@child)   ← biseq は固有 operand
  glue: a0 を staging (M0 と同じ)
  korb_call_blk(c, slots, mid, line, cc, argc, biseq, blk_env, blk_home)
    blk_env  = 自 frame の env (定義元 scope の env を継承。無ければ外側のを素通し)
    blk_home = §6 のトークン (call site で組む)
    → callee frame: [args | locals... | blk_iseq, blk_env, blk_home | (own env)]
```

`&blk` での転送 (`def m(&b); other(&b); end`) は cell 3 つの素通し。
Proc 化 (§5) するまで alloc は発生しない。

---

## 4. D3: yield = 「block を callee とする call」 ✅

```c
NODE_DEF
node_yield1(CTX *c, NODE *n, VALUE *slots, uint32_t line, VALUE_REF a0@child)
{
    return korb_yield(c, slots, /*argc=*/1, line);
}
```

korb_yield の仕事 (korb_call とほぼ同じ):

1. 自 frame の blk_iseq cell を読む。nil → LocalJumpError "no block given (yield)"
2. `base = slots - argc` (staged 引数窓 = block params 窓、M0 の call と同一)
3. arity 合わせ: 不足は nil 埋め、過剰は捨てる (block の緩い arity)。
   autosplat (`|a,b|` に Array 1 個) は M1 後半 ❓
4. limit check (slots + machine stack) → SystemStackError
5. 非 param block locals をゼロ埋め (= nil)
6. **block frame の隠し cell を書く**: outer_env = blk_env、home 系 = blk_home
7. block 自身が capture を持つなら prologue で KorbEnv alloc (parent = blk_env)
8. `(*biseq->body->head.dispatcher)(c, body, base + frame_size)` —
   **常に現在の top に積む**。v1 の「iterator の下で走る」A<B divergence は
   構造ごと存在しない
9. RESULT 処理: NEXT → NORMAL に畳む (§6)。RETURN / BREAK / RAISE は素通し

**block body の cursor 規約は method body と完全に同一**。node.def に block
専用の特例は無く、M0 の全ノードが無変更で block body 内でも正しい。

### 4.1 iterator builtin と ref 規約 (v2_design §13 #6 の答) ✅

C で書く iterator (M1 の `Array#each` / `Integer#times` 等) は:

```c
RESULT korb_ary_each_ref(CTX *c, VALUE *slots, VALUE_REF ary)
{
    for (long i = 0; ; i++) {
        if (i >= KORB_ARRAY_LEN(VALUE_REF_GET(ary))) break;  /* 毎回 ref 経由 */
        VALUE_REF a0 = SLOTS_PUSH(slots, korb_ary_at(VALUE_REF_GET(ary), i));
        CHECK_YIELD(korb_yield(c, slots, 1, line));   /* GC 任意 / BREAK 透過 */
        slots--;                                      /* 窓を巻き戻す (local cursor) */
    }
    return RESULT_OK(VALUE_REF_GET(ary));
}
```

- receiver は VALUE_REF で持ち、**yield (任意コード実行) を跨ぐ読みは毎回
  GET し直す** — §7.5 の通常規則で閉じる。特例なし
- 要素 staging は SLOTS_PUSH → yield の引数窓、と M0 の規約のまま

---

## 5. D5: Proc / lambda ✅(方針)/ 🤔(細部)

```c
typedef struct KorbProc {           /* KORB_OBJ_PROC */
    AroObjectHeader head;
    uint32_t flags;                 /* KORB_PROC_LAMBDA */
    struct korb_biseq *biseq;       /* immortal — SCAN_EDGES は触らない */
    VALUE ARO_GC_EDGE env;          /* KorbEnv | nil */
    VALUE home;                     /* fixnum トークン (§6) — scan 不要だが VALUE で保持 */
} KorbProc;
```

- `proc {}` / `lambda {}` / `-> {}` / `&b` 受け = 隠し cell 3 つを包むだけ。
  **escape 処理は存在しない** — env は生まれつき heap なので、定義元 method が
  return しても Proc から到達可能な間 KorbEnv は GC が生かす。
  v1 の `snapshot_env_if_in_frame` / `FL_HAS_PROC_IVARS` walk は廃止
- `Proc#call` = korb_yield と同じ frame 構築を KorbProc から行う。
  **常に top に積む**ので v1 の「slot 衝突 / overlap clone」も無い
- lambda: arity 厳格 (ArgumentError は method と同文言)。`return` は局所
  (KORB_RETURN を lambda 呼出境界で畳む)。proc: arity 緩く、`return` は
  非局所 (§6)
- 依存: `p.call` は receiver method call なので **M1 の receiver dispatch
  実装が前提** (block/yield 自体は前提にしない — 進め方 §9 参照)

---

## 6. D4: 非局所脱出 — RESULT state 追加 + fixnum トークン 🤔

state を 2 つ追加する。**伝播経路 (UNWRAP/CHECK) は無変更**で、境界だけが
新 state を畳む:

| state | 発生 | 畳む場所 |
|---|---|---|
| KORB_NEXT | `next [v]` (block body 内) | korb_yield / Proc#call (→ NORMAL、値 = v) |
| KORB_BREAK | `break [v]` (block body 内) | **block を供給した call site** (→ NORMAL、call の値 = v) |
| KORB_RETURN (既存) | `return` | method 境界 (M0 のまま)。**block 内 return** は home が一致する method 境界まで素通し |

### 6.1 トークン (blk_home) の中身

slots バッファは CTX 生存中アドレス固定なので、**frame base の slot offset が
活性 frame の一意な座標**になる。これに reuse 対策の serial を足して 1 fixnum
にパックする:

```
home = LONG2FIX( (frame_serial << 21) | frame_base_slot_off )
        21 bit = 8 MiB / 8 B / slot ≤ 2^20 slot (KORUBY_SLOTS_BYTES 拡大時は桁再配分)
        frame_serial = vm->serial++ (block を渡す / 定義する活性化ごと)
```

- **break 用**: block を渡す call node が自分の活性化トークンを組んで cell に
  載せる。BREAK が戻ってきた call site は「自分のトークン == 例外側の
  トークン」なら畳む。再帰中の同一 call site も serial で区別される
- **return 用**: block 定義時点の「lexically enclosing method frame」の
  トークン。block cell 生成時に自 frame のものを継承して渡す
- 不一致のまま toplevel / Proc 境界に達したら **LocalJumpError**
  ("break from proc-closure" / "unexpected return") — escape した Proc からの
  break/return が CRuby と同じ失敗をする
- 運搬: BREAK/RETURN の戻り値は RESULT.value に乗せ、トークンは
  `vm->nonlocal_token` (CTX 配下、in-flight 1 個) に置く。**raise と同じく
  unwind 経路に GC point は無い**ので stale 化しない。rescue (M1) が
  RAISE と同様に「畳まれずに横切る」ケースは ensure 実装時に要整理 ❓

### 6.2 段階導入

next (トークン不要) → break (call site トークン) → return-from-block /
proc (home トークン) の順に入れる。最初の gate は next だけで張れる。

---

## 7. GC contract 追加分 ✅

| 物 | 種別 | SCAN_EDGES |
|---|---|---|
| KorbEnv | moving heap | parent + vals[0..cnt) |
| KorbProc | moving heap | env のみ (biseq は immortal なので**触らない**) |
| 隠し cell blk_iseq | 奇数タグ → 偶数 8-align でないので skip | — |
| 隠し cell blk_env / outer_env | 普通の VALUE slot (frame 内) | root scan が拾う |
| 隠し cell blk_home / home | fixnum | — |

- env への書き (`vals[i] = v`、eset) は **ARO_STORE** 経由 (gen backend WB)。
  M0 で導入済みの discipline のまま
- STRESS+PURGE の効き所: block frame prologue の env alloc が「毎 yield GC」
  になるので、capture 周りの rooting 漏れは即日炙り出される (gate §9)

---

## 8. AOT / 部分評価との整合 🤔

- **block body = 独立 entry** ✅: yield は frame cell 経由の runtime dispatch
  なので specializer は畳めない (feedback_runtime_dispatch_entries と同じ)。
  parse 時に code_repo へ登録し、bake 対象に含める (M0 の method body と同列)
- **call site の biseq operand**: malloc ポインタなので SD には焼けない →
  operand override で `n->u.x.biseq` の runtime 参照を emit (M0 の
  `const char *` / line と同じ手口)。HASH には biseq->body の構造 hash を
  畳み込む (同一 block 構造の call site は SD 共有)
- **eget/eset の SD**: env_off / hops / idx は uint32 operand なので定数で
  焼ける。env load の間接 1 段は moving GC の代償として残る (v2_design §8.2
  R1 と同種 — heap 値はどうせ fold 禁止なので新たな後退ではない)
- リスク: yield の間接 call が iteration ごとに残る。v1 でもそうだった。
  block-site speculation (PG で biseq を仮定して直 call + guard) は
  §13 #9 (IC 防衛) と同じ枠で M2 以降に計測してから ❓

---

## 9. 進め方と gate 🤔

GC は常に copy + STRESS/PURGE、AOT 常時 green (M0 と同じ常設 gate)。

| 段 | 内容 | gate |
|---|---|---|
| **B1** | capture 解析 + KorbEnv + eget/eset (block なしでも capture 機構単体を `def` 内 lambda 不使用のダミーで…は不可能なので、B1+B2 は一括) | — |
| **B2** | block literal + yield + next + block_given? + 隠し cell 機構。**receiver call 非依存**: `def each_n(n); i=0; while i<n; yield i; i+=1; end; end` 級が駆動コーパス | rubyharness CAT=block の該当分 + 手書き syntax/hand_* green (STRESS+PURGE 込み)。bench/block.rb は receiver iterator 依存なら持ち越し |
| **B3** | `&b` param / `proc{}` / `->{}` / KorbProc。Proc#call は receiver dispatch (M1 並行作業) が入り次第 | proc_closure 系 green。**curry / nested-lambda 再現テスト** (v1 の env_size バグの回帰枠) を rubyharness 側に追加 |
| **B4** | break / return-from-block / LocalJumpError | exception 系の該当分 green |
| 以後 | autosplat、numbered params (`it`/`_1`)、self/cref (class 統合と同時) | — |

計測 (gate ではなく記録): capturing frame の env alloc コスト、eget 間接の
fib/loop 影響ゼロ確認 (非 capture コードに 1 命令も増えないこと)、
bench/block.rb / closures.rb の v1 凍結値・CRuby 比。

---

## 10. v1 から消えるもの / 残る原理 ✅

| v1 | v2 |
|---|---|
| env = stack in-place 共有 | captured だけ heap KorbEnv (単一住所) |
| creates_proc / fresh-env clone + writeback | 構造ごと消滅 (iteration ごとに自然に新 env) |
| method_overlaps_env 検出 + clone | 消滅 (block frame は常に top) |
| snapshot_env_if_in_frame (escape 昇格) | 消滅 (env は生まれつき heap) |
| env_size = method+block locals 同居 (curry バグ) | 消滅 (scope ごと独立 env / frame) |
| sp 役割 A < B (block 下走り) | 消滅 (yield = 普通の call) |
| c->current_block (global 連鎖) | 消滅 (frame 隠し cell。per-CTX どころか per-frame) |

残る原理は M0 と同じ 3 行 — slots は top、publish は korb_alloc だけ、
GC 跨ぎは ref 経由。block はその上の「callee が biseq な call」にすぎない。

---

## 11. 未決事項 ❓

| # | 論点 | 決め方 |
|---|---|---|
| 1 | blk_iseq の奇数タグ vs KorbBlock heap 化 (§3.2) | B2 で audit/STRESS を通してから。alloc コストは bench/block.rb で |
| 2 | per-scope KorbEnv レコード vs 変数単位 box。レコード採用だが、「1 個だけ捕捉」が支配的なら box の方が軽い可能性 | B3 後に capture 統計を採ってから |
| 3 | autosplat / implicit_rest の arity 細部 | B2 後、rubyharness の block corpus 差分で駆動 |
| 4 | nonlocal_token と rescue/ensure (M1) の交差 — ensure は BREAK/RETURN も横切らせつつ実行する必要 | ensure 実装と同時に設計 |
| 5 | home serial の桁配分 (KORUBY_SLOTS_BYTES 拡大時) | B4 実装時に encode を 1 箇所に閉じ込めておく |
| 6 | self / cref の block 継承 | M1 class 設計と同時 |
| 7 | yield site の block speculation (間接 call 除去) | M2 以降、PG/IC 枠で計測してから |
