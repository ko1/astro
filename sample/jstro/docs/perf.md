# perf.md — 性能向上の記録

jstro が現在の性能水準に至るまでに試した最適化を、効いたもの/効かなかった
ものの両方を時系列で並べる。各項目に **bench** 列で fib(35) と nbody 100k
への影響を書く (s 単位、best-of-3 のおおよその値)。

ベースの測定環境: Linux x86_64, gcc -O3、SMI fast path 入り、IC 最初期版。
比較対照: `node` (V8 JIT)。

| 改善                                      | fib   | nbody | mandelbrot |
|------------------------------------------|-------|-------|------------|
| 出発点 (-O2, BR チェック有, 単純 IC)        | 0.74  | 0.49  | 0.94       |
| -O3 へ昇格                                | 0.74  | 0.47  | 0.90       |
| arity 特化 call ノード (call0/1/2/3)       | 0.69  | 0.46  | 0.89       |
| flonum-flonum 算術 fast path              | 0.69  | 0.42  | 0.74       |
| compiler flags (no-stack-* / no-plt 等)   | 0.59  | 0.45  | 0.85       |
| call IC + body inline (`jstro_inline_call`) | 0.54  | 0.43  | 0.81       |
| flat n-statement seq (`node_seqn`)        | 0.54  | 0.41  | 0.72       |
| transition IC for member_set (`pre_shape`) | 0.54  | 0.33  | 0.74       |
| **現在**                                  | **0.52** | **0.32** | **0.73** |
| 比較: node.js                              | 0.08  | 0.02  | 0.03       |

下記、各項目を **採用 (○) / 不採用 (✗)** に分けて詳述。

---

## ○ 採用された最適化

### コンパイラフラグ

#### `-fno-stack-protector` / `-fno-stack-clash-protection`

`js_call_func_direct` の `objdump` を見ると、`alloca` の前後で stack-clash
保護のための `sub $0x1000, %rsp; orq $0x0, 0xff8(%rsp)` ループが挿入されて
いた。これはプログラムが大きな alloca で kernel guard page を踏み抜くこと
を防ぐ機構だが、jstro の frame は通常 8〜32 バイト程度なので不要。
`-fno-stack-clash-protection` で除去。同時に `-fno-stack-protector` で
canary も削減。

→ fib 0.74 → 0.61 (-18%)。最大級の単一改善。

#### `-fno-plt`

ELF の手続きリンケージテーブルを経由しない呼び出しに切り替える。
動的ライブラリ呼び出し (libc, libm) が `call ptr@plt` から `call *libc__symbol@GOT(%rip)`
に変わり、PLT スタブの **jmp + jmp** をスキップ。

→ fib 0.61 → 0.59。小さいが確実。

#### `-fno-semantic-interposition`

LD_PRELOAD で実装関数を差し替えできる ELF の互換性を捨てる。gcc がプロセス内
で関数間の **直接参照** を有効化するので、関数ポインタ越しでない直接呼び出し
が `call <local>` に最適化される。

→ fib 0.59 → 0.54 (累計)。

### 値表現側

#### CRuby 風 SMI / inline flonum

最初から採用。fib のような整数演算では heap allocate を完全に回避でき、
gcc は `(v >> 1)` を sign-extending shift 1 命令に落とす。

#### flonum-flonum / SMI-flonum 混在 fast path

mandelbrot のホットループは double 同士の乗算 / 加算が支配的。最初は
`js_to_double` を経由していたが、これは out-of-line 関数呼び出しを伴う。
`node_add` / `node_sub` / `node_mul` / `node_div` / `node_lt` 系すべてに

```c
if (JV_IS_FLONUM(a) && JV_IS_FLONUM(b))
    return JV_DBL(JV_AS_DBL(a) + JV_AS_DBL(b));
if ((JV_IS_SMI(a) && JV_IS_FLONUM(b)) || (JV_IS_FLONUM(a) && JV_IS_SMI(b))) ...
```

を追加。`JV_AS_DBL` も `always_inline` なので gcc が SSE 命令まで畳み込む。

→ mandelbrot 0.85 → 0.74 (-13%)。

### ノード設計

#### arity 特化 call ノード `node_call0/1/2/3`

汎用 `node_call` は引数を `JSTRO_NODE_ARR` (側面配列) に書き出して
`alloca` で staging するが、ほとんどの call は引数 0〜3 個。`node_call1(fn, a0)`
のように **AST node 自身に NODE * 子供として埋め込む** ことで:

- side-array indirection が消える
- alloca buffer がリニア (常に 1 引数なら register に乗る)
- ASTroGen の specialization が child を直接見られる (将来 SD 化したとき)

→ fib 0.74 → 0.69 (-7%)。

#### Monomorphic call IC (`JsCallIC`)

各 call site の AST node に `(cached_fn, cached_body, cached_nlocals)`
の 3 ワード IC を埋め込み、ヒット時に **`js_call`/`js_call_func_direct` を
完全にスキップ** して body の EVAL を直接呼ぶ:

```c
if (JV_IS_PTR(fv) && (JsFunction*)fv == ic->cached_fn) {
    return jstro_inline_call(c, ic->cached_fn, ic->cached_body,
                             ic->cached_nlocals, JV_UNDEFINED, &av0, 1);
}
```

`jstro_inline_call` は `static inline always_inline` なので
dispatcher 関数本体に展開され、frame 確保 → param copy → EVAL → 復帰
までが一つの C 関数の中で完結する。fib のような monomorphic な再帰サイト
では type check + indirect call を完全に外せる。

→ fib 0.69 → 0.54 (-22%)。

#### Transition IC for `member_set` (pre_shape フィールド)

最大の発見。nbody の `function Body(x,y,...) { this.x = x; this.y = y; ... }`
コンストラクタを 5 回呼ぶと、各 `this.x = x` の AST ノードは
**EMPTY_SHAPE → SHAPE_X** の遷移を毎回発生させる。

普通の IC は post-state shape (= SHAPE_X) を覚える。次の呼び出しは
EMPTY_SHAPE で始まるので IC ミス → `js_shape_find_slot` の線形スキャン。

そこで `JsPropIC` に `pre_shape` フィールドを足し、ミス時に
`(pre_shape, shape, slot)` を覚える。次の呼び出しでは

```c
if (o->shape == ic->shape /* in-place */) ...
else if (o->shape == ic->pre_shape) {  // transition hit
    o->shape = (JsShape*)ic->shape;
    o->slots[ic->slot] = v;
}
```

→ nbody 0.43 → 0.33 (-23%)。`js_shape_find_slot` の cycles 9% → ほぼ 0%。

これが現状最もコストパフォーマンスのよい IC 改造。

#### Flat n-statement sequence `node_seqn`

`{ a; b; c; d; e; f; }` のようなブロックは元々

```
seq(seq(seq(seq(seq(a,b),c),d),e),f)
```

の 5 段ネスト。各 seq は dispatcher 1 回 + child 2 回の indirect call なので、
ブロック先頭から末尾まで毎ステートメント間で 1 indirect 余分。

3 文以上のブロックを `node_seqn(idx, count)` という flat 配列ノードに変更。
1 つの dispatcher の中で `for` ループで子を回すので indirect call は子の
分だけ。

→ mandelbrot 0.74 → 0.72。

#### longjmp ベースの throw

最初は `JSTRO_BR != JS_BR_NORMAL` を **すべての算術ノードと property access
ノード** で `EVAL_ARG` の後にチェックしていた:

```c
JsValue a = EVAL_ARG(c, l);
if (JSTRO_UNLIKELY(JSTRO_BR != JS_BR_NORMAL)) return a;
JsValue b = EVAL_ARG(c, r);
if (JSTRO_UNLIKELY(JSTRO_BR != JS_BR_NORMAL)) return b;
```

このチェックは throw 用だったが、(throw が極めて稀なので) ほぼ全パスで
分岐予測が効くにもかかわらず、メモリ load + compare の 2 命令が常に乗る。

`throw` を **`longjmp`** に切り替えれば、実際に throw が起きたときだけ
スタックを巻き戻して try-frame に戻れる。算術ノードからチェックを完全に
削除可能。`break`/`continue`/`return` は静的に loop / function body 境界に
閉じているので、これらは引き続き `JSTRO_BR` グローバルで伝播。

→ 効果は単独では微小だが、後の最適化と相互作用 (生成コードがコンパクト)。

#### 関数宣言ホイスティング (テキスト pre-scan)

仕様準拠の修正だが、これがないと相互再帰が動かない。`{ }` ブロック / 関数
本体の入口で **トークン列ではなくソーステキスト** を depth=0 走査して
`function NAME` パターンを集める。文字列リテラル / 行コメント / ブロック
コメント / テンプレート文字列を skip しつつバイト単位で進める。

トークン化を経由せずバイト走査にしたのは、lexer が状態 (cur, next, tpl_stack 等)
を持っているので一時的に save/restore する仕組みを書くのが面倒だったため。
バイト走査でも要求 (depth-0 の `function NAME` だけ拾う) は十分に達成可能。

ホイスト名を予約してから本パースに入るので、内部の参照が `RES_LOCAL`/
`RES_BOXED`/`RES_UPVAL` のいずれかに正しく解決される。

#### AOT 特化 (SD bake)

各 AST ノードを `astro_code_store` で個別に C ソースに specialize し、
`gcc -O3 -fPIC` で `all.so` にビルド、`dlopen` + `dlsym` で各ノードの
dispatcher を SD に差し替える。`-c` でこの bake を実行 → 即実行、
`--aot-compile` は bake のみ、`--no-compile` は code store を完全に
無視。

設計上のポイント:

- **関数本体の登録**: `parser` の各 `ALLOC_node_func` で
  `code_repo_add(name, body, true)` を呼び、bake walker の entry point
  に追加。これがないと再帰呼出しが host バイナリの `DISPATCH_node_func`
  に戻ってしまう。
- **side-array ノードの bake**: `JSTRO_NODE_ARR` に格納されている
  variadic-operand の子 (call の args、object literal の値、等) は
  ASTroGen の typed-operand specializer が見落とすので、
  `jstro_specialize_side_array` が個別に `astro_cs_compile` する。
- **inner SD wrapper export**: ASTroGen は inner SD を `static inline`
  で吐く (in-source の関数ポインタ鎖を gcc が devirtualize できるように)。
  だが `static` だと `dlsym` で見えないので、`jstro_export_sd_wrappers`
  が SD ファイルを後処理して `SD_<h>` を全部 `SD_<h>_INL` にリネームし、
  末尾に `__attribute__((weak)) RESULT SD_<h>(...) { return SD_<h>_INL(...); }`
  という extern wrapper を append する。これで `dlsym` が全 SD を
  解決できる (luastro 同様)。
- **4 GiB virtual stack worker thread**: 深い AOT-inline 鎖は 1 関数
  呼び出しあたり数百バイト C スタックを食う。`fact(20)` を 5M 回回す
  ような hot loop で 8 MB スタックを食い潰すので、`pthread_create` で
  4 GiB stacksize の worker に切り出して実行。luastro の同名ヘルパを
  そのまま流用。

ベンチ (geo-mean 2.0×):
| bench | plain | -c (bake+run) | speedup |
| - | - | - | - |
| fib(35) | 0.78s | 0.32s | 2.4× |
| fact ×5M | 1.82s | 0.59s | 3.1× |
| sieve | 0.10s | 0.03s | 2.9× |
| mandelbrot(500) | 0.90s | 0.46s | 1.9× |
| nbody 100k | 0.44s | 0.28s | 1.6× |
| binary_trees(15) | 0.68s | 0.56s | 1.2× |

bake コスト自体は ~30-50 ms (SD ファイル数 × gcc 起動)。`-c` と
`-cached` (bake 済みを再起動して dlopen のみ) の差分がそれ。
binary_trees の伸び率が小さいのは、ノード割り当て / GC の比率が高く
SD で削れるのが eval ディスパッチ部分だけだから。

#### JsObject inline 4-slot storage

binary_trees の AOT 結果が plain と大差ないのを `perf` で見たら
malloc/free が 16% を占めていた (ノード `{ l, r, v }` の作成で
JsObject 本体 + slots 配列の 2 つ malloc)。JsObject 構造体に
`JsValue inline_slots[4]` を埋め込み、≤4 props のオブジェクトは
slots を inline_slots に向ける (一切 malloc しない) ように変更。
gc_finalize は `slots != inline_slots` のときだけ free。

ベンチ delta (AOT):
| bench | before | after | speedup |
| - | - | - | - |
| binary_trees | 0.56 s | 0.35 s | -38% |
| fib | 0.32 s | 0.30 s | -7% |
| fact | 0.59 s | 0.53 s | -10% |
| nbody | 0.28 s | 0.25 s | -10% |

#### Safepoint の inline 化

`jstro_gc_safepoint` を関数ポインタ呼び出し (extern) から
`static inline` に変更。fact ×5M の `perf` で 11% を占めていた
(per-statement の関数呼び出し overhead)。fast path は 2 loads +
compare のみ、slow path のみ `js_gc_collect` を out-of-line に残す。

ベンチ delta (AOT):
| bench | before | after | speedup |
| - | - | - | - |
| fact | 0.53 s | 0.48 s | -9% |
| mandelbrot | 0.45 s | 0.39 s | -13% |
| fib | 0.30 s | 0.29 s | -3% |

#### Mark-sweep GC (safepoint 駆動 + frame_stack root)

長時間ベンチでメモリが頭打ちにならない問題を解決するために自動 GC を追加。
設計判断:

- **保守的スキャンではなく明示的ルート**: alloca フレームの C スタック走査は
  ASTro の SD 化と相性が悪い (フレーム境界が消える) ため、`js_frame_link` の
  侵入リストを `c->frame_stack` でつないで明示的に walk する。各 callee は
  alloca した callee_frame と argv をリンクに登録、退出時に外す。
- **セーフポイント方式**: `js_gc_alloc` 内では GC を起動しない。半構築
  オブジェクトが C スタック上にあるタイミングで GC が走ると use-after-free
  になるため、`node_seq` / `node_seqn` / 各種ループの **文境界** で
  `jstro_gc_safepoint(c)` だけが GC を起動できる。
- **arg 評価中の pin**: それでも `new T(f(), g())` のように 1 引数評価が
  recursion を起こす場合、最初の引数値は C スタック上 (argv[0]) にあるが
  どの GC ルートからも参照されない。各 call dispatcher
  (call0/1/2/3 / call / call_spread / new / new_spread / method_call /
  method_call_spread / optional_call) と array/object literal の builder で、
  argv あるいは半構築 array/object を `js_frame_link` を介して pin する。
- **color polarity flip**: 全オブジェクトの mark フラグを 0/1 で持ち、
  GC 終了時に live/dead の意味を入れ替えるだけで sweep 後の reset を省略。
- **動的閾値**: 直前 GC 後の alive オブジェクト数の **4×** を次の閾値に
  (floor 4096 / ceiling 4 MiB)。throughput とメモリ占有のバランスを取る。
  当初 2× だったが mark+sweep が allocation-heavy ベンチで 30% 占めて
  いたので緩めた。
- **`gc_pending` フラグでホットパス短縮**: 当初 safepoint は `(disabled
  && bytes_allocated >= threshold)` の 3 load を毎文判定していた。
  allocator 側で threshold 越えを検知したら `gc_pending = 1` にだけ
  すれば、safepoint hot path は 1 load + 1 branch で済む。
- **sweep フェーズで生存数を同時集計**: rescan pass を排除。

実装後のコスト: `fact` ベンチが 1.49s → 1.73s (16%)。safepoint 自体の
コストは数命令だが、文ごとに条件分岐 1 回が乗る。
binary_trees / 50K 回の depth-8 木生成: RSS 2.8MB で安定 (GC 無しでは線形に
増加してプロセス死)。

#### profile-driven kind swap (`swap_dispatcher`)

luastro 互換の値ドメイン specialization。`node_lt` / `node_le` /
`node_add` が SMI×SMI を観測したら `swap_dispatcher(n,
&kind_node_smi_*_ii)` で kind を昇格。`@canonical=base` 指定で HORG を
共有しているので AOT lookup key は変わらず、HOPT だけが post-swap kind を
反映する。`-p` (PG) bake は HOPT 名で `PGSD_<hopt>.c` を吐き、`(HORG,
file, line) → HOPT` 索引が次回起動時の dlsym を案内する。

ASTroGen 拡張: `jstro_gen.rb` に `:hopt` task を register し、
`build_hopt_func` で `HOPT_<name>` 関数を `node_hopt.c` に自動生成。
node.h に `kind_swapped` フラグと `swap_dispatcher` inline ヘルパを追加。

純粋な perf 改善はそこまで大きくない (canonical SD 側も SMI fast path
を持っていて gcc が同じくらい良いコードを出すため)。本来の効果は
**PG bake が specialized SD を出すこと自体** にあるので、profile が当たる
パターンでより伸びる。

#### fused int-counter for ループ (`node_for_int_loop`)

parser-time に検出可能な極めて頻出するイディオム:
```js
for (var/let X = INIT; X </<= END; X++/X+=k) BODY
```
を fused dispatcher に置換。条件: `X` は新規宣言の local、body が `X` を
書き換えない (構文走査で確認)。

eval は init/end/step を 1 回だけ評価して、X を C 関数の `int64_t i`
ローカル変数として保持する。各反復:
1. `frame[X_slot] = JV_INT(i)` (body 用に書き戻し)
2. `EVAL(body)`
3. `i += step`

これで cond eval / step eval が消える (毎反復の SMI tag check + decode/
encode が削減)。type assumption 違反時は fallback path で素直に `js_to_double`
経由で動作。

ベンチ delta:
| bench | before | after | speedup |
| - | - | - | - |
| sieve(1M) | 0.020 s | 0.012 s | -40% |
| fact ×5M | 0.49 s | 0.31 s | -37% |

#### Map / Set のハッシュテーブル化 (300× speedup)

旧実装は `JsMapEntry { k, v, used }[]` の linear scan。要素 N 個の Map に
対する `get(k)` / `set(k, v)` が O(N)、N 増加で N×N 操作。`map_coll`
ベンチ (100K キーの set+get) が 25.5 s かかっていた。

新実装は **挿入順 entries[] + power-of-two index[] の二段構成**:
- `entries[]` は (k, v, hash, used) を挿入順に積む。iteration はここを
  頭から走査、`!used` を skip — JS Map 仕様の挿入順 iteration が保たれる。
- `index[]` は hash → entries[] index のハッシュ表 (-1=empty, -2=tomb)。
  `(hash & mask)` から線形プローブ。
- `jsval_hash` は SameValueZero 準拠 (interned string は precomputed
  `s->hash`、+0/-0 同一視、NaN を固定スロット)。
- load > 0.75 で index_capa 倍増、tombstone が 1/3 越えで entries[]
  compact。

結果: `map_coll` 25.5 s → 0.082 s (**300× speedup**)、node v18 0.06 s
に対して **1.3× 後ろまで** 詰まった。残るのは string intern オーバヘッド
(25% of cycles)。

#### node_index_set のインライン dense 配列パス

`a[j] = false` の hot loop で `js_array_set` への out-of-line 関数
呼び出しが 16% を占めていた。SMI index + array fast path を SD 内に
直接インライン展開:

```c
if (JV_IS_ARRAY(ov) && JV_IS_SMI(iv)) {
    int64_t i = JV_AS_SMI(iv);
    struct JsArray *a = JV_AS_ARRAY(ov);
    if (i >= 0 && (uint64_t)i < a->dense_capa) {
        a->dense[i] = v;
        if (i+1 > a->length) a->length = i+1;
        return RESULT_OK(v);
    }
    ...
}
```

sieve / state など array-write heavy なワークロードで 15% 改善。

#### encoded-form SMI compare

`node_lt` / `node_le` / `node_gt` / `node_ge` の SMI×SMI fast path で
encoded JsValue を直接 signed compare:

```c
// encoded SMI: (raw<<1) | 1 — 両方の low bit が 1 なら signed 大小比較が
// raw 値の大小と一致する。SAR デコード 2 つを省略。
if ((a & b & 1) != 0) return JV_BOOL((int64_t)a <= (int64_t)b);
```

combined-AND の SMI tag 判定 (`(a & b) & 1`) で 2 つの分岐を 1 つに
統合。sieve の hot loop で per-iter 2-3 命令削減。

#### node_for / node_while から redundant BR チェックを除去

init / cond / step は **式** で `break` / `continue` / `return` (文限定)
は出ない、`throw` は longjmp 経路で BR を経由しない。よって EVAL_ARG
直後の `if (BR != NORMAL)` は body 後を除いて常に false。perf で見ると
この BR 再ロードが sieve hot SD の 14% を占めていた。

---

## ✗ 試したが効かなかった最適化

### `-flto` (Link-Time Optimization)

理論上、関数ポインタ越しの呼び出しを直接呼び出しに promote できる場合が
ある (devirtualization)。実測ではほぼ無効果かわずかに悪化。dispatcher
の関数ポインタが「`n->head.dispatcher` を indirect 呼び出し」という形
なので、LTO でも静的に解析しきれない。

→ fib 0.69 → 0.79 (悪化)。LTO は外した。

### `-march=native`

AVX-512 のアラインメント要求が `alloca` バッファに合わず、
prologue が増えた。

→ fib 0.69 → 0.78 (悪化)。`-march=native` も外した。

### PGO (`-fprofile-generate` → `-fprofile-use`)

意外と効かなかった (2-5%)。すでに `__builtin_expect` で hot/cold が明示
されており、indirect call の予測は CPU 側で十分。バイナリサイズと
ビルド時間の悪化を考えると割に合わない。

### `EVAL` をマクロ化

`EVAL(c, n, frame)` を `((*n->head.dispatcher)(c, n, frame))` のマクロに
すれば 1 関数呼び出し分 (関数ポインタ load + indirect call) を hot path
内にインライン化できる気がした。実際は gcc が既に `EVAL` を inline していた
(static inline 化していた等)、結果はノイズの範囲。

### `__attribute__((flatten))` を全 dispatcher に

dispatcher 関数を flatten すると child の `EVAL_*` も全部 inline されて
1 つの巨大関数になる。実装量は多い。フレームワークが既に always_inline で
EVAL_* を内側に展開しているので、flatten 追加では大して動かなかった。

### computed-goto threaded interpreter

「dispatcher を関数ポインタにせず、ラベルアドレスにして goto する」典型的
なバイトコード VM 最適化。AST ベースの jstro では各ノードが構造的に独立
した「subtree-evaluating function」なので素直には適用できない。
`EVAL` 全体を 1 つの大 switch 文 にする方向もあるが、ASTroGen の生成コード
を大幅に書き換える必要がある。

### js_call_func_direct を inline 化 (常に)

`js_call_func_direct` を `static inline always_inline` にして全呼び出しを
インライン化する案。`alloca` を含む関数を inline すると caller 関数が
大きく膨れて register pressure が上がり、結果としてホットループ全体が
遅くなった。**monomorphic call IC のヒット時のみ** inline する現在の方式
(`jstro_inline_call`) が最適だった。

### `arena` allocator

文字列連結 (`js_str_concat`) や Map のエントリ拡張など、短寿命の小さな
allocation をまとめて arena に流せばメモリ確保コストを削れる。実装の
複雑性に見合うほどの効果が出なかった。GC を入れるなら本質的にこの方向に
進むので、後回し。

### Specialized comparison ノード `node_smi_lt` 等

「parser の段階で SMI 比較と確信できるなら専用ノードを emit する」型推論。
parser 側に型推論が必要 + 失敗時に general path に戻す guard が必要 +
ASTro が既に SMI fast path を inline しているので、ノードを増やしても
gcc が同じコードに落ちる。

---

## 機能拡張ラウンド後の状態

ES2023+ サブセットの大幅な追加 (destructuring, class extends/super, getter/setter,
Promise/async, regex, Proxy/Reflect, Map/Set, modules, など) を行ったが、
ベンチマーク数値はほぼ変化なし:

| benchmark         | 機能拡張前 | 機能拡張後 | 変化 |
|-------------------|-----------|-----------|------|
| fib(35)           | 0.52 s    | 0.55 s    | +6%  |
| fact ×5M          | 1.30 s    | 1.40 s    | +8%  |
| sieve(1M)         | 0.08 s    | 0.09 s    | ~    |
| mandelbrot(500)   | 0.74 s    | 0.79 s    | +7%  |
| nbody 100k        | 0.32 s    | 0.38 s    | +19% |
| binary_trees(14)  | 0.47 s    | 0.49 s    | +4%  |

nbody が一番悪化したのは `js_set_member` / `js_object_set_with_ic` に
freeze / NOT_EXTENS / accessor のチェックが入ったため。これは fast path
には乗らないが、IC 1 段目 (in-place update) の前段で `gc.flags` をチェック
する追加のロード+分岐が定常時にも入る。完全 monomorphic ループでは
ほぼ予測できるので影響は限定的。

`node_member_get` / `node_member_set` の IC fast path は HOLE / accessor
の場合に slow path に落ちるよう変更したが、通常のオブジェクト操作 (data
property 一辺倒) では追加チェック 1 件で済む。

## 振り返り

最も効いた最適化を時系列で並べると:

1. **シェイプ遷移 IC** + **monomorphic call IC + inline body** — 多形性
   が低いケースの最速パスを切る (V8 の発見の jstro 版)
2. **AOT 特化 (SD bake)** — `astro_code_store` を駆動して各 AST ノード
   を `SD_<hash>` に specialize、dispatcher を patch (geo-mean 2× 越え)
3. **JsObject inline 4-slot 内蔵** — 小オブジェクトの malloc を 1 個減
   らす。binary_trees / state で大きく効く
4. **Map / Set のハッシュテーブル化** — linear scan を open-addressing
   ハッシュ + 挿入順 entries[] に置き換え。`map_coll` で 300× speedup
5. **fused int-counter for ループ (`node_for_int_loop`)** — parser-time
   に頻出 idiom を検出して raw int64 カウンタの fused dispatcher に置換
6. **safepoint inline + `gc_pending` フラグ** — per-statement 関数呼び
   出しを 1 load + 1 branch に短縮
7. **profile-driven kind swap** — luastro 互換の値ドメイン specialization

逆に「全ケースを一律に速くしようとした」最適化 (LTO、march=native、
flatten) はあまり効かなかった。JS のような動的言語では、ホットスポットの
**type 多形性は実は低い** という V8 の発見が裏付けられている。

## 現状のベンチ位置 (jstro-cached vs node v18)

| カテゴリ | bench | jstro | node | 結果 |
| - | - | - | - | - |
| **Win** | sieve_big (40M) | 1.09 | 2.67 | **2.45× ahead** |
| | try_catch | 0.017 | 0.78 | **45× ahead** |
| | cold (1000-fn × 1) | 0.015 | 0.82 | **53× ahead** |
| | state (Redux spread) | 2.27 | 5.11 | **2.25× ahead** |
| | sieve (1M) | 0.012 | 0.014 | **1.17× ahead** |
| **Close** | map_coll | 0.063 | 0.047 | 1.34× behind (was 432×) |
| **Lose** | regex | 0.011 | 0.004 | 2.75× behind |
| | poly | 0.097 | 0.022 | 4.4× behind |
| | fib(35) | 0.31 | 0.10 | 3.0× behind |
| | fact ×5M | 0.31 | 0.07 | 4.6× behind |
| | binary_trees | 0.31 | 0.06 | 5.7× behind |
| | mandelbrot(500) | 0.39 | 0.04 | 11× behind |
| | nbody 100k | 0.25 | 0.02 | 14× behind |

勝ちは **V8 の tier-up エッジ** (cold), **deopt しがちな idiom** (try_catch
/ state), **構造的な差** (sieve_big の flat layout)。負けは **TurboFan の
数値最適化** と **専用エンジン** (Irregexp, polymorphic IC)。

## 次に取りに行ける gain (詳細は [`todo.md`](./todo.md))

これ以上の gain はアーキテクチャ級の改修になるので、それぞれ独立した
プロジェクト扱い:

1. **Generational GC** — binary_trees / state など allocation-heavy が
   30% を mark+sweep に取られている。nursery + write barrier で
   ~500-1000 行、1 週間程度。
2. **Escape analysis / 型推論** — mandelbrot / nbody の box/unbox
   除去。whole-method 解析が前提なので数週間。
3. **多形性 IC (4-way)** — `poly.js` の 4.4× を縮める。実装は中規模。
4. **regex JIT** — Irregexp 相当の native コード生成 or Onigmo 組み込み。
5. **call frame の register-pass 化** — `jstro_inline_call` の alloca +
   memcpy + CTX field save/restore を register ABI に置換。fib / fact
   recursion で効くはず。
3. **method call IC** — 現在は `member_get` の IC + `js_call` の IC を 2 段
   に分けて持つ。`obj.foo()` の AST node に `(shape, slot, cached_fn, cached_body)`
   を 1 セットで持てば直結できる。
