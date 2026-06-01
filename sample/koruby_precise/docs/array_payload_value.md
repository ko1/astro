# Array payload-as-VALUE 移行 (当初デザインの完遂)

2026-06-01 着手。clean HEAD (rubyspec PASS=7492 @ 182-file list) から作り直す。

## 背景: なぜ作り直すか

過去の「Array moving-GC 移行」(.wip_array_migration_20260531/, STRESS 21/25) は
**struct だけ moving、payload は libc malloc のまま** だった:

```c
struct korb_array { struct RBasic basic; VALUE *ptr; long len, capa; };
// korb_ary_new_capa: aro_gc_alloc(struct) + a->ptr = korb_xmalloc(...)  ← ptr は libc
```

→ `a->ptr` を GC が知らない → 中の VALUE を forward しない → 要素 stale/誤collect。
**rubyspec 7492 → 2922 の大規模 regression**。curated 25-suite では検出不能だった。
これは当初デザイン (payload も GC object) から逸れた中途半端な形。

## 当初デザイン = payload-as-VALUE (ascheme_precise が実装済の正解パターン)

要素バッファ (payload) を **それ自体 GC arena 上の独立 heap object (VALUE)** として持つ。
ascheme の `OBJ_VECTOR` / `OBJ_VEC_BACKING` と同型。

### struct (ハンドル) — 住所不変・固定サイズ
```c
struct korb_array {
    struct RBasic basic;
    VALUE backing;   // payload object への普通の VALUE 参照 (heap ref)
    long  len;       // 論理長 — Array(ハンドル)が持つ
    // capa は廃止。backing の header.gc_size から導出。
};
```

### backing (payload) — 独立 GC object、伸びると作り直す
- `aro_gc_alloc(c, sizeof(AroObjectHeader) + N*sizeof(VALUE))` の **SCAN 版**で確保
  (中身が VALUE なので byte 版は不可)。型タグ `T_ARY_BACKING`。
- 要素 VALUE[] は header の直後にインライン。
- **capa(物理容量) = (gc_size - sizeof(header)) / sizeof(VALUE)** で backing から導出。

### 分担 (確定)
| 情報 | 持ち主 | 理由 |
|---|---|---|
| len (論理長) | **Array struct** | 同一 backing 使い回し中に変わる。CRuby RArray も len はハンドル |
| capa (物理容量) | **backing (gc_size)** | 容量と payload は一体。backing 作り直しで変わる |
| 要素 VALUE[] | **backing 本体** | |

### なぜ struct と payload を分けるか (moving GC とは無関係の本質)
Array は identity を保ったまま中身が伸びる必要がある (`b=a; a.push(1); b.last #=> 1`)。
struct と要素を1 object に inline にすると push の realloc で **配列の住所が変わり aliasing 崩壊**。
ハンドル固定 + payload だけ作り直し、で identity を保つ。CRuby RArray も同じ二段構成。

### なぜ payload を VALUE(GC object)にするか
backing の中身は他 heap obj への VALUE。GC が scan/forward せねばならない。
libc malloc だと GC 不可視 → stale。GC object にすれば heap walk が backing を訪れ
gc_size から N 逆算して中の VALUE を全部 forward する。

## 実装ステップ (各ステップで force-rebuild + rubyspec>=7492 + default/STRESS 実測)

1. **enum korb_type に T_ARY_BACKING 追加** (context.h, T_LAST 前)。T_MASK=0x1f に収まる。
2. **struct korb_array 改** (object.h): `VALUE *ptr; long capa;` → `VALUE backing;` (len 残す)。
3. **korb_ary_new_capa 改** (object.c): 二段 alloc。
   - struct を park した上で backing を `aro_gc_alloc(SCAN)` 確保 → `T_ARY_BACKING` タグ
   - `ARO_STORE(c, a, &a->backing, backing_val)` で write barrier 経由書き込み
   - ascheme scm_make_vector が手本 (main.c:219)
4. **要素アクセス helper** (object.h korb_ary_aref/aset/len/push):
   - `len` は `a->len`、容量は backing header から。
   - 要素 base = `(VALUE*)((char*)a->backing + sizeof(AroObjectHeader))`。
   - 読み: `base[i]`。書き: `ARO_STORE(c, a->backing payload base, &base[i], v)`。
5. **korb_ary_push realloc** (object.c): len==capa で大きい backing を alloc → memcpy →
   `a->backing` 差し替え (ARO_STORE)。aro_gc_realloc_payload パターン (sample 実装、
   park-then-alloc-then-memcpy)。
6. **koruby_scan_edges に二段** (koruby_runtime.c):
   - `T_ARRAY` case: `visit_value_slot(&a->backing)` 1 個だけ (今の a->ptr[0..len) walk を置換)。
   - `T_ARY_BACKING` case: gc_size から N 逆算、`for i: visit_value_slot(&items[i])`。
     ascheme AROH_SCAN_EDGES の OBJ_VEC_BACKING が手本 (context.h:663)。
   - **注意**: backing は len ではなく capa(N)個 scan。未使用 slot は Qnil 初期化必須
     (aro_gc_alloc SCAN 版は post-head zero-fill するので 0=未使用、scan は 0 を skip)。

## gate (絶対)
- 各ステップ後: `rm -f main.o` → `CCACHE_DISABLE=1 make` → rubyspec (tools/sp_spec_baseline.sh)
  が **PASS>=7492** + default 25/25 維持。割ったら即止めて原因究明。
- Array 完遂 (rubyspec 7492 維持 + STRESS 改善) を確認してから String(char payload=byte版)/Hash へ。

## 参照
- ascheme_precise: main.c scm_make_vector(219), context.h AROH_SCAN_EDGES OBJ_VEC_BACKING(663),
  builtin.c vector_ref/set/fill/to_list (payload 都度 deref + sp park のループ手本)。
- runtime: aro_gc_alloc(SCAN, gc_common.c:32), aro_gc_size_of, aro_gc_realloc_payload(gc.h:393)。
- [[project_koruby_precise_gc_bump_migration]] memory に旧 WIP の顛末。
