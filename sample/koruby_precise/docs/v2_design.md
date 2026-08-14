# koruby v2 設計 — slots ABI による全面再構築

Status: **実装済み** — 本書は 2026-06-12 の設計ドラフトだが、v2 はこの設計どおりに
実装され、現在の `sample/koruby_precise` そのものになっている (M0 から積み上げ、
optcarrot が checksum 一致で動き、rubyspec core は実 mspec で 80%)。
**設計意図を読む文書**として維持しており、実装後に変わった点は本文中に注記がある。
現状の機能一覧は [done.md](./done.md)、CLI/gate の仕様は [v2_spec.md](./v2_spec.md)、
残タスクは [todo.md](./todo.md) を見ること。

以下の確定度マークは**設計当時のもの**（実装で解決済みのものも含む）:

- ✅ = 議論済み・合意
- 🤔 = 提案 (Claude 推奨、要レビュー)
- ❓ = 未決 (M0 spike や計測で決める)

関連ドキュメント:

- [v2_blocks_design.md](./v2_blocks_design.md) — M1 の block / Proc / closure 設計
  (§7.8 の「blk->env 別系統」注記はこちらで具体化: captured 変数は heap KorbEnv)
- [closure_sp_model.md](./closure_sp_model.md) — v1 の sp 二本問題の総括 (特に §10.7)
- [sp_transition_analysis.md](./sp_transition_analysis.md) — v1 sp 契約の全経路監査
- `../../../docs/idea.md` — ASTro の核心 (EVAL / DISPATCH / SD / 部分評価)
- `../../../docs/gc_design.md` — 統一 GC 基盤の設計
- `../../rubyharness/` — 検証基盤 (CRuby オラクル差分テスト + 多モード bench)

---

## 0. 何を作るか・なぜ作り直すか ✅

v2 は「**GC 安全性がコードの構造から自動的に出てくる C ABI**」で koruby を
書き直すプロジェクト。precise moving GC を前提に、root 管理を `slots` 一本に
統一する。

**v1 の実装は捨てる。互換性は考慮しない。** 検証の契約は rubyharness
(CRuby との差分テスト) だけ。v1 (koruby_precise) は性能と PASS 率の
基準線・もう一つのオラクルとして凍結して残す。

### v1 で構造的に解消不能と確定したこと

1. **sp と c->sp_top の二本立て** — sp (frame top、lvar 用) と c->sp_top
   (動的 staging top) は別概念で、機械置換は原理的に不可 (closure_sp_model.md
   §10.7 で実証)。c->sp_top への store は 900+ 箇所
2. **偽 frame parking** — node body が child の値を GC から守るために
   synthetic frame の last_line に退避する hack が各所に必要だった
   (staging と rooting が別系統だったため)
3. **RESULT / per-CTX 移行が中途半端** — c->state 併存、境界に新旧 ABI 混在
4. **libc malloc / arena 混在** — container が xmalloc で struct-moving 移行が
   頓挫 (STRESS+PURGE で stale 連鎖)
5. **AOT が静かに壊れた** — 後回しにした結果 dispatcher swap が hash 不一致で
   無効化していた

v2 はこれらを「最初から正しい ABI で書く」ことで設計から排除する。

---

## 1. 一枚で分かる全体像 ✅

```
CTX *c
 ├─ c->slots ──────→ 値スタック (VALUE の線形配列, mmap, 動かない)
 ├─ c->slots_top      GC scan の上端。korb_alloc 系だけが書く (publish)
 └─ (frame chain / per-CTX VM root table など)

        値スタック (低位 → 高位)
        ┌────────────────────────────────────────────┐
        │ ... 呼び出し元の live データ ... │ 自分の free 領域 ... │
        └────────────────────────────────────────────┘
                                         ↑
                                       slots  ← 全関数がこれを受け取る
```

コア原則は 3 つだけ:

1. **渡ってくる `slots` は常に top** (= 最初の空き slot)。
   live なものは全部 `slots` より下、上は自分の scratch。
2. **`c->slots_top` を書くのは `korb_alloc` 系だけ**。
   alloc が `c->slots_top = slots` を publish してから collect する。
   GC は `[c->slots, c->slots_top)` を scan して fixup する。
3. **GC を跨いで生の `VALUE` を持たない**。跨ぐ必要がある値は slot に置き、
   `VALUE_REF` (型で強制された rooted 参照) 経由で触る。

この 3 つから「いつでも GC してよい」「move されても壊れない」が出る。

---

## 2. 用語と前提知識

### 2.1 may_gc — 「GC に到達しうるか」 ✅

同じ性質を 3 つの粒度で使うので、定義を固定する。

| 粒度 | 定義 | 出どころ |
|---|---|---|
| may_gc(C 関数) | korb_alloc に到達しうる | 命名規約 / annotation (CodeQL が call graph で検証) |
| may_gc(node kind) | その kind の EVAL body + glue 自体に GC 点があるか。**child の評価は含まない** | node.def の宣言 (`@nogc` の逆、デフォルト true = 安全側) |
| may_gc(部分木) | may_gc(根の kind) ∨ ⋁ may_gc(子部分木)。method call / yield を含めば無条件 true | specialize 時に tree を畳んで計算 |

### 2.2 moving GC で何が壊れるか ✅

- GC はオブジェクトを **move** する。move 前のアドレスを持つ生の `VALUE` は
  GC 後に **stale** (PURGE 下では決定的に SEGV)。
- GC は **root から辿れる slot の中身は書き換えてくれる** (fixup)。
  だから「slot 経由で読み直せば常に正しい」。
- ただし **slot 自体が move する場所にあるとダメ**。object の ivar 領域は
  scan はされるが、object ごと move したら「その slot を指していた C ポインタ」
  が dangle する。

### 2.3 stable root — 参照を向けてよい場所 ✅

`VALUE_REF` が指してよいのは、(a) GC が scan する、かつ (b) その場所自体の
アドレスが GC を跨いで安定、の両方を満たす場所だけ。

| 指し先 | scan される | アドレス安定 | ref として |
|---|---|---|---|
| slots バッファ | ✓ | ✓ (固定 mmap) | **OK** (典型) |
| CTX root field / VM root table | ✓ | ✓ | OK |
| immortal object の field (NODE の literal / IC slot。§9.3) | ✓ (要: GC が edge を trace) | ✓ (move しない) | OK (🤔 初期は禁止し slots 経由に限定する案も) |
| C stack 上の frame struct の field (chain 経由で scan) | ✓ | ✓ | OK |
| **movable object の ivar / payload** | ✓ | **✗ (move で dangle)** | **NG** |
| C local の `&v` | ✗ (precise = C stack を scan しない) | ✓ | NG |

系: **move する heap payload への interior pointer を返す / 受け取る API は
全面禁止**。要素を触る API は `(VALUE_REF container, long idx)` の形で渡す
(CRuby が `RARRAY_PTR` を C 拡張に渡して Array を move できなくなった轍を
踏まない)。slots への slice (§5.2) は non-moving なので例外的に OK。

---

## 3. slots — cursor 規約 ✅

### 3.1 「渡ってくる slots は常に top」

すべての関数 (EVAL body、runtime helper、builtins) は
`(CTX *c, VALUE *slots, ...)` を受け取る。`slots` はその時点の
**最初の空き slot**。

- 自分宛の staged データ (@child の結果など) は `slots` の**下** (負 offset)
- `slots[0]` 以上は自分の scratch。call を跨いで生かしたい値は
  `SLOTS_PUSH` で cursor の下に入れてから渡す
- callee へは、push していなければ `slots` を**そのまま**渡す
- return すれば cursor は値渡しなので**自動で pop** (明示の cleanup 不要)

### 3.2 なぜ「top を渡す」のか

1. **「+N を忘れる」バグクラスが消滅する**。「base を渡して callee 側で
   +使用数」の規約だと、手書きコードの全 call site が正しい N を計算する
   義務を負う。1 箇所間違えると staged 値が publish 範囲の上に出て
   **GC が黙って見逃す**。top 規約なら危険な算術は生成 glue の中に 1 箇所だけ。
2. **korb_alloc の publish が無条件になる**: 受け取った `slots` を
   そのまま `c->slots_top = slots` するだけ。live は定義により全部その下。
3. **lvar アクセスと同じ機構に統一される** (§7.8)。lvar / @child / 一時 push
   が全部「top の下は rooted、上は free」の一つの規則に畳まれる。
   これが v1 で不可能だった「sp 一本」の正体。

### 3.3 korb_alloc — 唯一の publish 点

```c
VALUE korb_alloc(CTX *c, VALUE *slots, size_t size /* , type ... */);
/* 内部:
 *   c->slots_top = slots;        ← publish (これ以外の場所で slots_top を書かない)
 *   必要なら collect;             GC scan = [c->slots, c->slots_top)
 *   割り付けて返す
 * 呼ぶ瞬間の不変条件: 生きている VALUE はすべて slots より下 (または他の stable root) */
```

### 3.4 cursor は値渡し — callee は caller の root 領域を伸ばせない ✅

`SLOTS_PUSH` が書き換えるのはローカルコピーの cursor だけ。return すれば
caller の cursor 位置に戻る (= 自動 pop)。つまり **callee が push した値は
return 後 root されない**。pop 忘れバグが存在しない代わりに、「値を上に
返す」経路は 3 つに限られる:

1. **RESULT 戻り値 (1 個)** — register で返る。caller は「次の GC point
   までに root するか使い切る」(= §7.5 の VALUE local 規則そのもの)
2. **out-param (複数個)** — caller が先に `SLOTS_PUSH(slots, Qnil)` で
   placeholder を確保し、`VALUE_REF` で渡す。callee は caller の領域を
   伸ばせないが、**確保済み cell への SET はできる**:

   ```c
   VALUE_REF q = SLOTS_PUSH(slots, Qnil);
   VALUE_REF m = SLOTS_PUSH(slots, Qnil);
   CHECK(korb_int_divmod(c, slots, a, b, q, m));   /* callee が SET で書く */
   ```
3. **heap に置く** (Array で返す等)

この制約から従う設計判断:

- **@child の staging が生成 glue (DISPATCH) 内にある理由**。「子を N 個
  評価して slots に積む」は cursor を所有する関数の中でしかできない
  (helper に切り出すと return で pop される)。staging は ASTroGen が
  その場に inline で吐くしかない
- v1 の `c->sp_top` (global staging top) は「callee が caller を跨いで
  積む」を global で実現していた。v2 はそれを禁止した代償として
  out-param 様式と「staging は cursor 所有者がやる」規約を払う
- 「いくつ返るか callee しか知らない」可変長ケース (splat 展開等) は
  out-param で書けない — 未決事項 #8

### 3.5 c->slots は動かせない — バッファのサイズ戦略 ✅(戦略の細部は 🤔)

`slots` cursor と `VALUE_REF.p` は C のレジスタ・native stack 上に
**runtime から見えない形で**散在する。Go の movable stack (成長時に
copy + 全ポインタ書き換え) はコンパイラが全 frame の stack map を持つから
できることで、plain C の v2 では不可能。**`c->slots` のアドレスは CTX の
生存中、不変**でなければならない。

従って成長戦略は「move」ではなく「最初から大きく仮想予約」:

- `mmap MAP_NORESERVE` で **デフォルト 8 MiB / CTX** を仮想予約
  (pthread スタックの default と同じ「普通」のサイズ)、物理は touch 時に
  lazy commit。8 MiB = 2^20 ≒ 100 万 slot、1 frame 数十 slot として
  再帰数万段 — CRuby と同等の感覚で SystemStackError が出る。
  実 RSS は touch した分だけ。fiber 数の上限はアドレス空間ではなく
  `vm.max_map_count` (default 65530、guard page 分割で 1 CTX ≒ 2-3 VMA
  → デフォルト設定で 2-3 万 fiber、sysctl で拡張可) が先に効く。
  サイズは環境変数等で可変にしておく (深い再帰のテスト用)
- 暴走再帰は **frame push 時に `slots > c->slots_limit` を 1 比較**して
  `SystemStackError` を raise (CRuby の stack check と同じ流儀。node 単位
  ではなく frame 単位なのでコストほぼゼロ)
- 予約末尾の guard page は検査をすり抜けた場合の最後の防壁

系: 将来の Fiber / Thread は「1 CTX = 1 固定予約」。予約は増やせても
move はできないので、CTX を大量生成する設計をするときはこの制約が効く。

### 3.6 注意: slots に `restrict` を付けない ✅

GC が `c->slots` 経由で slot を fixup する (= 別ポインタからの書き込みが
ある) ので、`restrict` の前提に反する。const/restrict 積極方針の明示的な
例外とする。

---

## 4. 関数 ABI

### 4.1 引数の 3 つの型 ✅

| 型 | 意味 | いつ使う |
|---|---|---|
| `VALUE` | 値そのもの。root されない | GC point の**後**にその引数が要らないとき |
| `VALUE_REF` | rooted cell 1 個への参照 (§5) | GC point の後にその引数の (fixup 済みの) 値が要るとき |
| `VALUE_SLICE` | rooted cell の連続列 + 個数 (§5.2) | argv / 可変長要素列 |

**判定基準は「GC するか」ではなく「GC point の後に引数がまだ要るか」。**

### 4.2 値渡し (`VALUE`) で済む may_gc 関数の 3 パターン ✅

GC する関数でも、以下なら引数は値渡しでよい:

1. **引数が immediate** (Fixnum / Flonum / nil / true / false / Symbol)
   — move されない。例: `korb_fix_plus(c, slots, VALUE l, VALUE r)` は
   overflow で bignum を **alloc するが**、l/r は fixnum なので安全
2. **引数が immortal** (NODE 等。§9.3) — move されない。
   **Class は moving なので該当しない**点に注意 (v1 との違い: klass を
   GC 跨ぎで持つなら普通に rooting が要る)
3. **必要な情報を GC の前に C スカラへ抽出し切る**。例:
   `korb_float_plus` は double を取り出して**から** alloc する
   (GC 後に l/r を見ない)

### 4.3 `_ref` (VALUE_REF 渡し) が要る 3 つの形 ✅

1. **grow-then-write** — 一番分かりやすい型。容量拡張 (= alloc = GC) して
   **から**書く:

   ```c
   RESULT korb_ary_push_ref(CTX *c, VALUE *slots, VALUE_REF ary, VALUE_REF item)
   {
       CHECK(korb_ary_ensure_capa_ref(c, slots, ary, 1));  /* alloc し得る */
       VALUE a = VALUE_REF_GET(ary);                        /* GC 後でも fixup 済み */
       KORB_ARRAY_PTR(a)[KORB_ARRAY_LEN(a)++] = VALUE_REF_GET(item);
       return RESULT_OK(a);
   }
   ```

2. **alloc-then-copy** — 結果オブジェクトの alloc が先、ソースの読み出しが後
   (str_plus, ary_plus, dup / 構築系)。`+` のような演算もここに入るが、
   演算の顔をしていて順序依存が見えにくいので説明例には 1. を使う

3. **任意コード実行** — method dispatch、`to_str` 変換、block 呼び出しを
   挟むもの。GC 点が特定できないので無条件 ref

**罠**: 2. の「alloc の前に C の一時バッファへ copy しておけば値渡しで済む」
は **moving GC では shadow-buffer バグ** (malloc バッファ内の heap ポインタは
GC の scan 範囲外で stale 化する。baruby_precise で実際に踏んだ)。
ref で受けるのが正解。

### 4.4 レイヤリング: 外は ref、内は value ✅

変換コストが非対称なことを利用する:

- **VALUE_REF → VALUE は deref するだけ (タダ)**
- **VALUE → VALUE_REF は SLOTS_PUSH (store + slot 消費) が要る**

なので「型未確定・dispatch しうる外側は ref で受け、型を確定させた内側の
leaf へは deref して値で渡す」と、**どの境界でも詰め替えが発生しない**:

```c
RESULT korb_node_plus_slow_ref(CTX *c, VALUE *slots, NODE *n,
                               VALUE_REF l, VALUE_REF r, uint32_t arg_index)
{
    VALUE lv = VALUE_REF_GET(l), rv = VALUE_REF_GET(r);
    if (FIXNUM_P(lv) && FIXNUM_P(rv))
        return korb_fix_plus(c, slots, lv, rv);       /* deref して値で内側へ */
    if (KORB_STRING_P(lv) && KORB_STRING_P(rv))
        return korb_str_plus_ref(c, slots, l, r);     /* ref は素通し */
    return korb_dispatch_binop_ref(c, slots, n, l, r, arg_index);  /* 任意 GC */
}
```

### 4.5 便利 wrapper (foo / foo_ref の二枚看板) ✅

値しか持っていない呼び出し側のために、push してから `_ref` を呼ぶ
wrapper を用意してよい:

```c
static inline RESULT
korb_str_concat(CTX *c, VALUE *slots, VALUE str, VALUE append)
{
    VALUE_REF str_r = SLOTS_PUSH(slots, str);     /* cursor が 2 進む */
    VALUE_REF app_r = SLOTS_PUSH(slots, append);
    return korb_str_concat_ref(c, slots, str_r, app_r);  /* 進んだ cursor を渡す */
}
```

命名規約: pointer 引数を持つ関数は `_ref` suffix。C は overload がないので
値渡し版と並存させるための見分けでもある。

### 4.6 例外は RESULT ✅

例外を起こしうる関数は**すべて** `RESULT` (VALUE + state、16 bytes =
2 register 返し) を返す。v1 Phase 8 の規約を全面適用:

- `RESULT_OK(v)` で包む、`UNWRAP(result)` / `CHECK(result)` で伝播
  (引数は RESULT を返す式。典型は call をそのまま inline で包む)
  (`RESULT chk = ...; if (...) return chk;` の手書きは禁止)
- **c->state も c->errinfo も v2 に存在しない** — CTX 側チャネルを作らず、
  例外オブジェクトは RESULT.value で運ぶ。これで成立する理由:
  - UNWRAP / CHECK の伝播経路は「state を見て即 return」だけで
    **GC point を含まない** → register 運搬のままで安全
  - 例外を持ったまま仕事をする場所 (rescue 本体 / ensure / backtrace 付加 /
    retry) だけが root の義務を負う: 入口で `SLOTS_PUSH(slots, r.value)`。
    これは §7.5 の「VALUE local を may_gc 跨ぎで持たない」**通常規則
    そのもの**で、例外用の特例は無い (CodeQL も同じクエリで検査)
- `$!` は CTX field ではなく **rescue 節ごとに parser が確保する hidden
  frame slot** に置く。読みは普通の lvar 機構、nested rescue の
  シャドーイングも frame 構造から自然に出る。引数なし `raise` の再 raise
  も同じ slot を読む

---

## 5. VALUE_REF / VALUE_SLICE — 型による強制

### 5.1 VALUE_REF ✅

```c
typedef struct { VALUE *p; } VALUE_REF;   /* 1-member struct → register 渡し */

#define VALUE_REF_AT(ptr)     ((VALUE_REF){ (ptr) })
#define VALUE_REF_GET(r)      (*(r).p)
#define VALUE_REF_SET(r, v)   (*(r).p = (v))
/* audit build では GET/SET が korb_ref_check (stable-root 判定) +
 * stale 値検査 (gc_clock) を通る版に切り替わる */
```

ポイント:

- **生の `VALUE *` が ABI から消える**。`*ptr` と書きようがなくなるので、
  「deref は必ず検査 hook を通る」が型レベルで保証される
  (ARO_GC_EDGE qualifier + Werror で direct write を全捕捉したのと同じ発想の
  deref 版)
- GET / SET を分けてあるのは audit の中身が違うから
  (GET = stale 読み検査、SET = 不正値書込み検査 + 将来の write barrier hook 置き場)
- `VALUE_REF_AT` に渡してよいのは **slots 由来 (`&slots[k]` / SLOTS_PUSH の
  戻り) か、受け取った ref の素通しだけ**。`&local` や `&obj->ivar` は禁止
  (§2.3)。CodeQL が出所を追う (§10)

ivar の値を ref で渡したいときは copy + writeback:

```c
VALUE_REF tmp = SLOTS_PUSH(slots, korb_ivar_get(VALUE_REF_GET(obj), id));
CHECK(korb_str_upcase_bang_ref(c, slots, tmp));
korb_ivar_set(c, obj, id, VALUE_REF_GET(tmp));   /* obj も VALUE_REF なので GC 後も safe */
```

### 5.2 VALUE_SLICE 🤔

```c
typedef struct { VALUE *p; uint32_t cnt; } VALUE_SLICE;

VALUE_SLICE_GET(s, i)    /* audit: i < cnt の bounds 検査も付く */
VALUE_SLICE_SET(s, i, v)
VALUE_SLICE_LEN(s)
VALUE_SLICE_REF(s, i)    /* i 番目への VALUE_REF を切り出す */
```

- slots 領域内の**連続 rooted 列**の view。v1 の cfunc ABI
  `(c, int argc, VALUE *sp)` (argv idiom) の型強制版
- method call の引数 staging と array literal の要素 staging が同じ機構になり、
  staged した slot 列を**コピーなしで** callee の argv として渡せる
- 「interior pointer 禁止」の例外: slots バッファは non-moving なので slice は安全

### 5.3 命名: 値型名から導出する (`VALUE_REF`) ✅

`VALUE_REF` は「`VALUE` への ref」— **名前は言語の値型名から導出する**。
framework (ARO_) 層は将来「言語ごとに値型の名前を変えられる」方針なので、
値型を焼き込むこの機構を ARO_ 固定名にはできない。型名側に寄せておけば、
値型を `LV` と名付けた言語では `LV_REF` / `LV_SLICE` になる、という
自明な対応になる。

実装は framework が `#include` template で供給する (runtime/astro_node.c の
前例と同じ):

```c
#define ASTRO_REF_VALUE  VALUE    /* 言語の値型名 */
#include "astro_ref_template.h"   /* VALUE_REF / VALUE_REF_GET / VALUE_SLICE / ... が生成される */
```

---

## 6. マクロ早見表 ✅

| 名前 | 役割 |
|---|---|
| `VALUE_REF` / `VALUE_SLICE` | rooted 参照の型 (§5) |
| `VALUE_REF_AT(p)` | `&slots[k]` などから VALUE_REF を構築 |
| `VALUE_REF_GET(r)` / `VALUE_REF_SET(r, v)` | 検査 hook 付き読み書き |
| `SLOTS_PUSH(slots, v)` | v を cursor 位置に store、**cursor (local 変数) を 1 進め**、その cell への VALUE_REF を返す |
| `RESULT_OK(v)` | 正常値を RESULT に包む |
| `UNWRAP(result)` / `CHECK(result)` | RESULT 伝播 (非 NORMAL なら early return。値を取る / 捨てる) |
| `korb_alloc(c, slots, ...)` | 割付け。`c->slots_top = slots` を publish する唯一の場所 |
| `EVAL_ARG(c, node, slots)` | lazy operand の評価 (相棒 dispatcher 経由、§7.5) |
| `$name` | node.def body 内の一時 slot 宣言 (slots 負 offset に置換) |

禁止事項 (検査は §10):

| 禁止 | 強制手段 |
|---|---|
| 生の `VALUE *` 引数・`*ptr` deref | VALUE_REF 型で構文的に不可能 |
| `&local` / `&obj->ivar` を VALUE_REF_AT へ | audit build + CodeQL |
| `c->slots_top` への直書き (korb_alloc 系以外) | CodeQL |
| may_gc call を跨ぐ VALUE local の使用 | CodeQL + STRESS/PURGE |
| `slots` への restrict | レビュー規約 (§3.6) |
| movable payload への interior pointer API | API 設計規約 (§2.3) |

---

## 7. node.def 規約

### 7.1 前提: EVAL / DISPATCH / SD (idea.md の復習) ✅

- **EVAL_xxx** = node.def に書いた評価ロジック本体。不透明テキストとして
  転写される (ASTroGen は C を解析しない)。static inline
- **DISPATCH_xxx** = NODE の field を取り出して EVAL に渡す薄い glue。生成物
- **SD** = 部分評価で焼いた specialized dispatcher。DISPATCH と同じ形を
  operand 定数・子 SD 直呼びで吐き、EVAL が inline 展開されて畳まれる

v2 の @child 機構はこの構造の上に乗る。staging は DISPATCH / SD 側
(生成コード) に置き、EVAL body は受け取るだけ。

### 7.2 @child — strict operand の 2 形態 ✅

@child operand は「glue が先に評価して body に渡す」strict 意味論の operand。
**宣言の型で受け取り方を選ぶ**:

| 宣言 | glue の動作 | body での見え方 |
|---|---|---|
| `VALUE x@child` | staging **なし**、register でそのまま渡す | 普通の VALUE 引数。`&x` 禁止 |
| `VALUE_REF x@child` | `slots` の下に store してから ref を渡す | `VALUE_REF_GET(x)` で読む。`_ref` helper へ素通し可 |

選び方の規則:

> **`VALUE` にできるのは「自分より後に GC しうる評価が無い」operand**
> = 原則として**最後の @child** (かつ body が最初の GC point までに使い切るもの)。
> それ以外は `VALUE_REF`。

途中の @child が `VALUE` でいられないのは、後続 sibling の評価が GC した
ときに register 上の値が stale になるから。staging (= slot への store) が
そのまま rooting になる、というのがこの機構の核心。

なお「runtime で tag を見て fixnum なら staging を省く」はやらない —
**分岐 1 個は無条件 store (mov 1 本) より高い**。静的に分かる場合の省略は
§7.7 (specialize 時) で扱う。

### 7.3 生成される glue (staging = rooting) ✅

`node_plus(… VALUE_REF lhs@child, VALUE rhs@child …)` (slot_count = 1) なら:

```c
RESULT
DISPATCH_node_plus(CTX *c, NODE *n, VALUE *slots)
{
    slots += 1;                              /* 自分の slot area (lhs 用) を確保 */
    RESULT r0 = EVAL_ARG(c, n->u.node_plus.lhs, slots);
    if (UNLIKELY(r0.state != KORB_NORMAL)) return r0;
    slots[-1] = r0.value;                    /* staging = rooting */
    RESULT r1 = EVAL_ARG(c, n->u.node_plus.rhs, slots);
    /* ↑ rhs 評価中、lhs は slots[-1] に居るので GC されても fixup される。
       v1 の偽 frame parking はこの構造で消滅する */
    if (UNLIKELY(r1.state != KORB_NORMAL)) return r1;
    return EVAL_node_plus(c, n, slots,
                          VALUE_REF_AT(&slots[-1]),  /* VALUE_REF @child */
                          r1.value,                 /* VALUE @child (register) */
                          n->u.node_plus.arg_index);
}
```

- 親から受けた slots に対し「自分の slot area 分 advance してから使う」
  (= v1 iter 60 の child-self-advance 規約と同じ)。advance 量 slot_count は
  @child (VALUE_REF のもの) + `$tmp` の数で、NodeKind に静的に bake される
- RESULT の例外伝播も glue で完結 (例外なら body に入らない)
- SD を焼くときは specializer が同じ形を子 SD 直呼びで吐く

### 7.4 例 ✅

**lset (`a = 1`) — 全部 register、staging ゼロ:**

```c
NODE_DEF
node_lset(CTX *c, NODE *n, VALUE *slots, VALUE rval@child, uint32_t lvar_off)
{
    slots[lvar_off] = rval;     /* 唯一の child + body は GC しない (@nogc) */
    return RESULT_OK(rval);
}
```

唯一の @child なので VALUE でよく、staging slot 自体が無い。v1 で
「node_lset は @child 化できない」とされた lset destination と staging
slot の aliasing 問題も、slot が消えるので一緒に消える。
SD では rval の評価が定数に畳まれ `slots[off] = INT2FIX(1);` 1 命令になる。

**plus — 混合 (途中 = VALUE_REF、最後 = VALUE):**

```c
NODE_DEF
node_plus(CTX *c, NODE *n, VALUE *slots,
          VALUE_REF lhs@child, VALUE rhs@child, uint32_t arg_index)
{
    VALUE l = VALUE_REF_GET(lhs);
    if (LIKELY(FIXNUM_P(l) && FIXNUM_P(rhs) && !BASIC_OP_REDEFINED(c))) {
        long s;
        if (LIKELY(!__builtin_add_overflow(FIX2LONG(l), FIX2LONG(rhs), &s) && FIXABLE(s)))
            return RESULT_OK(INT2FIX(s));
        return korb_fix_plus(c, slots, l, rhs);          /* 引数 immediate → 値渡し */
    }
    return korb_node_plus_slow(c, slots, n, l, rhs, arg_index);
    /* slow は値渡し wrapper。中で push して _ref 版へ (§4.5) */
}
```

**array literal — 可変長 @children は VALUE_SLICE:** 🤔

```c
NODE_DEF
node_ary_new(CTX *c, NODE *n, VALUE *slots, VALUE_SLICE elems@children)
{
    return korb_ary_new_from_slice(c, slots, elems);
    /* glue が要素を slots の下に順次評価・staging 済み (後続評価中も全部 rooted)。
       staged 列がそのまま constructor の引数ベクタになる */
}
```

**制御構造 — lazy operand は NODE * のまま:**

```c
NODE_DEF
node_and(CTX *c, NODE *n, VALUE *slots, VALUE lhs@child, NODE *rhs)
{
    if (!KORB_TRUTHY(lhs)) return RESULT_OK(lhs);
    return EVAL_ARG(c, rhs, slots);          /* 右辺は条件成立時のみ評価 */
}
```

@child は strict 意味論専用。`&&` の右辺・`if` の枝・`while` の body のように
評価を遅らせたい operand は `NODE *` のまま body 内で `EVAL_ARG` する。

### 7.5 body 内の規律 ✅

- `VALUE` local (VALUE_REF_GET の結果含む) を **may_gc call を跨いで使わない**。
  跨ぐなら `VALUE_REF_GET` で取り直す (slot は fixup 済み) — CodeQL の検査対象
- callee へ渡す slots は、push していなければ受け取った `slots` そのまま
- `$name` で一時 slot を宣言できる (slots 負 offset に置換され slot_count に
  算入される。v1 iter 60 の機構を継承)

### 7.6 may_gc 宣言 ✅

NODE_DEF 行 option に書く (既存の `@canonical=` / `@noinline` と同じ置き場):

- デフォルト = may_gc true (保守的・安全側)
- `NODE_DEF @nogc` で opt-out (node_lvar, node_lset, 比較系など)
- 部分木レベルは specialize 時に kind レベルから畳むだけなので宣言不要

### 7.7 specialize 時の staging 省略 (将来) 🤔

SD を焼く時点では operand の実部分木が見えるので、静的に判定できる:

1. **後続 sibling の部分木が may_gc でない** (`x + 1` の rhs は literal)
2. **その child の結果が immediate 確定** (`1 + f()` の lhs)

どちらかなら staging を省略して register 渡しにできる。実現機構は
**multi-variant 転写**: body は不透明テキストなので、ASTroGen が binding を
変えた同じ body を複数 variant (`lhs = slot 経由` / `lhs = register`) として
転写し、specializer が判定して選ぶ。`VALUE_REF_GET` を `_Generic` で
「VALUE_REF なら deref、VALUE なら identity」にしておけば同一 body が両 variant
でコンパイルできる。「C パーサを持たない」原則は崩れない。

ただし body が ref を `_ref` helper に素通ししていると register variant は
作れない (slot が実在しない) ので、variant 可否は宣言で示す必要がある
(構文 ❓)。**v2 初期はこの機構なし** (正しさ優先、staging は store 1 本で安い)。
§8 の R1 対策として効くので、計測してから入れる。

### 7.8 lvar と frame ❓ (M0 spike で決定)

🤔 推奨は「**一本**」= stack-machine bake:

- parser が各プログラム点の staging 深さ depth を計算し、lvar を
  「動く slots からの負 offset」(`index - locals_cnt - depth`) で bake
- lvar / @child / $tmp が全部同じ「top の下の負 offset」になり、
  GC scan も `[c->slots, slots_top)` 一本で閉じる
- block body の outer lvar は従来どおり `blk->env` ポインタ経由 (別系統)

fallback は「**二本**」= lvar だけ frame base 経由 (frame struct に fp を持つ)。
一本の懸念は §8 R2 (depth 焼き込みで SD 共有が減る)。M0 で両方測って決める。

---

## 8. 部分評価 (AOT) との整合

### 8.1 機構としては回る ✅

specializer は「定型の写しを吐く fprintf の列」のまま。v2 で SD に増えるのは
staging store / cursor 算術 (specialize 時に定数) / @children の unroll
(arity 確定) / RESULT check で、すべて機械的に emit できる。RESULT/sp ABI の
SD は v1 で完動実績あり (f4d37d54)。

むしろ v1 の偽 frame parking や c->sp_top への global store という
「部分評価が消せない memory traffic」が存在しなくなるぶん、SD は構造的に
きれいになる。

### 8.2 質のリスク 2 つ (計測で埋める) ❓

- **R1: staged slot の往復が定数畳み込みを弱める**。v1 は child の結果が
  C local で EVAL に流れたので literal が SD 内で完全に畳まれた。v2 の
  `VALUE_REF @child` は store → (call を挟んで) load になり、call があると
  compiler は load を fold できない。heap 値はどうせ fold 禁止 (move される)
  なので損は無いが、immediate は畳めなくなる。
  → 対策: `VALUE @child` (最 hot ケースを覆う) + §7.7 の variant 選択
- **R2: depth 焼き込みで SD の共有が減る** (一本モデルの隠れコスト)。
  lvar offset に depth が入ると、同一部分木でも出現深さが違えば別 hash →
  SD dedup が効かず code store とコンパイル時間が膨らみ得る。v1 は
  sp = frame top 固定だったので offset が depth 非依存だった。
  → 一本 vs 二本 (§7.8) の判断材料として M0 で計測

### 8.3 AOT は M0 から CI gate に入れる ✅

v1 は AOT を後回しにした結果、dispatcher swap が hash 不一致で静かに無効化
して測定不能になった。v2 は最初から rubyharness の aot+compile / aot+cached
モードを常時 green に保つ。`@canonical=` (runtime promotion と SD hash の
整合機構) も最初から維持する。

---

## 9. GC 戦略

### 9.1 最初から moving で動かす ✅

コードは §2-5 の規約 (= full moving で正しい形) で書き、**default backend
も初日から copy (moving)**。GC 強度の段階導入はしない:

- non-moving では rooting 漏れが**発火しない** (root し忘れても誰も move
  しない)。後から moving に上げると違反が一斉に出る — v1 の struct-moving
  移行が頓挫したパターンの再演になる
- 検出器 (copy + STRESS + PURGE) は node が 1 個の時点から回す。
  gc_copy backend は runtime/precise_gc に存在し稼働中 (baruby_precise /
  ascheme_precise) なので待つ理由がない
- マイルストーンは GC 強度ではなく**言語スコープ**で刻む (v2_spec.md §6)

non-moving backend (mark 等) は比較・デバッグ用の build-time switch
(`make GC=<backend>`、baruby_precise の testbed と同じ流儀) として残す。

### 9.2 検証モード ✅

- **STRESS** (`ASTRO_GC_STRESS=1`): 毎 alloc collect。rooting 漏れの炙り出し
- **PURGE**: mprotect ベースの plane 巡回。stale ポインタの deref が
  **決定的に SEGV** する = 「GC 後に古い値を触った」の実行時検出器

### 9.3 オブジェクトの区分 — Class も moving、immortal は最小限 ✅

**Class / Module も含めて、オブジェクトは moving heap で一様に扱う**
(user 決定。GC に型別の特別経路を作らない)。move しないのは実行基盤
そのものだけ:

| 区分 | move | 回収 | 入るもの |
|---|---|---|---|
| moving heap | する | する | 普通のオブジェクト + **Class / Module** |
| immortal | しない | しない | NODE (AST)、frozen literal |

Class が動くことで引き受けるもの (いずれも対処が既知):

1. **inline cache の klass 同一性** — アドレス reuse で誤判定し得る
   (v1 で実証)。対処は ❓ M1 で選択:
   (a) **gen tag** (v1 実証済み: gen field を visit_roots で bump、
   キャッシュ判定に gen 比較を足す)
   (b) **IC slot を GC edge として fixup** (cache 内の klass ポインタも
   move に追従するので比較が常に正しい。代償: IC が class を延命 +
   IC 列挙の scan コスト)
2. **AOT への klass 定数焼き込みは不可** — `c->consts[idx]` (root 配列)
   経由の間接参照で扱う。元々 v1 の VALUE 埋め込みも同じ間接形なので
   (§13 #5)、新たな後退ではない
3. **class の表 (method table / const table / ivar) への VALUE_REF は不可**
   — movable object の field なので §2.3 のとおり。copy + writeback
   (§5.1) で書く。結果として「ref を向けてよいのは実質 slots だけ」に
   規則が狭まり、検査も単純になる

NODE は SD がアドレス参照する + 捨てられる頻度が低いので immortal。
メソッド再定義の churn が問題になったら collectable 化を検討する。

### 9.4 libc malloc の禁止 ✅

GC が見るオブジェクト・バッファは全部 korb_alloc (arena) 経由。v1 の
「container だけ xmalloc」混在は struct-moving 移行を頓挫させた直接原因
なので、v2 では最初から混ぜない。

---

## 10. 検査 — 規約を機械で守らせる

3 層 + 静的解析で「規約が守られているか心配する」状態をなくす。

| 層 | 何が捕まるか | コスト |
|---|---|---|
| **型 (VALUE_REF/SLICE)** | 生 deref・ポインタ演算の事故。コンパイル時 | ゼロ (release は素の deref に展開) |
| **audit build** (`-DKORB_AUDIT`) | VALUE_REF_GET/SET 時の stable-root 判定 + stale 値検査 (gc_clock。v1 KORB_RESULT_AUDIT の後継)、VALUE_SLICE の bounds | debug build のみ |
| **STRESS / PURGE** | すり抜けた時間差 stale を実行時に決定的 SEGV | テスト時のみ |
| **CodeQL** (audit build の compile DB 上で) | ①may_gc call を跨ぐ VALUE local の使用 ②`c->slots_top` への korb_alloc 系以外の代入 ③`VALUE_REF_AT` への不正な出所 (`&local`, `&obj->field`) | CI |

🤔 CodeQL クエリは make ターゲット (`make codeql`) として整備し、PR/iter の
gate にする。may_gc(C 関数) の根拠は命名規約 + annotation で与え、
call graph との矛盾も CodeQL で検出する。

---

## 11. 検証基盤とゲート ✅

検証契約は **rubyharness** (整備済み):

```sh
make gen                 # コーパス生成 (CRuby オラクル: method ~87k + syntax + spec mining)
make test [CAT=area]     # 差分テスト (1 ファイル = 1 プロセス、crash recovery)
make test STRESS=1       # GC ストレス下
make bench               # cruby / cruby+yjit / interp / aot+compile / aot+cached
```

ゲート:

- 各マイルストーンで `make test` (+ STRESS) green を維持
- AOT モード常時 green (§8.3)
- 性能は v1 koruby_precise (凍結) と CRuby を基準線に bench で追跡
  (数値目標は計測が始まってから設定する)

---

## 12. 進め方

### 12.1 前提整備 — 完了 (2026-06-12) ✅

- astrogen.rb の sample ABI 漏れを hook 化 (`child_dispatch_args` /
  `child_storage_*` / `slot_area_prologue`)。base は言語中立に
- runtime/precise_gc の koruby 専用診断コードを撤去、ログ接頭辞を
  `aro_gc=` に中立化
- **原則として明文化**: framework (lib/, runtime/) に sample 固有の
  ABI・CTX field 参照・デバッグコードを持ち込まない。sample 文脈が要る
  ものは hook (override / weak) で sample 側に置く

### 12.2 マイルストーン 🤔

GC は全段階で copy (moving) + STRESS/PURGE gate (§9.1)。刻みは言語スコープ:

| | 内容 | 出口条件 |
|---|---|---|
| M0 | spike: コア eval loop + lvar 一本/二本の実測比較 (§7.8, §8.2) + AOT 経路疎通 | 設計判断の確定。calc 級 subset が rubyharness CAT=basic green (STRESS+PURGE 含む) |
| M1 | コレクション / block / class / 例外 / builtins 拡大 | 主要 CAT green (STRESS+PURGE 含む)、bench で v1 (git 履歴の基準値) / CRuby と比較開始 |
| M2+ | スコープ拡大 (Module / 特異メソッド / …) | 同 gate 維持。最終目標は rubyspec 広範 PASS + optcarrot (v1 同等以上) |

builtins の C コードは v1 から**機械移植しない** (互換性不要の方針)。
v1 はロジックの参考資料とし、v2 ABI で書き下ろす。

### 12.3 場所 ✅

`sample/koruby_precise/` に in-place で再構築 (user 決定。v1 実装は
2026-06-12 に削除済み、基準線は git 履歴で参照)。CLI 等の外形仕様は
[v2_spec.md](./v2_spec.md)。

---

## 13. 未決事項まとめ ❓

| # | 論点 | 決め方 |
|---|---|---|
| 1 | lvar 一本 (stack-machine bake) vs 二本 (frame base) | M0 で実測 (R2 の SD dedup 影響込み) |
| 2 | slots overflow の細部 (§3.5 の方針は確定。limit check の置き場所 = prologue のみで足りるか、可変長 staging 前の SLOTS_RESERVE が要るか) | M0 |
| 3 | specialize 時 variant 選択の宣言構文 (§7.7) | 機構導入時 (初期は無し) |
| 4 | VALUE_SLICE の詳細 (splat / kwargs との接続) | M1 で builtins を書きながら |
| 5 | AOT の VALUE literal 埋め込み (v1 からの framework gap: c->consts 経由) | AOT gate 整備時 |
| 6 | callback 系 iterator (each 等) と ref 規約の詳細 | M1 |
| 7 | ~~ディレクトリ名~~ → `sample/koruby_precise/` in-place で確定 (§12.3) | 解決済み |
| 8 | 可変長の「上に返す」(splat 展開等、§3.4)。候補: (a) cursor を `VALUE **` で渡す例外 ABI (b) Array で返す (c) 生成コード側 (cursor 所有者) に寄せる | M1 (splat 実装時) |
| 9 | moving Class への inline cache 防衛 (§9.3): gen tag (v1 実証) vs IC slot を GC edge 化 | M1 (IC 実装時) |
