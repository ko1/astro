# koruby_precise — C Extension API 設計 (Track B: native)

> Status: **draft / 設計のみ**。実装は未着手。
> 関連: [`v2_design.md`](v2_design.md) §4 (RESULT), §5 (VALUE_REF/slots ABI),
> [`rooting_guide.md`](rooting_guide.md), `context.h`, `node.h`, `builtins/*.c`。

## 0. 目的と A/B の切り分け

koruby を CRuby の drop-in replacement にするとき、性能で負けている領域
(`json-parse` ~22×, `protoboeuf`/`ruby-json` ~2.7×) はいずれも **CRuby が C
拡張 / C 実装プリミティブ (`json` gem, `Array#pack`, `StringScanner`, `Regexp`)
でホットループをネイティブ実行している**ことが原因。AST を焼く AOT はメソッド
ディスパッチの特殊化なので、ホットループが C の中にある領域には効く場所がない。

これを埋めるには C 拡張の口が要る。方向は 2 つあり、**両方要る**:

| track | 何ができる | 目的 | 本ドキュメント |
|---|---|---|---|
| **A. CRuby ABI 互換** | 既存 gem の C ソース (`ruby.h`) が**そのまま**動く | 互換・エコシステムの広さ | 別途 (§6 に方針だけ) |
| **B. koruby ネイティブ API** | koruby 内部に合わせた C 拡張が**速い** | 性能・第一級プリミティブ | **これ** |

まず **B** から作る。B は A の shim が乗る土台にもなる (§6)。

## 1. 支配的制約 — moving / copy GC

koruby_precise は **オブジェクトが動く** GC (copy + PURGE plane 巡回)。CRuby の
`ruby.h` が成立するのは CRuby がオブジェクトを **pin している**から:
`RSTRING_PTR` は安定ポインタを返し、`VALUE` を C ローカルに握ったまま
`rb_funcall` してよい。koruby ではこれが**全部罠**になる:

- **生 VALUE を GC point を跨いで握ると stale** (`v2_design.md` §5 の中心規約)。
- **heap payload への生 `char*` / `VALUE*` は alloc を跨ぐと stale**。
  `KorbString` は本体と別に `KorbStrBuf *buf` を alloc しており (`context.h`)、
  `<<` / `[]=` の in-place 成長で buf 自体が動く。`string.c:1718` が
  「a raw pointer into the String's buffer would go stale under moving GC」と明記。
- 既知バグの記録 (memory): captured_self / str byte ptr / pack pointer / ary_mul
  は全部この「動くものを握った」系。

> **設計原理**: だから B の API は「動くポインタを握ってしまう事故」を
> **規律ではなく形状 (型) で防ぐ**。これが B の本質的な仕事であって、
> 「API を発明する」ことではない。

## 2. 既にあるもの (the 80%)

現状の builtin 規約は、実質 mruby 型の GC-safe C-ext ABI の骨格そのもの:

```c
/* context.h:607 — 受信者ディスパッチ builtin の型 */
typedef RESULT (*korb_method_fn)(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE args);
typedef RESULT (*korb_method_blk_fn)(CTX *c, VALUE *slots, VALUE_REF self, /* +block */ ...);
```

| 要素 | 実体 | CRuby/mruby 対応 |
|---|---|---|
| コンテキスト | `CTX *c` (グローバル禁止ルールの帰結) | `mrb_state *` |
| rooted スクラッチ | `VALUE *slots` = GC がスキャンする第一空きスロット。GC は `[c->slots, c->slots_top)` を走査 | mruby arena |
| 値を rooting | `SLOTS_PUSH(slots, v)` → cell に置き cursor を進め `VALUE_REF` を返す | `mrb_gc_protect` |
| ハンドル | `VALUE_REF` (rooted 1 cell) / `VALUE_SLICE` (rooted run)。生 `VALUE*` は ABI から排除、ref は stable root からのみ構築 | handle |
| deref | `VALUE_REF_GET(r)` は**毎回 `*cell` を読み直す** → GC が動かしても追従 | — |
| 制御/例外 | `RESULT{value,state}` + `UNWRAP`/`CHECK` 伝播 + `korb_raise`。`c->state` なし | `mrb_value` + longjmp |
| 登録 | `korb_def_cmethod(c, KORB_C_*, name, fn, arity)` / `_blk` | `mrb_define_method` |
| 生成 | `korb_str_new` / `korb_ary_new`+`push_val` / `korb_hash_new`+`set` — **全部 `(c, slots, ...) → RESULT`** (alloc は明示的、返りで再 rooting) | `mrb_str_new` 等 |
| audit | `ARO_GC_EDGE` (edge 監査) + `-DKORB_RESULT_AUDIT` (held-across-GC 検出) + `ASTRO_REF_CHECK` (stale deref hook) | — |

`enum korb_class`: INTEGER/STRING/SYMBOL/ARRAY/HASH/RANGE/…/REGEXP/PROC/… (context.h)。

つまり B に足りないのは:
(a) この面を**公開 API として命名・安定化**する、
(b) CRuby/mruby にある**人間工学** (引数パース・型チェック・文字列データアクセス) を足す、
(c) GC 規律を**型で強制**する、
(d) header / versioning / 文書。

## 3. 公開面 `korb_ext.h` の提案

### 3.1 state とハンドル

- `korb_state` = `CTX` の opaque alias。全 API の第 1 引数。
- ハンドル型: `KVALUE_REF` (rooted cell) / `KVALUE_SLICE` (引数列)。
- **immediate は生 VALUE で握ってよい**: Fixnum / Symbol / nil / true / false /
  Flonum は動かない (tag 表現; `context.h` §Tagged VALUE)。heap VALUE だけ ref
  が要る。この境界を API doc に明記し、`korb_immediate_p(v)` を提供。

### 3.2 引数パース — `korb_get_args`

mruby 風フォーマット文字列。heap 型は呼び出し側 slots に park して `VALUE_REF`
を返し、immediate はスカラで返す。型チェック + 係数 (`to_str`/`to_int`) + arity
エラーは RESULT-raise:

```c
/* "S o | i" = 必須 String, 必須 任意オブジェクト, 省略可 Integer */
VALUE_REF str; VALUE_REF obj; long n = 0;
CHECK(korb_get_args(c, slots, args, "So|i", &str, &obj, &n));
```

- kwargs: `:` セクションで symbol 名を並べる。trailing-Hash 規約は既に `pack` の
  `buffer:` で個別対応済み — それを一般化する。

### 3.3 文字列 / バイト列アクセス — **the crux**

長寿命の生 `char*` を**外に握らせない**。ただし **「毎回フルコピー」は最初から
選択肢に入れない**(重い)。移動 GC 下で最も効くのは **zero-copy の
offset+re-derive**。優先順は:

1. **offset + re-derive (zero-copy, ホットループの標準)**: ext は
   **rooted な `VALUE_REF` + 整数オフセット**を持ち、alloc のたびに `buf` を
   引き直す:
   ```c
   long pos = 0;
   while (pos < len) {
       const char *p = korb_str_data(VALUE_REF_GET(sref));  /* = VAL2STR(..)->buf->data; alloc 後は引き直す */
       char ch = p[pos];
       ... /* 結果オブジェクトを alloc しても、次のループで p を再取得 */
       pos++;
   }
   ```
   `buf` 引き直しは数命令。コピーもピンも不要。**既に `pack` builtin がこの
   「係数の後に再 derive」パターンを使っている**。json/StringScanner の
   スキャンループはこれ。
2. **断片 copy (libc 連携時のみ)**: 数値トークンを `strtod` に渡す等、NUL 終端
   C 文字列が要る場面で、**その断片だけ**を C スタックにコピー
   (`korb_str_bytes_copy(c, ref, off, len, dst, cap)`)。全体ではないので誤差。
3. **scoped borrow**: `korb_str_borrow(c, ref, const char **p, uint32_t *len)` —
   次の alloc/dispatch まで有効な no-GC window。offset+re-derive で足りない
   細かいケース用。audit ビルドで GC epoch を bump し跨いだら poison (§4)。
4. **pin**: Track A が「生ポインタを alloc 跨ぎで握る未改変 gem C」を要求したとき
   だけ (§6)。B の第一クライアントには不要。

> **why not copy-by-default**: JNI がフルコピーを強いられるのは ①UTF-16→UTF-8
> 変換が不可避 ②C に生ポインタを不透明に握らせ後続 alloc を跨ぐから。koruby は
> どちらも当てはまらない (String は既にバイト列; ext イディオムを自分で決められる)
> ので、offset+re-derive で zero-copy にできる。json-parse が遅い真因もコピーでなく
> 1 文字ごとの Ruby ディスパッチなので、スキャンを C に移すだけで勝てる。

構築側 (json generate / pack): `korb_str_new` + builder ハンドル
(`korb_str_cat`, 毎回 buf を derive し直すので grow-safe)。追記の累積は
amortized O(n) で、フル再コピーではない。

### 3.4 返り値 / 例外

`RESULT_OK(v)` を返す / `UNWRAP`・`CHECK` で伝播 / `korb_raise(c, slots,
KORB_E_*, line, fmt, …)`。ext コードは自然に合成できる。

### 3.5 登録 / module init

- `void Init_<name>(korb_state *c)` 規約 (CRuby の `Init_` を踏襲)。
- クラス定義 API: `korb_define_class` / `korb_def_module_function` を追加
  (現状 `korb_def_cmethod` は既存コアクラス向け)。
- 現状は静的リンク builtin なので `Init_` は runtime init から呼ぶ。
  **dynamic-load (dlopen した .so ext)** は後続フェーズで、code_store /
  require-AOT 機構に接続する (§7)。

## 4. GC 安全性を「形状」で強制する (本質的な設計作業)

- **規約**: alloc しうる関数は全て `RESULT` を返し `slots` を取る。ext は自分の
  `VALUE_REF` を常に「読み直せる (GC が動かしたかも)」ものとして扱う。
  `VALUE_REF_GET` が毎回 `*cell` を読むので、**ref を握る限り自動で安全**。
  raw VALUE を握った瞬間だけ壊れる。
- **公開 header から raw `VALUE*` を禁止**。ハンドル (`KVALUE_REF`/`KVALUE_SLICE`)
  のみ。heap 値を「握るために」bare VALUE で返す API を提供しない。heap 結果は
  必ず呼び出し側 slots に park された ref で返す。
- **borrow したバイトポインタの寿命** = 「次に alloc しうる API 呼び出しまで」。
  debug poison: GC epoch を bump、borrow は epoch を記録、deref で不一致なら abort。
- 既存ツールに乗る: `ARO_GC_EDGE` (struct edge) / `KORB_RESULT_AUDIT`
  (held-across-GC) / `ASTRO_REF_CHECK` (stale deref) + ext TU で `char*`/`VALUE*`
  escape を禁じる grep/CodeQL ゲート。

### 4.1 関数 GC-effect 属性 + 静的 borrow 検査(`@nogc` の C 関数への一般化)

**現状**: GC-point を機械が知る箇所は無い。頼りは `rooting_guide.md §2` の手書き
リスト + プログラマ規律 + **実行時** `-DKORB_RESULT_AUDIT` のみ。`node.def` の
`NODE_DEF @nogc` marker はどの生成器/検査器も消費しておらず(出現は node.def と
生成後コメント 2 行 = 未配線)、`astro_ref_template.h` の "CodeQL layer" も未実装。

これを **C 関数レベルの GC-effect 属性 + CodeQL 静的検査**に一般化する。GC-safety の
本体は builtins/*.c・korb_runtime.c・ext の**素の C 関数**で node ではないので、属性は
関数に置き、node の `@nogc` は関数 effect から**導出**する(single source of truth;
codegen の staging 省略と安全検査を同じ effect で兼ねる)。B の前提ではない(B は現行
規律 + `RESULT_AUDIT` で着手可)が、公開 ext API を「形状で安全」にする決定打で、
§3.3/§8 の実行時 epoch-poison をこの静的検査で置換する。

#### 属性(共通ランタイム)

koruby 固有でなく **ARO 層(全サンプル共通)の機能** — `ARO_GC_EDGE`/`AROH_*`/
`ASTRO_REF_*` と同族なので `runtime/` に置く。

```c
// runtime/aro_gc_effect.h  (全サンプル共有)
#if defined(__clang__)
#  define ARO_NOGC   __attribute__((annotate("aro_nogc")))
#  define ARO_MAYGC  __attribute__((annotate("aro_maygc")))
#else
#  define ARO_NOGC          /* gcc は annotate 非対応 → 空マクロ(警告ゼロ) */
#  define ARO_MAYGC
#endif
```

**gcc の罠**: 既定は gcc(`CC ?= gcc`)で annotate 非対応 → 素で書くと「attribute
directive ignored」警告(無視禁止ルール違反)。回避(併用可):(1) マクロは空 +
CodeQL は宣言に付いた `ARO_NOGC` の **`MacroInvocation`** を位置照合で拾う(gcc の
まま警告ゼロ、本命)、(2) CodeQL 抽出ビルドだけ clang で回して `annotate` を活かす。

#### may-gc 推論 — 直接呼び出しは recursion で健全かつ完全

- **seed は 1 つ**: 「ARO の allocator が GC を起こす」。そこから**直接コールグラフの
  推移閉包**で全関数を分類(may-gc を 1 つでも呼べば may-gc、でなければ no-gc)。
- **循環・相互再帰は least-fixpoint で解ける**(CodeQL 再帰 predicate)。→ **直接
  呼び出しに「推論できない」ケースは無い**。手貼りは不要(pure accessor も推論で
  no-gc)。`ARO_NOGC` は要所の「no-gc のはず」を検証する契約宣言として使う。
- 詰まるのは inference の限界でなく**コールグラフの辺が見えない 2 箇所**だけ:

**(a) 間接呼び出し(関数ポインタ)→ effect を funcptr の型に持たせる**
C++ の `noexcept` funcptr と同型。funcptr 型に GC-effect を付け、検査を**代入/受け渡し
サイト**へ移す:
- 既定 funcptr = may-gc(dispatch `korb_method_fn` 等)。`nogc` funcptr 型を別途宣言。
- **may-gc 関数を nogc funcptr に代入/引き渡し → error**(throwing 関数を noexcept
  funcptr に入れられないのと同じ)。
- **nogc funcptr 経由の呼び出しは健全に no-gc**(no-gc しか入らないと検証済み)。
- → 「間接呼び出しは一律保守 may-gc」が「**宣言による精密判定**」に。comparator/
  iterator/hash 等の callback は nogc 型へ。精度を上げるなら候補を address-taken・
  型互換関数に絞る。

**(b) body 無し外部関数 → no-gc(CTX-threading で正当化)**
koruby の GC は allocator 経由でしか起きず allocator は `CTX *c` を要求する。body 無し
extern は `c` を持たない → koruby オブジェクトを alloc できない → **GC できない**。
「たどれない ⇒ no-gc」は実務近似でなく CTX 規律から**健全**(`strcmp`/`memcmp`/
`strtod`/libc)。
- 例外「extern が koruby callback を中で呼ぶ」(qsort + alloc する comparator)は
  callback = funcptr なので **(a) の管轄**: nogc 型 param なら alloc callback で error、
  素の C 型なら「may-gc funcptr を渡す呼び出しは may-gc」で近似。
- **健全性は strict no-globals ([[feedback_no_globals_strict]]) に依存**: global
  `current_ctx` fallback があれば extern が `c` 無しで GC でき (b) が崩れる。CTX を必ず
  引数で通す規律がこの解析の土台。

#### borrow-source は有限の閉集合 + immortal 除外

移動ヒープから**生の内部ポインタ**が出る payload は 2 種だけ:
- **文字列バイト** `KorbStrBuf.data`(`char data[]`)を `KorbString.buf->data` 経由。
- **VALUE 配列要素** `KORB_OBJ_VALUE_ARRAY` の `items->data`(KorbArray/KorbHash の裏)。

他フィールドは全て `VALUE ARO_GC_EDGE`(個別 VALUE = VALUE_REF/edge 機構が管理、生
ポインタでない)。新型を足しても増えるのは `ARO_GC_EDGE` VALUE で新規 raw buffer では
ない(flexible-array payload 追加は意図的でレビュー対象)。→ CodeQL は関数リストでなく
**構造マッチ**(上記 2 payload への到達)で網羅でき、`ARO_GC_EDGE` マーカーを再利用可。
公開 ext API ではさらに小さく、公開アクセサ数個(`korb_str_data` 等)。

**immortal は除外**: immortal = libc(calloc/mmap)確保で不動・GC 管理外(AST(NODE)/
method entry/interned symbol 名/slots/def_env)。immortal borrow は GC 跨ぎで安定なので
borrow-source から外す(`korb_sym_name`=stable, `korb_str_data`=movable)。別確保クラス
なので構造上区別でき false positive なし。

**borrow-source の与え方 — 構造マッチ vs `ARO_BORROW` 注釈**:
- **現行 inline コード**(`VAL2STR(v)->buf->data` のようにマクロ/フィールドで取り出す)
  は貼る関数が無いので**構造マッチ**で拾う(実装済: leaf `data` + ポインタ演算 +
  `&elem` + local alias)。ただし interprocedural(helper 戻り値)/構造体フィールド
  格納/整数ロンダリング/中間 movable 構造体ポインタ(`s->buf`/`ary->items` 自体の
  保持)は**原理的に構造 local マッチでは網羅できない**(既知の非カバー)。
- **ext API(将来)= `ARO_BORROW` 注釈**(推奨・単純・完全): 「戻り値が GC
  オブジェクトの中/自身を指す」アクセサ関数(`korb_str_data` / `korb_str_borrow` /
  `korb_ary_ptr` 等)に `ARO_BORROW` を貼り、borrow-source =「その関数呼び出しの
  戻り」1 つに集約。抽出を**アクセサ 1 チョークポイントに通す**ことで、上記の
  構造マッチの穴(cast/alias/interprocedural)を丸ごと回避できる。`ARO_NOGC`/
  `ARO_MAYGC` と同じ `runtime/aro_gc_effect.h` に置き、CodeQL は MacroInvocation で
  拾う(gcc ビルドを壊さない)。
- 結論: 重い「GC 構造体ポインタ一般への構造マッチ拡張」は複雑な割に不完全なので
  採らない。**現行 inline は構造マッチ(4 形)で据え置き、抽出を関数化する ext API
  では `ARO_BORROW` に一本化**する。

**spatial は compiler、temporal は CodeQL(役割分担)**:
> **STATUS (2026-08-07): 完遂**。payload フィールドを `data_priv` に改名し、全
> **1424 箇所**の直接アクセスを `korb_strbuf_data`/`korb_items_data`/`korb_str_data`
> (ARO_BORROW inline)経由に変換済み(commit a1ec4566)。以後は**コンパイラが
> spatial encapsulation を hard 強制**(bypass = コンパイルエラー)。検証: build
> 0/0・make test 99808/1890 crash 0(baseline 一致)・AOT optcarrot 60838 / DOOM
> 一致・STRESS crash 0・codeql interior-encapsulation 0。`interior_encapsulation.ql`
> は data/data_priv 両対応で baseline 0 の belt-and-suspenders として残置。

- **spatial(誰が生フィールドに触れるか)= C の field-rename + `ARO_BORROW` inline
  アクセサが最強**。payload フィールドを改名(または nested struct 化)すれば、
  直接アクセスは**全部コンパイルエラー**になり、コンパイラが箇所を列挙する。
  実測(2026-08-06): `KorbStrBuf.data`/`KorbArrayItems.data` を改名 → gcc が
  **1427 error**(= CodeQL の 1429 とほぼ一致、独立クロス検証)。アクセサ化後は
  bypass すると**コンパイルが通らない** = CodeQL ratchet より強い hard 強制。
- **temporal(borrow を may-gc 跨ぎで保持)= CodeQL `borrow_after_gc.ql` 専任**。
  「GC が borrow と use の間で起きるか」はコンパイラには判定できない。
- したがって `interior_encapsulation.ql`(spatial)は **field-rename 移行までの
  暫定 ratchet**。rename+アクセサが入れば spatial はコンパイラが保証し、この
  CodeQL クエリは冗長になる。GC-safety の本体は `borrow_after_gc.ql` の temporal 検査。

#### CodeQL クエリ 2 本(実 API)

**Q1: borrow を may-gc 跨ぎで使ったら alert**
```ql
import cpp
import semmle.code.cpp.dataflow.new.DataFlow

predicate calls(Function f, Function g) {
  exists(FunctionCall c | c.getEnclosingFunction() = f and c.getTarget() = g)
}
predicate mayGcFunction(Function f) {
  f.hasName("aro_alloc")                                     // 唯一の seed
  or exists(Function g | mayGcFunction(g) and calls(f, g))  // 直接辺を fixpoint 伝播
}
predicate mayGcCall(Call c) {
  mayGcFunction(c.getTarget())
  or not exists(c.getTarget())        // 間接呼び出し = (a) 未整備なら保守的 may-gc
}
class BorrowCall extends FunctionCall {
  BorrowCall() { this.getTarget().hasName(["korb_str_data", "korb_ary_ptr"]) }
}
from BorrowCall borrow, DataFlow::Node use, Call gc
where
  DataFlow::localFlow(DataFlow::exprNode(borrow), use) and   // borrow 値が use に届く
  mayGcCall(gc) and
  borrow.getASuccessor+() = gc and gc.getASuccessor+() = use.asExpr()  // borrow<gc<use (CFG)
select use, "borrowed ptr (from $@) used after may-GC call $@ — stale under moving GC",
  borrow, borrow.getTarget().getName(), gc, "GC point"
```
`use` = dataflow の sink(borrow の到達先)。**re-derive で自動安全**: 途中で
`ptr = ref(obj)` し直すと flow が届かなくなる。`strcmp(ptr)` 等は「may-gc を跨がない
use」なので弾かれない(= 許可)。

**Q2: escape = 長寿命格納 / return のみ**(関数への引き渡しは対象外 = Q1 の管轄)
```ql
from BorrowCall borrow, DataFlow::Node esc
where
  DataFlow::localFlow(DataFlow::exprNode(borrow), esc) and
  ( esc.asExpr() = any(ReturnStmt r).getExpr()                    // return
    or exists(AssignExpr a | a.getRValue() = esc.asExpr() and     // 長寿命メモリへ格納
         (a.getLValue() instanceof FieldAccess
          or a.getLValue().(VariableAccess).getTarget() instanceof GlobalOrNamespaceVariable)) )
select esc, "borrowed ptr が呼び出しを跨いで生存 — 禁止(offset+re-derive か copy)"
```
`strcmp(ptr, x)` は引数渡しなので escape でなく、Q1 でも may-gc を跨がなければ安全 →
正しく許可。残る「渡した先が溜め込む」(`stash(ptr)` が `g = ptr`)は、同じ格納ルールを
全関数のポインタ引数へ適用して閉じる(ext は scoped API 経由でしか interior ptr を得られ
ないので任意 stash 不可)。

#### 前例

C++ `noexcept` funcptr(効果型)、V8 `DisallowGarbageCollection` scope(RAII・実行時)、
Linux sparse `__attribute__((context(...)))`(静的・ロック文脈)。koruby は「静的属性 ×
CodeQL × effect-typed funcptr」の組。

## 5. 最初のクライアント — native `json` (と pack / StringScanner)

面の妥当性検証を兼ねる。json パーサが要るのは:高速な byte borrow (scoped) /
string builder / array・hash 構築 / Float・Integer パース — 全て §3 にある。

- **目標**: `json-parse` の 22× ギャップを詰める。
- **成功条件**: `lib/json.rb` のパーサを `korb_ext.h` 経由の C ext で再実装し、
  raw-pointer-across-GC ゼロ、checksum 一致、かつ計測で有意に高速。

## 6. Track A (互換) との関係

- A = **B の上に `ruby.h` の subset を実装した shim**。未改変 gem の C に escape
  する VALUE を pin する戦略が要る: (i) 呼び出し中だけ ext のスタック領域を
  conservative pin、または (ii) JNI 的な per-call local-ref テーブル。
- A の性能天井は B より低い (pin/handle オーバヘッド) が、エコシステムの広さを
  買う。B のハンドルモデルが自然な土台: `ruby.h` の `VALUE` ↔ koruby `VALUE_REF`
  を per-call handle テーブルで対応づける。
- **決定**: B を先に作り、B のハンドル寿命規則が固まってから A の pin 層を設計。

## 7. Open questions

- dynamic `.so` ext ロード vs 静的 builtin (code_store / require-AOT と接続)。
- 文字列を C に渡すとき **pin モード** (pinned-string) を持つか、copy/borrow 強制か
  — A の性能トレードオフ。
- versioning / ABI 安定性ポリシー (koruby はまだ内部 ABI が動く段階)。

## 8. 実装フェーズ案

1. `korb_ext.h` を切り出し (§2 の既存型を公開名で re-export、raw `VALUE*` を隠す)。
2. `korb_get_args` (§3.2) + string borrow/copy/builder (§3.3)。
3. epoch-poison による borrow-lifetime audit (§4)。
4. native `json` ext を第一クライアントとして実装・計測 (§5)。
5. `korb_define_class` / `Init_` 規約 + 静的登録の整理 (§3.5)。
6. (後続) dynamic load、次いで Track A の pin 層。

---

## Appendix A. 主要処理系の C-ext 引数処理サーベイ (2026-08-05)

`korb_get_args` の形 (#3) と borrow/pin (#1) を決めるための他処理系比較。

| 処理系 | GC モデル | 引数パース | 文字列→C |
|---|---|---|---|
| **CRuby (MRI)** | 非移動 (保守的スタック走査; `GC.compact` は pin 対応 opt-in) | `rb_scan_args(argc,argv,"1*:&",…)` 個数+`*`/`:`/`&` フォーマット + `rb_get_kwargs`; `Check_Type`/`StringValue`/`NUM2INT` | `RSTRING_PTR` = 安定ポインタ (動かない) |
| **mruby** | **非移動** (incremental/generational M&S) + arena | `mrb_get_args(mrb,"S!o|i&:",…)` 型付きフォーマット | `s`→(char*,len) は値が生きてる間**安定**(動かない); `mrb_gc_arena_save/restore` は生存管理 |
| **CPython** | 非移動 (refcount) | `PyArg_ParseTupleAndKeywords(…,"s#|i$O!",…)` format-unit; 新規は **Argument Clinic** (ビルド時 codegen DSL) | `PyUnicode_AsUTF8` 安定 (動かない) |
| **PHP** | 非移動 (refcount+COW) | 旧 `zend_parse_parameters("sl|b",…)` フォーマット; **Fast ZPP** マクロ DSL (`Z_PARAM_STR`/`Z_PARAM_OPTIONAL`, インライン展開) | `ZSTR_VAL` 安定 |
| **Lua** | 非移動 (incremental M&S) | 明示スタック: `luaL_checkinteger`/`luaL_checklstring`/`luaL_argcheck` を引数ごと | `luaL_checklstring`→const char*、スタックに載ってる間有効 |
| **Perl (XS)** | 非移動 (refcount) | `items`+`ST(i)`; `SvIV`/`SvPV`; **typemap** codegen (xsubpp) | `SvPV`→char*+len、SV が生きてる間 |
| **Tcl** | 非移動 (refcount) | `objc/objv`+`Tcl_GetIntFromObj`/`Tcl_GetStringFromObj`; `Tcl_WrongNumArgs` | Tcl_Obj が生きてる間 |
| **V8 / Node-API** | **移動 (generational)** | `napi_get_cb_info`→argc/argv handle; `napi_get_value_*` を引数ごと | `napi_get_value_string_utf8` は **常に copy** (2-call: 長さ→充填) |
| **JNI (JVM)** | **移動 (compacting)** | Java 型シグネチャ (宣言的) | `GetStringUTFChars`(copy-or-pin, `isCopy` で判別) **+** `GetStringCritical`(scoped pin, GC-locker, critical-region 制約: 間に他 JNI 呼び出し/blocking 禁止) |
| **Wren** | **非移動** (mark-sweep, wren_vm.c で確認) | 明示 slot API `wrenGetSlot*` / `wrenGetSlotCount` | `wrenGetSlotBytes`→ポインタは**安定**(動かない); slot は生存 root |

### A.1 引数パースの型 (#3)

3 系統に分かれる:
1. **ランタイム・フォーマット文字列**: MRI / mruby / CPython(旧) / PHP(旧)。人間工学は良いが varargs パースの per-call コスト + 追加/変更で型不整合が起きやすい。
2. **明示 per-arg チェック関数 (スタック)**: Lua / Perl / Tcl / Wren / V8・Node-API。移動 GC 勢 (Wren/V8) はこちらが多い。
3. **マクロ・インライン / codegen DSL**: PHP **Fast ZPP**(型別 extractor をインライン)、CPython **Argument Clinic**・Perl **typemap**(ビルド時生成)。①の性能/保守性問題への回答として後発で登場。

> **koruby への含意**: mruby は GC こそ非移動で koruby の双子ではない (A.2) が、
> **API 表面**(精密 + 明示 state + arena rooting + 型付きフォーマット引数)は
> koruby に最も近い。引数パースは GC 移動性とは直交するので、**mruby 語彙の
> フォーマット文字列 `korb_get_args`** を採るのが最も驚きが少ない(ただし park
> 規律は koruby 側で移動 GC 用に足す)。一方 PHP/CPython が示す通りホットパスでは
> varargs が効くので、profiling 次第で **Fast-ZPP 風インライン or Clinic 風
> codegen** を後段で重ねる。koruby は node.def から C を生成する処理系なので、
> **Clinic 的 codegen は ASTro の思想に最も自然**(将来の最適形)。

### A.2 文字列データの渡し方 (#1)

**重要な訂正 (2026-08-05)**: サーベイ 10 個のうち**本当に移動 GC なのは V8 と
JNI の 2 つだけ**。CRuby(通常)/mruby/CPython/PHP/Lua/Perl/Tcl/**Wren** は全て
**非移動**(mark-sweep か refcount)。Wren の GC は wren_vm.c で確認 = 非移動
mark-sweep。当初「Wren = 移動 = koruby と同型」と書いたのは誤りで撤回した。

- **非移動 GC (10 中 8)** は `RSTRING_PTR`/`PyUnicode_AsUTF8`/`ZSTR_VAL`/`SvPV`/
  `mrb_get_args "s"`/`wrenGetSlotBytes` で**安定ポインタを直接渡す**。オブジェクトが
  動かないので #1 の問題自体が発生しない。だから彼らの C API は人間工学が良く、
  **移動 GC への移植が難しい**(= koruby が CRuby/mruby ABI をそのまま真似られない
  理由)。Wren/mruby の slot/arena は「生存 root」であって「移動追従」ではない。
- **真の移動 GC peer は V8 と JNI の 2 つだけ**:
  - **V8/Node-API**: 文字列は**常に copy-out**。pin なし。
  - **JNI**: **両方持つ** — copy (`GetStringUTFChars`) と scoped pin
    (`GetStringCritical`, critical-region の間 GC を止める / GC-locker)。

> **#1 への含意**: 移動 GC 下で「生の内部ポインタを alloc 跨ぎで持てない」問題に
> 実際に直面しているのは V8 と JNI だけで、**両者とも copy-by-default**、JNI のみ
> 追加で **scoped pin**(GC を止める critical-region)を持つ。→ 先の順序付け
> (**B は borrow+copy から。pin は Track A が安定ポインタを要求したときに
> JNI-Critical 型の scoped pin として追加**)は、この 2 つの実例と一致する。
> critical-region の「間に alloc/dispatch 禁止」制約 = 提案した epoch-poison 寿命。
> (Wren は非移動なので pin/borrow の区別が不要 = 前例にならない。)

### A.3 ハンドル / rooting (補足)

- 非移動勢: MRI は VALUE を C スタックに置き**保守走査**(koruby は精密なので不可)。
  CPython/PHP/Perl/Tcl は **refcount 手動**(`Py_INCREF`/`SvREFCNT_inc`)。
- 移動勢 (V8/JNI): JNI は **local ref テーブル**(復帰で自動解放、`NewGlobalRef`
  で永続)。オブジェクトが動くので **jobject は間接ハンドル**(生ポインタでない)。
  → koruby の **`VALUE_REF` の正しい対応物は JNI local ref**。どちらも「安定した
  間接セルの中身を GC が書き換える」構造。**Wren slot / mruby arena は非移動なので
  生ポインタが安定という別物**(koruby の re-read 追従は不要)。既存の VALUE_REF
  設計は移動 GC 前提として正解筋で、非移動勢の slot モデルとは似て非なるもの。

### A.4 出典

- Ruby `rb_scan_args`: <https://github.com/ruby/ruby/blob/master/include/ruby/internal/scan_args.h>, <https://silverhammermba.github.io/emberb/c/>, kwarg 分離 <https://bugs.ruby-lang.org/issues/16168>
- mruby `mrb_get_args`: <https://mruby.org/docs/api/headers/mruby.h.html>
- CPython `PyArg_ParseTupleAndKeywords` / Argument Clinic (PEP 436): <https://peps.python.org/pep-0436/>, <https://docs.python.org/3/howto/clinic.html>
- Lua auxlib `luaL_check*`: <https://www.lua.org/source/5.4/lauxlib.h.html>
- PHP Fast ZPP: <https://wiki.php.net/rfc/zpp_improv>, <https://www.phpinternalsbook.com/php7/extensions_design/php_functions.html>
- Node-API: <https://nodejs.org/api/n-api.html>
- JNI Critical / GC locker: <https://docs.oracle.com/javase/8/docs/technotes/guides/jni/spec/functions.html>, <https://shipilev.net/jvm/anatomy-quarks/9-jni-critical-gclocker/>
