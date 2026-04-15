# optcarrot `-c` (compile-first) 性能分析とボトルネック

日付: 2026-04-15
commit: 55bf417 (skip cs_compile when already specialized) 上でのプロファイル

## 計測概要

```
$ exe/abruby -c benchmark/optcarrot/bin/optcarrot-bench -b   # optcarrot, 180 frames
```

| 実行環境 | fps | wall time | 備考 |
|---|---:|---:|---|
| `ruby benchmark/optcarrot/bin/optcarrot-bench -b` | 44.0 | 4.52 s | baseline |
| `exe/abruby -c ...` (cold, 空 store) | 52.6 | 5.51 s | 初回: cs_compile × 600, cs_build × 16 |
| `exe/abruby -c ...` (warm, store populated) | 52.0 | 3.92 s | ALLOC 時に cs_load → 再 compile スキップ |
| `exe/abruby -c ...` (warm再実行) | 50.6 | 4.00 s | |

warm 実行で **fps +18% / wall -13% vs ruby**。cold run は cs_build が支配的。

## 改善履歴 (本セッション)

1. **`Parser.on_parse` フックで require_relative / eval も code_store に投入** — 以前はトップレベル AST と直接の def entries のみコンパイル対象。optcarrot の 20+ ファイルが plain-dispatcher のままで -c 効果が出ていなかった。
2. **`astro_cs_reload` を世代別ファイル名で dlopen** — 同一 `all.so` を再 dlopen しても dynamic loader は古いハンドルを返すため新シンボルが見えなかった。各 reload を `all.<N>.so` にハードリンクしてから dlopen。
3. **`const char *` フィールドの DUMP で C 文字列エスケープ** — 改行・引用符を含む `node_str_new` が SD_.c 内のコメントを壊して gcc がパース失敗していた。
4. **`already-compiled` ノードの cs_compile スキップ** — warm run で無駄に make を再実行して link していたのを抑制。

## perf プロファイル (warm run, 4019 samples, 99 Hz)

```
     5.40%  SD_28b9cd1c8a2716e7      (cpu.rb 内部 hot loop)
     4.76%  SD_f1d0907c12437133      (同上, step block body)
     3.09%  abruby.so  ab_array_get
     3.08%  libruby.so rb_gc_mark_children
     2.67%  all.so     prologue_ast_simple_0
     2.48%  all.so     prologue_cfunc
     2.26%  libruby.so gc_mark_internal
     2.12%  SD_c706c0c2dc469711
     1.38%  libruby.so ary_ensure_room_for_push
     1.34%  libruby.so rb_ary_entry
     1.16%  libruby.so rb_ary_push
     1.13%  libruby.so rb_ary_splice
     1.03%  libruby.so rb_gc_mark
     0.94%  abruby.so  abruby_yield
     0.88%  abruby.so  DISPATCH_node_call1_cfunc
```

## ボトルネック分類

### 1. GC が ~8% (rb_gc_mark_children 3.1% + gc_mark_internal 2.3% + gc_sweep_step 0.9% + gc_aging 0.9% + rb_gc_mark 1.0%)

optcarrot の CPU 命令 1 本ごとに短寿命オブジェクト (Fixnum はイミディエイトだが ivar 書き込みで shape bump / RARRAY_AREF など) が発生。

**考えられる改善**:
- `abruby_object` の inline-ivar split — 既に done だが、nbody (7 ivars) のように 4 を超えるクラス (CPU@registers 等) は heap extra に回る。extra ivar を「ちょうど必要な数 + flexible array member」で確保する (todo.md にも記載済み)。
- 機械 stack scan の縮小 — 現在 VALUE stack 10000 スロット全体をマーク。`sp` high-water mark を厳密に使う (todo.md §VALUE スタック遅延 GC マーク)。

### 2. CRuby Array 操作が ~5% (rb_ary_entry / rb_ary_push / rb_ary_splice / ary_ensure_room_for_push)

optcarrot は PPU / APU でリングバッファ的な Array 操作を多用。

**考えられる改善**:
- `ab_array_get` / `ab_array_set` を FIXNUM 添字で `RARRAY_AREF` 直接アクセスに狭める (bounds check を 1 回で済ませる)。
- `ary_ensure_room_for_push` が重い場合は、事前 reserve API を ab_array に足して optcarrot 側で使える余地がある。

### 3. SD_ 関数群 (ユーザコード) が ~20%+ 合計

5% / 4.7% / 2.1% / 1.4% … と散らばっており、個々のノードは重くない。全体として**命令ディスパッチの上位 5-6 関数に集中していない** = optcarrot の CPU emulator は制御フローが平坦でないため、specialize された inline chain が深くならない。

**考えられる改善** (いずれも todo.md §AOT / JIT):
- **call 特化 Phase 2 (callee body 特化)** — dispatch の indirect call を消せる。nested SD_ からさらに方法 body を展開できれば、現状 SD_ 内に残っている `prologue_ast_simple_0` (2.67% self) が吸収される。
- **method inlining** — `@noinline` ロック解除 + 型 feedback。SD_ 内の cfunc / ast method body を直接展開。
- **ローカル変数レジスタ化** — `c->fp[i]` の pointer aliasing が LICM を阻害している。

### 4. prologue / DISPATCH オーバーヘッド ~4%

`prologue_ast_simple_0` 2.67%, `prologue_cfunc` 2.48%, `DISPATCH_node_call1_cfunc` 0.88%。

**todo.md に計画済み**: `mc->prologue` の定数化 (ノードフィールドのロード時解決)。これが入れば SD_ 内から直接 prologue 本体が inline 展開される。

### 5. abruby_yield は 0.94% — 既にかなり軽い

Integer#step の block 呼び出しは、以前 (cold 側で) 7% に見えたが、warm では 0.94% 程度。abruby_yield 自体の最適化より、block body 側の SD_ を厚く inline 化する方が効く。

## まとめ

- `-c` での optcarrot は **warm run で ruby 比 +18% fps**、end-to-end の時間でも上回る。
- 残る runtime 上のボトルネックはおおむね **(1) GC と (2) CRuby Array/libruby 呼び出し** に偏っており、specialization 自体は効いている。
- specialization を**さらに**効かせるには todo.md の **call 特化 Phase 2 / method inlining / lvar レジスタ化** が必要で、これ以上はインタプリタ本体を弄らずに数%上げるのは難しい。

## 実施した改善の内訳

| 項目 | 効果 |
|---|---|
| Parser.on_parse 経由で全ファイルを compile 対象化 | optcarrot の -c が初めて意味を持った |
| 世代別 all.so + dlopen | cs_reload の静かなバグを修正、require_relative 後の新 SD_ が可視 |
| DUMP cstr エスケープ | optcarrot の str リテラル由来の build 失敗を解消 |
| verify_compiled による skip | warm wall time 6.5s → 3.9s (-40%) |

残ボトルネック改善は todo.md の既存プランに沿って進めるのが妥当。
