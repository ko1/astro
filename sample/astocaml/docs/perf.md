# 性能改善ログ

astocaml の性能まわりで「**やったこと / その効果**」を時系列で残す。

## ベースライン (Phase 1 完了時)

`make bench`, gcc -O2:

| ベンチ | 入力 | 結果 | 時間 |
|--|--|--|--|
| ack    | ack(3, 9)        | 4093    | 1.20 s |
| fib    | fib(35)          | 9227465 | 2.62 s |
| nqueens| 10-queens × 3    | 724     | 1.59 s |
| sieve  | sum prime ≤8000 ×8 | 3738566 | 1.21 s |
| tak    | tak(24,16,8) ×5  | 9       | 1.23 s |

## Phase 2 中間 (多相比較・トランポリン導入後; gref キャッシュ前)

| ベンチ | 結果 | 時間 |
|--|--|--|
| ack    | 4093    | 3.0 s |
| fib    | 9227465 | (fib33 で) 2.6 s |
| nqueens| 724 (×3) | 1.6 s |
| sieve  | 3738566 (×4) | 1.0 s |
| tak    | 9 (×2) | 1.4 s |

機能追加 (多相比較・トランポリン・variant/record/object チェック) で 25-30% 退化。

## 現状 (gref IC + TCO + 比較ファストパス + method send IC + closure leaf alloca + 型特化 binop + AOT 全 closure body + LTO)

`./astocaml -c bench/<x>.ml` (warm cache, min of 5):

| ベンチ | 入力 | 結果 | 時間 |
|--|--|--|--|
| ack    | ack(3, 9)         | 4093    | 0.090 s |
| fib    | fib(35)           | 9227465 | 0.146 s |
| nqueens| 10-queens × 3     | 724     | 0.275 s |
| sieve  | sum prime ≤8000 ×8 | 3738566 | 0.322 s |
| tak    | tak(24,16,8) ×5   | 9       | 0.075 s |
| method_call | counter#incr 10M | 10000000 | 1.0 s 程度 |

ベースラインからの累積効果:
- fib: 2.62 s → 0.146 s (**18× 高速化**)
- ack: 1.20 s → 0.090 s (**13×**)
- tak: 1.23 s → 0.075 s (16×)
- nqueens: 1.59 s → 0.275 s (5.8×)
- sieve: 1.21 s → 0.322 s (3.8×)

公式 OCaml 4.14.1 との比較 (warm cache):

| bench | astoc | astoc(-c) | ocaml(top) | ocamlc(BC) | ocamlopt |
|--|--|--|--|--|--|
| ack | 0.35 | **0.09** | 0.20 | 0.12 | 0.011 |
| fib | 0.44 | **0.15** | 0.38 | 0.20 | 0.040 |
| nqueens | 0.64 | 0.28 | 0.22 | 0.17 | 0.017 |
| sieve | 0.55 | 0.32 | 0.26 | 0.23 | 0.019 |
| tak | 0.28 | **0.08** | 0.25 | 0.15 | 0.014 |

ack / fib / tak で **astocaml AOT が ocamlc bytecode を上回る**。 nqueens / sieve は list 操作が重く ocamlc に対し 2× 程度の差。 ocamlopt (native) との差は 3.5× (fib) 〜 20× (nqueens / sieve)。

**主要な勝利順**:
1. **gref インラインキャッシュ** — 全ベンチ 3-5×
2. **AOT specialize** (closure body + match arm まで対象) — 全ベンチ 2-3×
3. **closure leaf alloca** — fib 等で frame の malloc を排除、+2× 程度
4. **`node_appN` closure-leaf fast path** — `oc_apply` の type chain を caller 側で skip
5. **型特化 binop (`_int` 系)** — 型推論結果を dispatcher swap で焼き込み

## 試した改善

### ✅ Inline cache for gref (大勝利)

実装:
- `node_gref` / `node_gref_q` に `struct gref_cache *cache @ref` を追加。
- `c->globals_serial` を `oc_global_define` ごとに bump。
- ホットパス: `cache->serial == c->globals_serial` なら cached value を即返す (2 load + 1 cmp)。
- 冷ホットパス: `oc_global_ref` で線形探索 → cache 更新。
- ASTroGen の `@ref` 対応のため `astocaml_gen.rb` を作成 (ascheme と同パターン)。

効果: ベンチ全般で 3-5× 高速化。最大の単一改善。

```
fib(33):  3.0 s →  0.6 s   (5×)
ack(3,9): 3.0 s →  0.84 s  (3.6×)
tak:      1.5 s →  0.29 s  (5×)
```

### ✅ 末尾呼び出し最適化 (TCO)

実装:
- パース後 post-pass `mark_tail_calls()` が tail-position の `node_app{0..4}` の dispatcher を `node_tail_app{0..4}` に **in-place で swap**。
- `node_tail_app_K` は `c->tail_call_pending=1` をセットして dummy を return。
- `oc_apply` は `loop:` ラベル + `goto loop` のトランポリン。

効果: 機能正しさ。1M 段階の `count_down` が定数スタックで通る。
非 tail 呼び出しのオーバーヘッドは pending check 1 回 (UNLIKELY) のみ。

### ✅ 比較演算の fixnum インラインパス

実装:
```c
NODE_DEF
node_lt(...) {
    VALUE av = EVAL_ARG(c, a);
    VALUE bv = EVAL_ARG(c, b);
    if (LIKELY(OC_IS_INT(av) & OC_IS_INT(bv)))
        return (OC_INT_VAL(av) < OC_INT_VAL(bv)) ? OC_TRUE : OC_FALSE;
    return (oc_compare(av, bv) < 0) ? OC_TRUE : OC_FALSE;
}
```

効果: int だけのコードでは多相 `oc_compare` の関数呼び出しを回避。fib / tak など int-heavy ベンチで ~10% 改善。

### ✅ ASTroGen の always_inline 活用

各 `EVAL_node_*` は ASTroGen が自動で `static inline __attribute__((always_inline))` 付きで生成する。
`setjmp` を含む `node_try` は always-inline と両立しないので `oc_run_try` に切り出した。

### ✅ Pattern compiler の単一フレーム化

最初は cons パターンを `node_match_list` で chain する設計だったが、`[]` と `h::t` の両方を扱う場合に depth ずれる重大バグ。

修正: `node_match_arm` という統一ノードを 1 個用意し、**arm 内の全変数を 1 つのフレームに集約**。
パターンコンパイラ (`pat_gen_test` / `pat_gen_extracts`) が事前にテスト式と extractor 配列を組み立てる。

### ✅ コードストア (AOT) wireup

`--compile` (`-c`) オプションで各トップレベル expression を `astro_cs_compile` → `astro_cs_build` → `astro_cs_reload` → `astro_cs_load`。
小幅改善 (10%)。specialize 自体は他のサンプル (ascheme, wastro) のように真価を発揮しておらず、まだ改善の余地大。

### ✅ モジュール対応 gref (`node_gref_q`)

ネストした module 内の bare 名前参照を、`<prefix>.<name>` と `<name>` の両方を試行。1 つのノードでカバーするので AST のサイズが膨らまない。

### ✅ Method send IC (`obj#m`)

実装:
- `node_send` に `struct send_cache *cache @ref` を追加 (gref と同パターン)。
- ホットパス: `cache->names_ptr == oo->obj.method_names` ならキャッシュ済み closure を即取得、`strcmp` ループを完全スキップ。
- 各オブジェクトインスタンスは独自の `method_names` 配列を持つので、IC は **同一インスタンスを繰り返し呼び出すループ** で効果が大きい。
- 冷ホットパス: `oc_object_lookup_method` で線形探索 → cache 更新。
- フィールド読み出し / `set_X` のフォールバックは従来の `oc_object_send` に丸投げ。

効果: method_call ベンチで 2.1 s → 1.66 s (**1.34× 高速化**)。
注: より大きな勝利はクラス間で method table を共有する版だが、現状では各 `new` がフレッシュな配列を確保するため per-instance に留まる。

### ✅ tiny / `c->reg` / `_iu` を撤去 — シンプル路線へ

短期的に `c->reg[16]` + `node_lref0_reg` で frame skip を試した (commit a3b7234) が、user 指摘で再検討:

問題:
- `c->reg` は CTX 内の共有スクラッチ。CPU register と違い memory access。
- 再帰のたびに save/restore する N 個の store/load。
- GC を入れる時、reg slot が tagged か raw int か kind 依存で曖昧 (root 扱い困難)。
- multi-thread になったら破綻。

検証で **撤去した方がベンチも速かった** (ack 0.13 → 0.09, nqueens 0.41 → 0.37 など)。c->reg の cache pressure と save/restore コストが累積していた。

教訓: 「教科書通りの heap-allocated env + leaf には alloca」が clean かつ十分速い。`c->reg` は中途半端な「register file もどき」で、本物の register passing (= dispatcher signature 変更) ほど速くもなく、frame アプローチほどクリーンでもなかった。

### ✅ 型特化 binop (`node_add_int` 等)

`infer_arith_int` で両辺 `int` が確定したら、親ノードの dispatcher + kind を `node_add_int` 等の型特化版に in-place swap (同じ ASTro 機構)。型特化版は冒頭の `OC_IS_INT(av) & OC_IS_INT(bv)` チェックがなく、untag → 加算 → retag だけ。比較も `lt_int` 等で polymorphic `oc_compare` への分岐が消える。

`node_eq_int` / `node_ne_int` は更に `av == bv` の単一 cmp に縮小 (タグ付き fixnum はビット同一なら整数値も同一なので)。

ASTro 機構の利点: node.def に新型を追加しただけで dispatcher / SPECIALIZE / hash / replacer がすべて自動生成、整数値版 SD が AOT で素直に出る。runtime は kind と dispatcher を 2 ポインタ書き換えるだけ。

効果 (warm AOT):
- ack: 0.15 → **0.13** s (eq + sub + add の type-check 全滅)
- nqueens: 0.45 → **0.41** s
- sieve: 0.45 → **0.42** s
- fib(40) で ocamlopt との差: 3.27× → **3.13×**

### ✅ さらに型特化: `if_bool` / `neg_int` / `and_bool` / `or_bool`

型推論で確定したものはどんどん専用ノードへ焼き込む方針。

- `node_if`: cond が bool 確定なら `cv == OC_TRUE ? then : els` の 1 cmp に短縮 (元の `OC_TRUE` / `OC_FALSE` / type_error の 3-way 判定を全廃)
- `node_neg`: 単項マイナス、operand int 確定で type check 削除
- `node_and` / `node_or`: 短絡 + 両 operand bool 確定で per-operand type check 削除

仕掛けは `_int` 系と同じ ASTro dispatcher / kind swap。type infer の各 unify 後に `oc_node_to_*` を呼ぶだけで、AOT codegen は何も知らずに新 dispatcher の SD を吐く。

効果: ack / tak で +5%、fib / nqueens / sieve は影響軽微 (条件分岐は branch predictor が既に効いていた)。コードの clean 化が主目的。

### ✅ (旧) Tiny closure 試み — 撤去済み

ocamlopt の fib body 逆アセンブルを見ると frame allocation 自体が無く、引数は register に乗っている。我々の `oframe` chain は意味的には必要だが、形が単純な closure (= "tiny": leaf, body 全て `lref(0, *)` のみ, 内部 let / match / fun / lazy なし) なら frame そのものを skip して `c->reg[16]` に置けば済む。

実装は ASTro の dispatcher / kind 差し替え機構で:

1. **新 NODE 型 `node_lref0_reg`** を `node.def` に追加 (struct layout は `node_lref` と同一: `{depth, idx}` — depth は読まないが互換性のため残す)
2. **post-parse rewrite**: tiny 判定が通ったクロージャの body を walk し、各 `node_lref` の `head.dispatcher` と `head.kind` を `lref0_reg` のものに in-place swap
3. **2 段階 walker**: 1 回目は mutation なしで全 NODE kind を認識できるか確認 (record / tuple / send 等の variable-arity を含む body は reject)、OK なら 2 回目で実際に rewrite。中途半端な部分 rewrite は orphan `lref0_reg` を残してしまい、oframe path に逃げた時に segfault するので両パス必須
4. **APPN_FAST_PATH と oc_apply の tiny 分岐**: `cache->is_tiny` または `cl->closure.is_tiny` の場合は `c->reg[i] = av[i]` で frame skip して body を直呼び出し
5. **AOT 連携**: kind ごと差し替えたので ASTroGen の specializer も自動的に `EVAL_node_lref0_reg` を呼ぶ SD を生成 → AOT で 1 命令 (`mov 0x80(%rdi), %rax` 相当) になる

効果:
- fib(35): 0.17 → 0.15 s (~12%)
- tak: 0.10 → 0.08 s (~20%)
- fib(40) で ocamlopt との差: 3.85× → **3.27×**

ocamlopt は frame alloc も無く register ABI なので ~3× が AST walker としての残り限界。さらに詰めるには (1) closure value 廃止 + 関数ポインタ ABI、(2) 型推論結果を SD codegen に流して unboxed int 計算、のような **値表現自体の変更** が必要で、これは別の話 (要相談)。

### ✅ Call IC (`node_appN` per-site cache)

`oc_apply` の inline 化後の SD コードを逆アセンブルしたら、関数呼び出しごとに毎回:
- gref IC serial check
- IS_PTR (closure value tag)
- type field == OOBJ_CLOSURE
- nparams 一致
- is_leaf != 0

の 4 段 type chain が走っていた (~10 命令)。

実装: `struct app_cache { VALUE fn; NODE *body; struct oframe *env; }` を `node_app1..4` に `@ref` で追加。hot path は `cache->fn == f` の 1 cmp で全 type check を skip し、`cache->body` / `cache->env` をそのまま使う。

合わせて:
- gcc の **stack-clash protection probe** を `-fno-stack-clash-protection` で外す (alloca 1 回ごとの dead probe loop が消えた)
- `f->nslots` の write を削除 (write-only field; 読み手なし)

効果:
- fib(40) 1.85 s → **1.72 s** (~7% 加速、per-call 6.4 → 5.2 ns)
- 単独だと小さいが、続く最適化の足場として大事 (`oc_apply` 完全 skip + 直接 body 呼び)

ocamlopt との残差は **3.6×**。ocamlopt の fib body 16 命令の内訳 (recursive call 2 つ込み) を見ると、これ以上の縮小は frame 廃止 + register ABI + body inlining が必要で、ASTro 上の AST walker としての limit に達した感。

### ✅ list-heavy ベンチ (sieve / nqueens) の最適化

`perf record` で sieve を見たら `DISPATCH_node_match_arm` 13% + `_int_malloc` 18% が支配的。原因を 4 段で潰した:

1. **`oc_alloc` の calloc → malloc** — `struct oobj` は ~64 byte の union だが、caller は対象の union member 1 つしか触らない。zero-init は無駄。
2. **`oc_cons` 専用の小サイズ alloc** — cons は `{type, head, tail}` 24 byte で十分。union 全体を取らない。
3. **match_arm body の leaf alloca** — `node_is_leaf(body)` で leaf マークし、leaf なら frame を C スタックに alloca。filter 等の per-success malloc を排除。
4. **`node_match_arm` から `@noinline` 削除** — SD が生成され dispatcher indirection が消える (DISPATCH_node_match_arm は perf 上位から消えた)。
5. **cons 専用 bump allocator** — 4MB プールを線形消費、`oc_cons` は `{add, store, store, store}` 程度に。malloc bookkeeping を完全 skip。

効果:

| bench | before | after | speedup |
|--|--|--|--|
| sieve  | 0.90 s | **0.42 s** | 2.1× |
| nqueens| 0.83 s | **0.42 s** | 2.0× |

profile 後 (sieve):
- `_int_malloc` 18% → 9%
- `oc_cons` 10% → 2.4%
- `DISPATCH_node_match_arm` 13% → 圏外
- 上位は SD_xxx (実計算) 中心

ユーザの示唆「機械語に落ちてないんじゃないか」が大当たり — 関数呼び出し dispatcher chain が dispatch indirection + malloc で消えていた。

### ✅ `node_appN` の closure-leaf fast path (oc_apply inline)

`oc_apply` は per-call で:
- `OC_IS_PRIM` 判定
- `OC_IS_CLOSURE` 判定
- `argc < nparams` (partial application) 判定
- frame 確保 (alloca か malloc)
- env save / restore
- `tail_call_pending` 判定
- `argc > nparams` (over-application) 判定

を毎回実行。**fib 級の tight recursive code ではこれが本体計算時間を上回るレベル**。

実装:
- `node_app1` ... `node_app4` の本体に `APPN_FAST_PATH(NP)` macro を inline。
- 受け側が closure かつ matching arity かつ leaf body の場合、`oc_apply` 関数呼び出しを完全 skip して直接 frame 確保 + body dispatcher 呼び出し。
- tail-call escape (body が `tail_call_pending` を立てた) のみ general `oc_apply` にフォールスルーしてトランポリンを使う。

効果 (fib 40、累積比較):

| 実装 | 時間 | per-call |
|--|--|--|
| astocaml -c (fast path 前) | 3.97 s | 12.0 ns |
| astocaml -c (fast path 後) | **2.12 s** | **6.4 ns** |
| ocamlc (BC) | 2.49 s | 7.5 ns |
| ocamlopt (native) | 0.46 s | 1.4 ns |

**astocaml -c が ocamlc bytecode を抜いた**。ocamlopt との差は 8.5× → 4.6× に縮小。

ユーザの直感「call/apply ごとに毎回検索してたりする？」がきっかけで気付いた。直接の検索ではなかったが、oc_apply 内部の type chain check + partial-app 機構の per-call コストがここまで効いていた。

### ✅ AOT specialize の本格活用

実装の鍵は **closure body / pattern arm を AOT エントリとして登録**:
- `SPECIALIZE_node_fun` / `SPECIALIZE_node_match_arm` (および他の `@noinline` ノード) は **空** — 親 SD の中にインライン展開できないので。これにより `let f x = ...` の中身は AOT specialize されないままだった。
- `make_fun` / `make_match_arm` / `make_let_pat` / `make_letrec_n` / `make_try` のラッパを用意し、各クロージャ body / pattern arm body / try body+handler を `AOT_ENTRIES` に登録。
- `maybe_aot_compile` がトップレベルフォームと共に未コンパイルのエントリを全部 `astro_cs_compile` に渡し、`astro_cs_build` で .so を一括生成、`astro_cs_load` で各 NODE の dispatcher を patch。
- `setenv("CCACHE_DISABLE", "1", 1)` で sandbox 環境でも ccache がぶつからないように。

**warm cache での効果** (`./astocaml -c bench/<x>.ml`):

| bench | no-AOT | AOT(warm) | speedup |
|--|--|--|--|
| ack | 0.62 s | 0.21 s | **3.0×** |
| fib | 0.61 s | 0.30 s | **2.0×** |
| tak | 0.46 s | 0.16 s | **2.9×** |
| nqueens | 1.14 s | 0.83 s | 1.4× |
| sieve | 0.98 s | 0.89 s | 1.1× |

公式 OCaml 4.14.1 (interpreter) との比較では:
- fib / tak で **astocaml(-c) が ocaml toplevel より速い**
- ack でほぼ同等
- nqueens / sieve は list 操作が重く、まだ ocaml に大差で負け (3-4×)

cold (build含む) は +100-150ms のオーバーヘッドだが、~1秒以上のスケールでは net win。

### ✅ Closure leaf alloca

実装:
- パース時に各 `node_fun` 本体を walk して、内部に `(node_fun ` または `(node_lazy ` が出現しなければ "leaf" とマーク (dump-grep 方式: 冗長だがミスは安全側に倒れる)。
- `oc_make_closure_ex` で leaf フラグを closure 値に保存。
- `oc_apply` の最初の iteration が leaf closure を呼ぶときは、frame を `alloca` で確保 (oc_apply の C スタック上に乗る)。leaf なので body は新しい closure を作らず、frame は escape しない。
- TCO トランポリン経由 (`goto loop`) の二回目以降の iteration は `first_iter = false` で `malloc` フォールバック (同じ C frame に alloca を積み続けるとスタック爆発)。

効果: fib(35) が 1.65 s → 0.57 s (**2.7× 高速化**)。tak / method_call も 1.5× 程度。
ベンチ全般で frame 確保の `malloc` が完全に消えた点が大きい。

## 試したが効果がなかった / マイナスだった改善

### ✗ パターンマッチで scrut node を共有 (戻した)

実装初期、`build_match_chain` で 1 個の `lref(0,0)` ノードをすべての arm test/body で共有していた。
ASTroGen の生成コードが `parent` ポインタを書き換える可能性があるので、共有していると上書きが発生する潜在バグ。
最終的に **毎回 `ALLOC_node_lref(0, 0)` をフレッシュ**。

### ⚠️ 多相比較演算 (退化、後にファストパスで回復)

最初の Phase 1 では `<` / `>` / `<=` / `>=` は **fixnum 専用**: `OC_INT_VAL(av) < OC_INT_VAL(bv)`。
Phase 2 で float / string / list / variant の比較を要件にしたため、`oc_compare(a, b)` を呼ぶ多相版に置き換え。これで 2× ほど退化。

回復: fixnum インラインファストパスを追加してほぼ Phase 1 並みに戻した。

### ⚠️ トランポリン (TCO 機構) のオーバーヘッド

`oc_apply` のループ化によって、**全ての関数呼び出し** に `c->tail_call_pending` チェックが入る。

緩和策:
- `UNLIKELY(c->tail_call_pending)` で分岐予測ヒント
- 初期エントリは caller の `argv` を直接使い (local copy なし)、tail re-entry のみ local buffer にコピー
- `argc > 16` のケースは recursion fallback

それでも 5-10% 程度の遅延が残る。

### ✅ Boehm libgc 統合 (旧 malloc-leak の解消)

当初は全 alloc を `malloc` 直叩き / 解放なしで、長時間実行で OOM リスクがあった。Boehm libgc を `main.c` に局所 `#define malloc GC_malloc` で全置換 (AST NODE は除く):
- 影響範囲: oobj, oframe (non-leaf), CTX, globals, type checker, partial_state, etc.
- cons cell も per-cell `GC_malloc` (bump allocator は撤去) — Boehm の small-object freelist が再利用するので bump 版より速い
- 1M iter ループで peak RSS 36 MB に収まる
- ベンチ影響: 全般で誤差レベルか改善 (cons-heavy bench で nqueens 0.355 → 0.275, sieve 0.434 → 0.322)

### ✗ Tiny closure + `c->reg` で frame skip (撤去済)

`c->reg[16]` を CTX に置いて tiny closure 引数を入れ、`node_lref0_reg` で読む方針 (ASTro dispatcher swap で in-place rewrite)。35/35 tests pass + ack/tak で 12-20% wins まで持っていったが:

- `c->reg` は CTX 内の共有スクラッチ。CPU register と違い memory access。
- 再帰のたびに save/restore する N 個の store/load。
- GC を入れる時 root 扱いが曖昧 (kind 依存で tagged か raw int かが変わる)。
- multi-thread になったら破綻。

**最終的に撤去した方がベンチも速くなった** (`c->reg` のキャッシュ pressure と save/restore コストが累積していた)。教訓: 「教科書通りの heap-env + leaf には alloca」がクリーンかつ十分速い。

### ✗ 値表現の unbox channel (`_iu` 系) — 撤去済

`int` 確定 subtree を bit pattern として raw int64_t で運ぶ案 (dispatcher signature は変えず、kind ごとの解釈)。`tiny + c->reg` の上に乗せたが、共有 reg が破綻して撤去。

「unbox channel」自体は ASTro 流に実装可能 (frame slot を kind 依存で raw int として読む) だが、frame はそのまま使う前提でやり直す必要あり。要相談 (= 値表現の意味論変更)。

### ✗ `node_appN_direct` (call 先 body を NODE * operand で焼き込み) — 撤去済

`app1(gref X, arg)` を post-parse rewrite で `app1_direct(body_of_X, arg)` に変換。AOT で body の SD が直接 call される (`(*cache->body->head.dispatcher)(c, body)` の indirect 排除)。

- fib 単体: 1.85 → 1.4 s (+20%)
- mutual recursion (cross-block の `let rec` siblings): AOT 整合が壊れて sieve / nqueens で segfault

問題: block N の AOT compile 時に block N+1 の body はまだ rewrite されておらず、SD 内で参照される dispatcher が古い shape を仮定。後で rewrite されても既存 SD は更新されず、ノード kind と SD の前提がずれる。

「全 block を parse して rewrite を済ませてから AOT」の two-pass 化なら直るが、astocaml 全体を二相に変える大改造。AST walker としての streaming model を保ったまま実現する道は見つけられず。

### ✗ `node_if_<cmp>_int` (if + 比較融合) — 撤去済

`if_bool(lt_int(a, b), thn, els)` → `if_lt_int(a, b, thn, els)` で OC_TRUE/OC_FALSE singleton dance を完全排除。fib body の SD は **50 命令 → 28 命令** まで縮んだ (ocamlopt 16 命令)。disasm はキレイになったが:

```asm
SD_fib_body_fused:
    mov  (%rdi), %rax       # c->env
    mov  0x10(%rax), %rax   # n
    cmp  $3, %rax            # n_tag <= 3 (n_int <= 1)
    jle  return_n            # then
    push regs; call SD_fib_n_minus_1
    push call SD_fib_n_minus_2
    sar; sar; add; lea       # untag-add-retag
    pop; ret
```

ところが bench 時間は **fib 0.15 → 0.18 s に regress**。原因不明 (cache pressure? branch prediction? AOT entry の二重登録?)。disasm 上は明らかに改善してるのに runtime が遅くなる現象は再現できたが原因特定できず。撤去。

仮説: 旧 body NODE と新 body NODE の両方が AOT_ENTRIES に残り、新旧両方の SD が `.so` に入って icache が肥大、もしくは `node_fun.body` slot の入れ替えで closure value に反映するタイミングのズレ。

### 構造的限界 (ocamlopt との残差)

ocamlopt fib body は 16 命令。我々の AOT は 28-50 命令。差の正体:

- **値が常に `VALUE` (tagged int64)** で運ばれる → untag/retag が消えない (unbox channel が要る)
- **frame は heap-allocated `oframe` struct** → ocamlopt の register / stack-spill より重い
- **関数呼び出しは `oc_apply` (closure type check) 経由** → ocamlopt の `call <addr>` より長い

これを越えるには **値表現か呼び出し規約のどちらかを ASTro framework レベルで変える** 必要があり、AST walker + AOT specialize モデルの再定義に近い (= 別の framework 拡張、別 session)。

## 今後試す予定

- **環境チェーンキャッシュ** (深い lref のため)
- **クラス間で method table 共有** — `__class_build` を改造して同じシグネチャの methods/fields 配列を共有 (method send IC が cross-instance で効く)
- **AOT specialize の本格活用** — 現状の `--compile` パスは loose; PGC まで入れたい
<!-- Boehm GC: 統合済 (上の done 節参照) -->
- **Tagged float** (Ruby 流の inline flonum encoding) — boxed double の alloc を削減 (※ 値表現の変更は要相談)
- **`OPTIMIZE` の本格化** (hot 検出 → JIT)
