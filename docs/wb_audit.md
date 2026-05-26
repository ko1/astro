# WB 漏れ検出 mechanism 設計メモ

## 動機

世代別 GC では tenured→young の reference は write barrier (= WB) を経由
して remset に登録される必要がある。 WB を呼ばずに直書きすると、 minor GC
で young 子が「参照されてない」 と誤判定されて回収され、 後で dangling
pointer SEGV や微妙な値破壊として顕在化する。

現状の検出手段:

- **`copy_scramble` backend** (= `runtime/precise_gc/gc_copy_scramble.c`):
  per-cycle XOR mask `R` で VALUE slot を撹乱、 stale slot を deref した
  時に SEGV で検出。 確率的・実行時 cost あり
- **STRESS mode** (`BARUBY_GC_STRESS=1`): 全 alloc で GC を発火、 短命
  obj が早期に dead 化して dangling を顕在化。 確率的・遅い

どちらも runtime audit で、 **「WB を呼ばずに書いた箇所」 を特定する手段
が無い** (= 症状だけ見えて原因不明)。

## 提案: 型システムによる compile-time 検出

heap pointer field の型を `const` 化し、 直接代入を compile error にする。
WB 付 `OBJ_STORE` macro のみが `(T *)&field` cast で const を外せる、
という mechanism。

### 効果

- **compile-time 100% 検出**: WB を呼ばずに `obj->field = x` した瞬間に
  C compile error。 runtime stress 不要、 0 cost
- **scramble / stress と補完的**: scramble は「WB 呼んだが stale 化した
  slot」、 stress は「dangling 顕在化のタイミング前倒し」、 本機構は
  「そもそも WB を呼ばずに書いた箇所」 を catch
- **release は 0 cost**: `#ifdef ARO_GC_WB_AUDIT` で debug build 限定。
  本番 build は plain pointer に戻り、 cast 不要

## 設計詳細

### 1. heap object field の const annotation

heap 上の VALUE pointer field と VALUE element を `const` qualified に
する。 例 (baruby_precise):

```c
#ifdef ARO_GC_WB_AUDIT
#  define ARO_WB_FIELD const     /* compile-time guard for heap-ptr fields */
#else
#  define ARO_WB_FIELD           /* release: no-op, plain field */
#endif

typedef struct BaArray {
    AroObjectHeader head;
    uint32_t len;
    uint32_t capa;
    BaArrayItems *ARO_WB_FIELD items;   // ← WB 経由でのみ書換可
} BaArray;

typedef struct BaArrayItems {
    AroObjectHeader head;
    VALUE ARO_WB_FIELD data[];          // 各 element も const
} BaArrayItems;
```

audit build では `a->items = new_items;` が compile error:

```
error: assignment of read-only member 'items'
```

### 2. STORE macro (= WB + const cast)

```c
/* obj->field = val with WB.  In audit build, casts away const. */
#define ARO_OBJ_STORE(c, holder, field, val) do {                    \
    AROH_VALUE_TYPEOF((holder)->field) _v = (val);                   \
    aro_gc_wb((c), (holder), (VALUE *)(uintptr_t)&(holder)->field,   \
              (VALUE)_v);                                            \
    *(AROH_VALUE_TYPEOF((holder)->field) *)(uintptr_t)&(holder)->field = _v; \
} while (0)

/* Array element store. */
#define ARO_OBJ_ARRAY_STORE(c, items, idx, val) do {                 \
    VALUE _v = (val);                                                \
    aro_gc_wb((c), (items), (VALUE *)(uintptr_t)&(items)->data[idx], _v); \
    *(VALUE *)(uintptr_t)&(items)->data[idx] = _v;                   \
} while (0)
```

key point:
- `(uintptr_t)&(holder)->field` で const を外す cast (= 完全 UB 回避は
  難しいが、 元の宣言を「conceptual const」 として扱うので実用上 safe)
- WB は変更前に呼ぶ (= SATB style ではなく post-write barrier; 詳細は
  各 backend の WB 仕様)
- audit OFF 時は `ARO_WB_FIELD` = 空、 cast は no-op (= 普通の代入)

### 3. 初期化 path

新規 alloc 直後の obj は heap reachable でない (= roots から到達不能)
ので WB 不要。 ただ `obj->field = x` を const のせいで書けない。

選択肢:
- (a) `ARO_OBJ_INIT_STORE(holder, field, val)` macro: WB を呼ばずに cast
  だけする。 alloc 直後の初期化専用
- (b) `ARO_OBJ_STORE` をそのまま使う: WB の fast path で OLD bit を check、
  alloc 直後は OLD = 0 なので no-op return。 cost ほぼ 0
- (c) `memcpy` で初期化: cast 不要、 全 field を一括で埋める。 既存
  pattern と相性◎

**推奨**: (b) を default、 hot path で気になれば (a)。 (c) は struct
literal で書きたい時。

### 4. 既存 code の migration

baruby_precise を例に直接代入を grep:

```bash
grep -nE "[->]\.?items\s*=|[->]\.?bytes\s*=|[->]data\[.*\]\s*=" sample/baruby_precise/*.c
```

各 hit を `ARO_OBJ_STORE` か `ARO_OBJ_ARRAY_STORE` に置換。 数十箇所
予想。 sed 一括 + 残った compile error を case by case 修正。

### 5. coverage

const annotation で catch できる:

- `BaArray.items` ← `BaArrayItems *` (heap ptr)
- `BaString.bytes` ← `BaByteData *` (heap ptr)
- `BaArrayItems.data[]` ← `VALUE` (heap ptr) per element
- `BaByteData.data[]` ← `uint8_t` (= 非 VALUE、 対象外、 const 不要)

class instance vars / ivars が将来加わるなら同様に const 化。

### 6. backend hook の整合性

WB-less backend (= `none`, `bump`, `copy`, `mark`, `copy_scramble`,
`mark_freelist`, `mark_compact`, `immix`、 8 個) では `aro_gc_wb` は
no-op で定義済 (`#define aro_gc_wb(c, h, s, v) ((void)(s), (void)(v))`
等)。 audit ビルドでも cast の経路を通って正しく書き込まれる。

世代別 backend (= 8 個) では `aro_gc_wb` の fast path / cold path で
remset push。 const cast は WB 後に書込み。

## 段階的 rollout

1. **PoC**: baruby_precise のみで実装。 1 backend (= `mark_gen`) で
   audit build を試す。 全 testbench pass を確認
2. **expansion**: ascheme_precise にも展開 (= 同 framework 利用)
3. **CI integration**: `ARO_GC_WB_AUDIT=1` build を CI に追加、 build
   error = WB 漏れとして fail
4. **既存 bug の発掘**: audit build で残っている WB 漏れを潰す

## 試算: WB 漏れの実例 (= 既知)

過去の bug の多くが「WB 漏れの間接症状」 だった:

- `mark_bitmap_gen` の N-survive bug (= commit `a8914250` 修正)
  → major sweep が dirty_bm を clear してなくて、 WB は呼ばれたが
    invariant 違反 (= 別系統だが、 WB 周辺の bug)
- `aro_gc_realloc_payload` の stale ptr (= sample/baruby/bench/hash_chain.ba.rb
  history) → WB 後に sp スロットへの park が必要、 buf 内 heap ptr が
  scan 範囲外 (= これも WB 漏れの近縁)
- baruby_precise iter 35-40 の dangling pointer 痕跡

これらが 「audit build で必ず compile error として出る」 のが理想。

## 開発上の cost

- migration: 数十箇所の直接代入を置換 (= 1 sample あたり 1-2 時間)
- maintenance: 新 obj 追加時に WB-protected field を `ARO_WB_FIELD`
  qualify する習慣化
- audit build を CI で回す追加時間: ~30 秒 × N sample

## open question

1. `const` cast の UB 懸念: C 標準は「元が `const T` 宣言だった field
   への書込は UB」 とする。 実用上 gcc / clang は問題ないが、 厳密に
   は `aro_compat.h` で `__attribute__((no_sanitize("undefined")))`
   等の hint が必要か
2. union を使う代替: `union { T x; T const cx; }` で書込と読出を分ける
   pattern。 cast 不要だが冗長
3. C++ なら `private` + accessor で同等、 ASTro は pure C なので採用せず
4. `_Static_assert` で field offset を check しておけば cast の memory
   layout 保証は強化できる

## 次のステップ

1. user review (= 本メモを確認、 設計方針 OK / NG / 修正点)
2. PoC ブランチで baruby_precise + 1 backend (`mark_gen`) で実装
3. testbench 走行で漏れ検出 + 修正
4. 既知 bug の中で 「audit build で catch できるはず」 のものを試して
   実証
