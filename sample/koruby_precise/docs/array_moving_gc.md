# Array moving-GC 移行 (payload-as-VALUE) — 完了報告

2026-06-01。koruby_precise の Array を **完全な moving GC object** に移行した記録。
設計詳細は [array_payload_value.md](array_payload_value.md)、callsite 変換ルールは
[payload_transform_spec.md](payload_transform_spec.md)、STRESS rooting は
[stress_rooting_spec.md](stress_rooting_spec.md) を参照。

## 成果

| 指標 | clean HEAD | 旧 WIP (struct-only) | **payload-as-VALUE** |
|---|---|---|---|
| rubyspec PASS (182-file list) | 7492 | 2922 | **12973** |
| default suite | 25/25 | 25/25 | **25/25** |
| STRESS+PURGE suite | 25/25 | 21/25 | **22/25** |

rubyspec が clean HEAD 比 **+5481** (旧 struct-only WIP の 4.4 倍)。Array が moving GC で
正しく動くようになり、以前 crash/abort していた多数の spec が完走するようになった。

## STRESS+PURGE rooting campaign (2026-06-01 第2弾)

curated 28-suite は通っても、rubyspec を **STRESS+PURGE で広く流す** と多数の latent crash が
残っていた。`tools/sp_stress_sweep.sh` (182-file fixed list を STRESS+PURGE で完走 PASS/CRASH 集計)
で計測しながら、支配的 crash site から潰した。

| 段階 | rubyspec STRESS PASS | CRASH |
|---|---|---|
| campaign 開始時 | 179 | 138 |
| dispatch recv + array builtins | 555 | 81 |
| **eval_frame_chain** + lshift + rng_each | 1092 | 80 |
| class_new/obj_dup/module/str_clone/const_path/str | **1964** | **53** |

PASS が **11×**、CRASH が **−61%**。FAIL/ERR が増えたのは、crash していた file が完走して
feature-gap の assertion (未実装メソッド等) を露出するようになったため (= 前進)。

### 効いた fix (大きい順)
1. **eval_frame_chain** (最大、PASS 555→1092): `korb_eval_string` の top_frame は制御フロー隔離で
   `prev=NULL` push されるため、nested require/load 中に外側 (suspended) eval top_frame が
   `visit_roots` の head chain から切れ、その間に `main_obj` が動くと `frame->self` が stale 化。
   → `DISPATCH_node_ivar_get` / `korb_class_of_class(recv)` SEGV (mspec harness 全体で多発)。
   CTX に `eval_frame_chain` stack を追加し全 eval top_frame を walk。`korb_frame.eval_prev` で linked。
2. **dispatch recv** (`korb_dispatch_to_method` AST path): rest-array/kwh alloc の GC を跨いで
   recv stale。CTX `dispatch_recv_root` に park。
3. **EVAL_node_lshift** の Array fast-path (`a<<x`): grow 後 stale C-local l を return →
   stored-closure `acc<<x` crash の真因 (`a.push` は別 path で OK だった)。
4. **builtin stale-self / class-self handle** 多数: ary_first_n/last_n/delete_at/class_brackets/
   initialize, class_new, module_methods_by_vis, obj_dup, str_clone, str_include, str_sub_bang,
   node_const_path_get の parent module 等。型は IDIOM A (GC 後 `sp[-argc-1]` 再読込) /
   B (synthetic frame) / C (class handle sp park + re-derive) の 3 つ。

### 残課題 (campaign 後、CRASH=53)
- **string 系 builtin の self-by-value impl** (str_tr_bang_impl 等) — parking 規約が無く mechanical follow-up。
- **string range の STRESS crash** — range が xmalloc (非moving) で begin/end の moving string が
  libc-obj registry forward の timing で破壊される (`spec_port_pending.md`)。range→arena 移行が要る。
- **clone_spec** — str_clone 後も singleton/frozen edge で koruby_scan_edges 内 crash。
- test_fiber (gate の残 1)。

## 設計の要点

### 旧 WIP がなぜ失敗したか
旧 struct-only WIP は `struct korb_array { VALUE *ptr; ... }` の **struct だけ arena に置き、
要素バッファ `ptr` は libc malloc のまま** にしていた。GC が `ptr` バッファを知らないため
中の VALUE を forward せず、要素が stale/誤collect → rubyspec 7492→2922 の大規模 regression。

### payload-as-VALUE (当初デザイン)
要素バッファ自体を独立 GC object にする (ascheme_precise の OBJ_VECTOR/OBJ_VEC_BACKING と同型)。

```c
struct korb_array {           // ハンドル: 住所不変・固定サイズ
    struct RBasic basic;
    VALUE backing;            // payload object への普通の VALUE 参照
    long  len;               // 論理長 (ハンドルが持つ)
};
struct korb_ary_backing {     // payload: 独立 GC object (T_ARY_BACKING)
    struct RBasic basic;
    VALUE items[];           // 要素 VALUE[] をインライン
};
```

- **len** (論理長) はハンドル、**capa** (物理容量) は backing の `header.gc_size` から導出。
- backing は `aro_gc_alloc` (SCAN版) で確保。要素 access は `korb_ary_items(a)[i]`。
- scan_edges 二段: `T_ARRAY` は `backing` 参照1個を visit、`T_ARY_BACKING` は gc_size から
  要素数 N を逆算して `items[0..N)` を walk (reader とデータが co-located、backend 非依存)。
- push は `korb_ary_push(c, sp, ary, v)` = ary/v を sp[0]/sp[1] に park し `push_sp(c, sp+2)`。
  grow は `korb_ary_grow` が大きい backing を確保 → memcpy → handle 差し替え。

### identity が保たれる理由
ハンドルが住所不変なので `b = a; a.push(x); b.last` が CRuby 通り動く。push の realloc は
backing を作り直すだけでハンドルは不動。CRuby の RArray と同じ二段構成。

## ★ c->sp_top 規約 (厳守)
- **callsite で `c->sp_top = sp + N` を書かない。** alloc helper の中で GC trigger 直前のみ。
  push(c, sp, ary, v) は内部の push_sp(c, sp+2) が処理。callsite は staging base を渡すだけ。
- 例外: iterator builtin が yield ループ前に自分の park slot を reserve する場合のみ
  `c->sp_top = sp+K` ... ループ後 `c->sp_top = sp` で戻す (下記の通り、これも不足で
  最終的に synthetic frame に移行した)。

## STRESS rooting: synthetic frame 方式

iterator builtin (`map`/`select`/`each` 等) が結果を sp slot に park しても、**korb_yield は
block body を block 自身の (低い) sp で実行 → scan 範囲 [stack_base, c->sp_top) が parked slot
より下に縮んで collect される** (旧 WIP の vstack_frontier と同じ問題)。

解決 = **synthetic frame**: root を合成 `korb_frame` の `last_line`/`last_match` slot に park し
`c->current_frame` に push。frame chain は `visit_roots` が `sp_top` 無関係に必ず walk するので、
yield が sp_top を下げても root は生存。node_plus と同手法。`KORB_ARY_YIELD_FRAME` (array.c) /
`KORB_HASH_YIELD_FRAME` (hash.c) マクロ。**全 exit path で `c->current_frame = fr.prev` 復元必須**。

その他: class/object handle を GC point 跨ぎで保持する builtin (module attr_*, struct_class_new,
NoMethodError path の recv 等) は sp slot に park + GC point 毎に re-derive。

## 残課題 (payload migration の regression では無い、pre-existing を GC 圧で炙り出したもの)

- **test_fiber**: fiber は stack 切替 (c->stack_base/sp_top swap) するので synthetic frame の
  扱いが別レイヤ。fiber 特有 edge。
- **test_alias_redef**: 未定義 method の NoMethodError path で recv (T_OBJECT = clean HEAD でも
  moving) が dispatch 入口で stale。rescue 経路特有。
- **test_hash**: compare_by_identity / array_key_identity の 3 assert (crash でない)。moving GC で
  object address が変わると identity hash が壊れる既知の semantic 問題。

## 変更ファイル
core: object.c / object.h / context.h / koruby_runtime.c / node.def。
builtins: array / hash / comparable / module / integer / kernel / object / string / range /
float / file / proc / binding / exception / math + builtins.c + main.c。
