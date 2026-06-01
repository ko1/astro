# STRESS rooting fix spec — payload-as-VALUE Array

payload-as-VALUE 移行で default 25/25 + rubyspec 12950 達成済。残るは STRESS+PURGE
(GC-every-alloc) で露出する rooting 漏れ。**設計は正しい**。これは「moving handle を
GC point 跨ぎで C-local 保持 → stale」の純粋な rooting cleanup。

## 根本原因 (2 パターン)

### P1. iterator builtin: 結果 array を park したが c->sp_top を reserve してない
`each`/`map`/`select`/`flat_map`/`reduce` 等。結果を sp[0] に park しても、**korb_yield は
戻りで c->sp_top を呼び出し時のレベルに restore する**。park 時に c->sp_top が sp のままだと、
yield 後 sp[0] は scan 範囲 [stack_base, sp_top) の**外**に落ちて collect される。

**FIX (ary_map が手本、検証済)**:
```c
sp[0] = 0;
c->sp_top = sp + 1;            // ← 結果 slot sp[0] を scan 範囲に reserve (これは必須・正当)
sp[0] = korb_ary_new_capa(c, sp + 1, len);
for (...) {
    VALUE v = korb_ary_aref(sp[-argc - 1], i);   // source は receiver slot から都度読む
    VALUE m = UNWRAP(korb_yield(c, 1, &v));
    korb_ary_push(c, sp + 1, sp[0], m);           // 結果は parked slot sp[0] から
}
c->sp_top = sp;               // ← reserve を戻す
return RESULT_OK(sp[0]);
```
park slot が複数 (例 result + pair) なら sp[0],sp[1] を 0 init して `c->sp_top = sp + 2`、
内側 alloc/push は sp+2 を staging base に。

### P2. source / self を C-local cache せず、receiver slot から都度読む
`self = sp[-argc-1]` を関数頭で C-local に取ると yield 後 stale。**ループ内で毎回
`sp[-argc-1]` を直接読む** (ary_each / ary_map が手本)。block を持たない早期 return path
(to_enum/funcall) では C-local self で可 (その後 GC point を跨がないため)。

## ★ c->sp_top 規約 (混同注意)
- **やってよい**: iterator builtin が yield ループの前に `c->sp_top = sp + K` で**自分の
  park slot を reserve** し、ループ後 `c->sp_top = sp` で戻す。これは P1 の必須要素。
  hash_map(hash.c:652)/ary_map が既にこの形。
- **やってはいけない**: korb_ary_push/aset の **wrapper や alloc helper の callsite** で
  `c->sp_top = sp + N` を書く。push(c, sp, ary, v) は内部で push_sp(c, sp+2) が処理する。
  alloc helper (korb_ary_new_capa 等) は内部で c->sp_top を立てる。callsite は staging base を
  渡すだけ。

## 適用対象
agent が「park したが reserve 漏れ」or「self を C-local cache」した iterator builtin 全部。
各ファイルで `korb_yield`/`korb_funcall`/`korb_funcall_r` を含むループを持つ関数を洗い、
- 結果 array park があるのに `c->sp_top = sp + K` reserve が無い → 追加。
- `self = sp[-argc-1]` を yield 跨ぎで C-local 使用 → `sp[-argc-1]` 直読みに。
を当てる。logic は変えない。

## 検証
各修正後: `rm -f main.o && CCACHE_DISABLE=1 make 2>&1 | grep -c error:` = 0。
代表 1-liner を STRESS で: `echo 'CODE' | ASTRO_GC_STRESS=1 ASTRO_GC_PURGE=1 ./koruby_precise /dev/stdin`。
最終 gate は core 側で `bash tools/gc_harness.sh stress` (全 25 suite) + rubyspec>=7492 維持。
