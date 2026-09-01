# ObjectSpace.each_object — moving GC 上でヒープ walk はできるか

結論: **できる**。`runtime/precise_gc/gc_copy.c` の Cheney semispace は
bump 割り当てで隙間がないので、線形 walk が健全に成立する。
`aro_gc_each_object` として実装済み。

## なぜ健全か

walk の前提は「ある領域を先頭から `gc_size` 刻みで辿ると、必ず次の
object header に着地する」こと。gc_copy ではこれが 4 つの性質から出る。

1. **隙間がない。** `gc_bump` は `active_top` を `ALIGN8(gc_size)` だけ
   進める単純な bump で、free list も孤立した空き穴も作らない。
   `[active_base, active_top)` は「前回の collect 以降の割り当てログ」
   そのものになっている。
2. **header が必ず先頭にある。** `korb_alloc` は 1 箇所しかなく
   (`aro_gc_alloc` の呼び出し元は全ソース中でここだけ)、必ず
   `AroObjectHeader` を payload offset 0 に置いて `gc_size` と型タグを
   埋める。型タグの付いていない arena object は存在しない。
3. **forwarding pointer に出会わない。** gc_copy の forwarding は
   from-space 側の payload offset 8 に overlay される。from-space は
   collect の最後に retire されるので、active space には forwarding
   overlay を持つ object は 1 つも残らない。
4. **stale pointer に出会わない。** active space の object は
   (a) 直前の collect で copy された生存者 — 全 edge が forward 済み、か
   (b) その後の新規割り当て — 全 edge が現 active space を指す、
   のどちらか。前世代の plane を指すフィールドは原理的に存在しない。

大きい object (payload >= 4 KiB) は arena の外で malloc され
`gc->large_head` の linked list に載るので、そちらは list を辿る。

### 罠 1: visitor は絶対に alloc してはいけない

walk 中に GC が走ると全 object が move し、cursor は retire された
plane を指す (PURGE 下なら PROT_NONE で即 SEGV)。よって
`aro_gc_each_object` の visitor は alloc 禁止で、Ruby の block を
walk の途中で yield することはできない。

`__heap_objects__` はこれを **2 pass** で回避する。

1. pass 1: 条件に合う object を数えるだけ (alloc なし)。
2. その数ちょうどの capacity で結果 Array を確保する (ここでだけ GC 可)。
3. pass 2: 詰め直す。capacity が足りているので
   `korb_ary_push_val` は grow path に落ちず、1 回も alloc しない。

pass 2 の件数が pass 1 を超えることはない。間に挟まる GC は object を
減らすだけだし、間に作られた結果 Array 自身は identity で除外している
(その backing store は `KORB_OBJ_VALUE_ARRAY` なのでそもそも見えない)。
capacity は常に足りる。

### 罠 2: まだ回収されていないゴミも見える

active space は「前回の collect 以降の割り当てログ」なので、死んでいる
object もそこに残っている。walk はそれも yield する。これは CRuby の
`rb_objspace_each_objects` と同じ性質なので仕様として受け入れている。
walk の前に collect を強制すればゴミは消えるが、そうすると
「CRuby なら生きている扱いの object が precise GC では回収済み」という
逆方向のズレが出るので、CRuby に寄せて collect しない方を選んだ。

なお `BARUBY_GC_STRESS=1` (毎 alloc collect) 下では each_object_spec の
2 例 (block に暗黙に捕捉された local / Proc#binding 経由) が落ちる。
これは GC-safety のバグではなく、koruby がその env を materialize せず
precise に回収するために起きる意味論の差である。

### 罠 3: 内部 object を Ruby に見せない

arena には Ruby object ではない payload も入っている。除外するのは 3 種:

| tag | 中身 |
|---|---|
| `KORB_OBJ_VALUE_ARRAY` | Array/Hash の backing store |
| `KORB_OBJ_STR_BUF` | String の backing store |
| `KORB_OBJ_ENV` | closure env |

残りは全部 Ruby から見える object なので、`korb_obj_kind_of_p` で
`is_a?` フィルタをかけるだけでよい。

## コスト

**each_object を呼ばない限りゼロ。** 追加した bookkeeping は無い —
alloc path も write barrier も collector の scan loop も一切変更して
いない。walk は「その場で arena を線形に舐める」だけで成立するので、
class registry のような常時維持するテーブルも要らない。

計測 (`perf stat -e instructions`、3 回、interp / AOT 両方):

| bench | mode | before | after | 差 |
|---|---|---|---|---|
| fib(30) | interp | 926.2 M | 926.2 M | ±0.0% |
| fib(30) | AOT | 486.1 M | 485.3 M | −0.17% |
| gcchurn | interp | 6280 M | 6280 M | ±0.0% |
| gcchurn | AOT | 2538 M | 2538 M | ±0.0% |
| gc_bigobj | interp | 2880 M | 2881 M | +0.04% |
| gc_bigobj | AOT | 2512 M | 2510 M | −0.06% |

すべて run 間のばらつきの範囲内。

## 他 backend

`aro_gc_each_object` は `gc_common.c` に weak default (「walk 不可」を
返す) を置いてある。walk できるのは gapless bump 領域を持つ backend だけ
なので、mark-sweep 系や free list を持つ backend で対応するなら
それぞれの側で override する。walk できない backend では
`ObjectSpace.each_object` は `NotImplementedError` になる。

## 未対応

- `each_object` の singleton class 2 例 (`sclass.is_a?(meta)`)。koruby の
  singleton class は superclass に実クラスを置く設計で、メタクラス階層を
  持たない。each_object 側ではなくクラス階層側の課題。
- `_id2ref` の heap object 解決は、object_id が address 由来なので
  GC を跨ぐと引けない (RangeError)。STRESS 下では常にそうなる。
