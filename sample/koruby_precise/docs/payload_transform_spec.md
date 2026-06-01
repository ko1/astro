# callsite transform spec — Array payload-as-VALUE 移行

`struct korb_array` が `VALUE *ptr; long capa;` から `VALUE backing; long len;` に
変わった。`ptr`/`capa` フィールドは廃止。以下の機械変換 + 文脈依存の rooting を施す。
core (object.c/object.h/context.h/koruby_runtime.c) は変換済み。**触らない**。

## 変換ルール

### R1. 要素読み書き: `X->ptr[i]`  →  `korb_ary_items(X)[i]`
`X` は `struct korb_array *`。`korb_ary_items(X)` は要素 VALUE 配列の base を返す
inline (object.h)。読み (`= X->ptr[i]`) も書き (`X->ptr[i] =`) も同じ置換。

### R2. base ポインタ取得: `X->ptr` (添字なし)  →  `korb_ary_items(X)`
例: `func(..., X->ptr, ...)` → `func(..., korb_ary_items(X), ...)`。
`&X->ptr[i]` → `&korb_ary_items(X)[i]`。

### R3. 容量: `X->capa`  →  `korb_ary_capa(X)`  (読み出しのみ。capa への代入は core で廃止済、callsite に無いはず)

### R4. push: `korb_ary_push(ARY, V)`  →  `korb_ary_push(c, SP, ARY, V)`
- `c` は CTX。全 callsite の関数は `c` を持つ。
- `SP` = staging 用の空き value-stack 先端。**2 slot 以上の空きが必要**。
  - builtin の標準は関数頭で `c->sp_top = sp;` 済 → **`c->sp_top` を SP に使う**のが既定。
    `korb_ary_push(c, c->sp_top, ARY, V)`。
  - ★ただし下記 R5 の「結果配列を park して loop」では park slot の**上**を SP にする。

### R5. ★loop で結果配列に push する builtin (map/select/flat_map/zip など) = 要 park
GC point (korb_yield / korb_funcall / korb_*_new / korb_str_concat 等) を跨いで
`struct korb_array *` や その VALUE handle を C-local で保持すると、moving GC で
**handle が stale 化**する (payload 要素は GC 追跡されるが handle 自体は別)。
→ 結果 handle を sp slot に park し、毎回読み直す:

  BEFORE (HEAD, 壊れる):
    VALUE r = korb_ary_new_capa(c, c->sp_top, len);
    for (...) { VALUE m = UNWRAP(korb_yield(c,1,&v)); korb_ary_push(r, m); }
    return RESULT_OK(r);

  AFTER (payload版, 正):
    sp[0] = korb_ary_new_capa(c, sp + 1, len);   // 結果を sp[0] に park
    for (...) {
        VALUE m = UNWRAP(korb_yield(c, 1, &v));   // GC point: sp[0] forwarded
        korb_ary_push(c, sp + 1, sp[0], m);        // parked handle に push、stage sp+1
    }
    return RESULT_OK(sp[0]);

  - `self` も loop 内 GC を跨ぐなら同様に park し `korb_ary_aref(self_slot, i)` で読む。
    builtin の self は `sp[-argc-1]` に居る (GC 追跡される) ので `self = sp[-argc-1]` 再読込で可。
  - sp の空き: builtin は sp から上が空き。park に sp[0], sp[1]... を使い、push の SP は
    park の1つ上。足りなければ素直に番号をずらす。

### R6. `korb_ary_aset(ARY, i, V)`  →  `korb_ary_aset(c, SP, ARY, i, V)`  (R4 と同様、SP は c->sp_top)

## やってはいけない
- **`c->sp_top = sp + N` を書かない** (alloc helper 内部のみ。callsite は SP を渡すだけ)。
- core 4 file (object.c/object.h/context.h/koruby_runtime.c) を編集しない。
- logic を変えない。R1-R3 は純粋置換、R4-R6 は引数追加、R5 は park 追加のみ。

## 検証
変換後 `rm -f main.o && CCACHE_DISABLE=1 make koruby_precise 2>&1 | grep -c error:` が 0。
その後 default test + STRESS+PURGE + rubyspec (>=7492) は core 側でまとめて測る。
