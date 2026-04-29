# wastro 性能チャレンジ記

WebAssembly 1.0 を ASTro フレームワーク上に実装した wastro の視点から、
**「型付き SSA-like な言語をツリーウォークで走らせて native に近づけるには
何が効くか」** を整理する。

本ドキュメントは ascheme / castro の perf.md と同じ位置付けで、
プロジェクト横断の知見は `docs/perf.md` (リポジトリ ルート) にある。
ここでは **wastro 固有のチャレンジと得られた知見** を残す。

## 0. 全体像と現在地

- 入力: WebAssembly 1.0 (MVP) の `.wat` / `.wasm`。型は `i32 / i64 / f32 / f64`、
  すべて `VALUE = uint64_t` 一つに収納し、各 use-site の `AS_*/FROM_*` で
  reinterpret する。
- ターゲット: tree-walking 部分評価器による AOT。SD\_<hash> 関数を
  `code_store/all.so` にビルドして dlopen する。
- 比較対象: native (`gcc -O2 / -O3`) と wasmtime (Cranelift JIT)。
- 現状: AOT cached モードで native 比 **3〜6×**、wasmtime 比は
  ループ系で互角〜勝ち、call-heavy (fib) で負ける、という形勢。
  詳細は `bench.rb` の出力を参照。

| Workload (cached SD\_) | wastro | native | ratio |
|---|---|---|---|
| `fib(34)` — recursive | 54 ms | 9 ms | **6×** |
| `sum(1_000_000)` — i32 loop | 3 ms | 1 ms | **3×** |
| `sieve(1_000_000)` — i32 loop + linear memory | 9 ms | 3 ms | **3×** |

最大のボトルネックは **再帰の境界に残る間接呼び出し**（fib で顕著）。
これは ASTro framework 側の Phase 3 (depth-bounded recursive specializer)
が必要で、wastro 単体ではこれ以上削れない。

## 1. RESULT-pair 返り値で setjmp を hot path から追い出す

**問題**: wasm の構造化制御は `block / loop / if / br N` で、`br N` は
ネストした N 段ぶんの label を一気に飛び越える。素直に書くと
`setjmp/longjmp` を hot path に置きたくなる（旧版の interpreter は実際そう
していた）。が、`setjmp` は GCC SROA を分断するので AOT 化しても遅い。

**解**: 全 dispatcher が `RESULT { VALUE value; uint32_t br_depth; }`
（12B、`rax:rdx` 二本に乗る）を返す。`br_depth == 0` なら正常値、`> 0` なら
脱出中、`WASTRO_BR_RETURN` (`0xFFFFFFFF`) は関数境界で消費する return
sentinel。`UNWRAP` マクロが各 child 評価の戻り値を即チェックして
脱出を上方へ伝播させる:

```c
#define UNWRAP(r) ({                                  \
    RESULT _r = (r);                                  \
    if (__builtin_expect(_r.br_depth != 0, 0))        \
        return _r;          /* propagate to caller */ \
    _r.value;                                         \
})
```

**効果**: hot path から `setjmp` が消える。`block / loop / if` は
`if (--r.br_depth == 0) return RESULT_OK(r.value); return r;` の
3 行ですべての label 解消が書ける（`node.def:1273-1297`）。

**教訓** (一般化): 多段 break/escape を持つ言語（wasm の `br N`、Scheme の
named-let、Ruby の break）は **caller 側で消費する depth カウンタ** に
潜伏させると `setjmp` を排除できる。castro/abruby の RESULT パターンと
同根。`br_depth` の代わりに `escape_target` ポインタを返す形でも同じ。

**コスト**: leaf node が **常に** 12B の RESULT を返すので、
no-branch ノードでも 1 つ余計な命令（`xor edx,edx`）が出る。ただし
register pair なのでメモリは触らない。castro perf.md §10 でも記述ある通り
loop_sum 系で局所的に -50% な悪化を見ることもある — wastro は br_depth が
ほぼ常に 0 で予測がよく当たるためか観測されていない。

## 2. SIGSEGV-as-bounds-check で load/store を 1 命令化

**問題**: wasm のメモリアクセスは「`addr + offset` が現在ページ範囲内か」を
**毎回** チェックして OOB なら trap、というのが仕様。素朴にやると
`if (a + n > c->memory_size_bytes) trap();` を全 load/store に挿入することに
なり、AOT で SD\_ が肥大化して inline の機会も減る。

**解** (`main.c:80-133`): モジュール instantiation 時に **8 GB の仮想空間**
を `mmap(MAP_ANONYMOUS, PROT_NONE)` で予約し、ライブな部分だけ `mprotect`
で `R/W` に上げる。OOB なアドレスは PROT_NONE 領域に当たって SIGSEGV を
発生させるので、SIGSEGV ハンドラが `wastro_trap("out of bounds")` に
変換する。

```c
// load の本体は 1 命令で済む
NODE_DEF
node_i32_load(CTX *c, NODE *n, slot *frame, uint32_t offset, NODE *addr)
{
    uint64_t a = (uint64_t)AS_U32(UNWRAP(EVAL_ARG(c, addr))) + (uint64_t)offset;
    uint32_t v; memcpy(&v, c->memory + a, 4);
    return RESULT_OK(FROM_U32(v));
}
```

**効果**: AOT 後の load/store が単一の `mov` (+ アドレス計算) に縮む。
sieve / heapsort / mandelbrot のメモリ重 bench はこれで wasmtime と同等
レンジに乗る。

**教訓** (一般化): wasm 1.0 のように **アドレスの上限が分かっている**
（u32 + u32 offset で 33-bit）言語であれば、最大可能アドレスを覆う仮想予約
+ 外周 PROT_NONE が成立する。Lua のテーブル / Ruby の Array のように
**任意拡張の動的データ構造** には適用しづらい点に注意。

**副作用**:
- SIGSEGV ハンドラはプロセスグローバル → wastro は single-CTX 前提
  (`wastro_segv_ctx`)。マルチスレッド対応するなら `siginfo_t::si_addr` で
  CTX を解決する map が必要。
- `--test` モードの `assert_trap` が SIGSEGV → setjmp 経路を要するので、
  ハンドラに「アクティブな setjmp があれば longjmp、なければ exit」の
  二段切替を入れている。`SA_NODEFER` 必須 (longjmp 後の signal mask 復帰
  問題を避けるため)。

## 3. `union wastro_slot` で型情報を gcc に渡す

**問題**: wasm の locals は型付き (`i32/i64/f32/f64`) だが、frame は単一の
flat array にしたい（VLA で確保するため）。素朴に `uint64_t frame[]` で
持つと、`local.get/set` のたびに reinterpret が必要で、特に float が
xmm に乗らずメモリトラフィックが残る。

**解** (`context.h:51`):

```c
union wastro_slot {
    int32_t  i32;
    int64_t  i64;
    float    f32;
    double   f64;
    uint64_t raw;
};
```

各 `local.get_<T>` / `local.set_<T>` は `frame[i].<T>` で対応するメンバを
読み書きする。型ごとに別ノード kind (`node_local_get_i32`,
`node_local_get_f64`, ...) に分けてあるので gcc は **各アクセス時点で
slot の C 型を知っている**。

**効果**: SD\_ の鎖が一直線に inline されると、gcc は frame slot を
union 型で見ているので **SROA が個別に動作** する。mandelbrot の内側
loop で `zx / zy / zx2 / zy2 / iter` が xmm/gpr に同時に乗る。

```asm
;; AOT-cached mandelbrot inner loop (抜粋)
;;   iter (i32) は %r12d, zx/zy (f64) は %xmm0/%xmm1 上
.Lloop:
    mulsd  %xmm0, %xmm2        ;; zx2 = zx*zx
    mulsd  %xmm1, %xmm3        ;; zy2 = zy*zy
    ...
```

**教訓** (一般化):
- frame を **union メンバアクセス** で表現する＋型ごとに別ノード kind を
  立てると、ASTroGen が生成する SD\_ が各 slot を C 型レベルで扱う。
- 単純な `uint64_t[]` + `memcpy` reinterpret でも同等になりそうに見えるが、
  実際には gcc が **alias 解析**で躊躇うことがある。union で書くと alias
  ルールに沿うので確実。
- 動的型言語（Lua / Scheme）では type-check が伴うのでこの単純化は
  不可能。**静的型ありの言語サンプル特有の勝ち筋**。

## 4. fixed-arity 0..4 + var-arity フォールバックで可変引数を分離

**問題**: ASTroGen の `NODE_DEF` は **固定形** struct を生成する仕組みなので、
0..N の任意引数を持つ wasm `(call ...)` を一つの kind で表現できない。
全引数本数ぶんの kind を立てると、wasm 1.0 の上限 1024 まで効くようにすると
1024 個 kind になりハッシュ空間が爆発する。

**解** (`node.def:1672-1765`, `1968-2032`):

| Node | Arity | 仕組み |
|---|---|---|
| `node_call_0..4` | 0..4 | args を declared `NODE *` 子として受ける → sibling-dispatcher inline |
| `node_call_var`  | 5..1024 | `(args_index, args_cnt)` で module-global flat 配列 `WASTRO_CALL_ARGS[]` を参照 |

実モジュールはほぼ 0..4 引数で覆える（emcc / rustc 出力をスキャンして検証）。
fast tier に入った call は SD 化で sibling 鎖が collapse する。

**教訓** (一般化): 可変要素を持つノード（call, br_table, switch, format args）は
**fixed bucket + 共有 side array** が標準パターン。ハッシュは
`(index, count)` だけで済むので code store の dedup が効く。luastro の
`LUASTRO_NODE_ARR`、wasm の `WASTRO_BR_TABLE` も同パターン。

**注意点**:
- var-arity 経路は子の dispatcher を `arg->head.dispatcher` 経由で間接呼びするので
  sibling-inline の機会を失う。call boundary で SD 鎖が一旦切れる。
- 0..4 を超える wasm モジュールは稀だが、**ここに本当に到達したら遅い** ので
  ベンチには載せない。

## 5. `@always_inline` をどこに付けるか — 制御フローノードに必要

**問題**: 初期実装では `node_loop` を素直に書いていたが、mandelbrot で
特化後の SD\_ を逆アセンブルすると `EVAL_node_loop.isra.0` が **out-of-line
コピー** で残っていて、内側 loop 越しの SROA が断たれていた。

**原因**: gcc -O3 のインライナはコード長と呼び出し回数のヒューリスティクスで
判断するが、`for(;;)` を抱える `node_loop` は本体長＋エイリアスの曖昧さで
inline 候補から落とされる。

**解**: 制御フロー系 (`if / seq / block / loop / br / br_if`) に
`@always_inline` を付ける (`node.def:1250 直下` のコメント参照)。

**効果**: mandelbrot の内側 loop の frame slot が SROA でレジスタ化される。
具体的には `iter` カウンタと escape 判定の `zx2 + zy2` 比較が同 SD 内に展開され、
loop body が xmm/gpr の純粋オペレーションだけで回る。

**教訓** (一般化): `@always_inline` は **小さい leaf** だけでなく、
**rare-but-essential な制御フローノード** にも必要。ヒューリスティクス任せに
すると、loop / branch 越しの SROA が fragile になる。
ascheme `perf.md §14`（`scm_apply_tail` への always_inline）と同根の話。

**反例**: 全 NODE に `@always_inline` を付けると inline が爆発して
SD\_ のコードサイズが膨張する。**制御フロー＋小型ノード** に絞るのが定石。
`@noinline` を全 call に付けるのもアンチパターン (ascheme `perf.md §4`)。

## 6. `static inline EVAL` で `.so` 境界の PLT bounce を避ける

**問題**: SD\_ から子のディスパッチャを呼ぶ経路は本来
`(*n->head.dispatcher)(c, n, frame)` で、ホスト binary の `EVAL` 関数を
呼ぶ。`EVAL` がホスト binary 内のシンボルだと、`all.so` 内の SD\_ から
**PLT 越しの間接呼び**になり、毎ノードで余計な jump が一段増える。

**解** (`node.h:74` の `EVAL` を `static inline`):

```c
static inline RESULT
EVAL(CTX *c, NODE *n, union wastro_slot *frame)
{
    return (*n->head.dispatcher)(c, n, frame);
}
```

加えてホスト側を `-rdynamic` でリンクし、`all.so` から `WASTRO_FUNCS` /
`WASTRO_GLOBALS` 等のグローバルが解決できるようにする (`Makefile:23`)。

**効果**: SD\_ → EVAL → 子 SD\_ の鎖が **`.so` 境界を一度も跨がない**
直接呼びに collapse する。

**教訓** (一般化): ホスト binary と `code_store/all.so` が **対称的に** 互いの
シンボルを参照する状況では、トランポリン関数を `static inline` で
ヘッダに置く＋ホストを `-rdynamic` でリンクする、の二点セットが必須。
castro `perf.md §2`、`docs/perf.md §6` でも触れられている。

**未着手**: `code_store/Makefile` 側に `-Wl,-Bsymbolic` を追加すれば
`all.so` 内シンボル参照も GOT 経由を避けられる (castro `perf.md §7`)。
wastro はまだ direct cross-SD call を発行していないので恩恵は限定的だが、
将来 Phase 3 (recursive specialization) を入れたら必須になる。

## 7. body slot を `NODE *` operand にする — sibling-inline と forward-ref の戦い

**問題**: `(call $f arg)` の callee body を **`NODE *body` operand として
node_call_N に持たせる** ことで、ASTroGen の sibling-dispatcher inline が
caller → callee body へ recurse できる。が、call を allocate する時点で
callee の body はまだ parse されていないことがある（前方参照、相互再帰）。

**解** (`main.c:194-240`):

1. ALLOC 時には body = NULL のままノードを作成。
2. `(func_index, arity, call_node)` を `PENDING_CALL_BODY[]` に積む。
3. 全 (func) parse 完了後 `wastro_fixup_call_bodies()` で全 call の body slot を
   `WASTRO_FUNCS[idx].body` に書き込む。
4. その後 `OPTIMIZE` (= `astro_cs_load`) を全 body のルートに対して呼ぶ。

**効果**: caller SD\_ から callee body の SD\_ への直接呼びが成立。
特化済みなら C コール 1 発、未特化でも `body->head.dispatcher` 1 段間接。

**教訓** (一般化):
- forward-ref を許す言語 (C, Ruby, JS, wasm) では parser 後処理での
  patch-up は必須。AST allocate と OPTIMIZE はフェーズを分ける。
- 「ハッシュは parse 直後に確定する」前提 (`docs/perf.md §4.5`) と
  「parser 後処理で kind/operand を書き換える」が **衝突する** ので注意。
  wastro では ALLOC で `OPTIMIZE` を呼ばず、 fixup 後に明示的に
  `astro_cs_load` を回している。
- 再帰サイクル (`call_N(body=自己)`) は ASTro framework の
  `is_specializing` フラグが破断点として処理する。閉口側のエッジは
  間接呼びに残るので、これが fib の 6× ギャップの主要因。

## 8. 共通パラメータ拡張 — 全 EVAL に `frame` を 3 番目で通す

**問題**: locals は呼び出しごとに変わるので「現在の frame ポインタ」を
EVAL のどこからでも触れる必要がある。CTX 経由 (`c->fp[]`) で渡すと
load/store にメモリ間接が 1 段乗る。

**解** (`wastro_gen.rb`): ASTroGen の `common_param_count = 3` を override し、
全 EVAL/DISPATCH の引数に `(CTX *c, NODE *n, union wastro_slot *frame)` の
**3 引数共通プレフィックス** を強制する。`local.get/set` は `frame[i].<T>` で
直接アクセス。

**効果**: frame は **レジスタで持ち回り**、CTX へのストアが消える。
wasm の locals アクセスは ASTroGen 標準の 2 引数 (c, n) より明らかに
早い。**castro / luastro と完全に同じパターン**で、wastro でも全面採用。

**教訓** (一般化): EVAL 引数は **「全ノードが頻繁に触るが call frame で
変わるもの」** を優先して足すべき。CTX フィールド経由よりレジスタ経由の
ほうが速い。**逆に**「頻繁に触らない」ものは CTX のままで OK
（wastro なら memory ポインタは CTX に残してある）。

## 9. memory.grow を mprotect で済ませる

**問題**: `memory.grow` は wasm のメモリを動的に拡張する命令。素朴な実装だと
`realloc` 相当が必要で、ホストが保持する `c->memory` ポインタが移動する
可能性があり、imported host functions が握ったポインタが invalidate される。

**解**: instantiation で 8 GB を `PROT_NONE` で予約済みなので、`memory.grow`
は **既存の reservation 内で次のページを mprotect で R/W に上げるだけ**
(`node.def:1599-1619`)。`c->memory` は不変、新ページは kernel が lazy zero-fill。

```c
NODE_DEF
node_memory_grow(...)
{
    uint64_t new_pages = c->memory_pages + d;
    size_t add_bytes = (new_pages - c->memory_pages) * WASTRO_PAGE_SIZE;
    mprotect(c->memory + old_bytes, add_bytes, PROT_READ | PROT_WRITE);
    ...
}
```

**効果**: `memory.grow` がほぼ free（syscall 1 つ + キャッシュ更新）。
JIT 系の wasmtime も同じ方針。

**教訓** (一般化): 大きな仮想予約が許されるなら、**動的拡張は mprotect
ベース**で「ベースアドレス不変」の保証を実装で追加コストなく担保できる。
ポインタ invalidation 問題が消えると C との FFI が大幅にシンプルになる。

## 10. 部分評価フレームワーク任せで OK な箇所

ここまでの 1〜9 はすべて **言語実装側（wastro）の選択**だが、実は
wastro が出している数字の **大半は ASTro framework 標準の特化** で出ている。

明示的に「やっていない」最適化:

| やってない最適化 | 理由 |
|---|---|
| プロファイル駆動の型推測 SD | wasm は静的型なので不要 |
| インラインキャッシュ (`@ref`) | wasm に動的ディスパッチがない |
| 数値タワー / box 化 | i32/i64/f32/f64 で FIX、box 不要 |
| ローカル定数畳み込み | wasm はすでに `i32.const N` で表現済み |
| ループ融合（fused for） | wasm は `loop` + `br_if` の generic 形のみ |
| Tail call → goto | wasm 1.0 に末尾呼び出し最適化なし |
| 自前 frame 再利用 (leaf closure alloca) | VLA 一発で C スタックに乗るので不要 |

つまり **wasm の言語 semantics が静的すぎて打ち手の余地が少ない** ため、
parser-pass 系の最適化は他のサンプル（luastro / ascheme / castro）ほど
入っていない。これは「framework 標準の SD\_ specialization が wasm では
直接効きやすい」と言い換えてよい。

**教訓** (一般化): 言語が静的型 + linear control flow なら、framework の
標準特化だけで native の 3× は取れる。3× を切るには **call boundary**
を framework 側で消す必要があり、これは wastro 単独で解けない。

## 11. 残課題と未踏

### 11.1 再帰境界の間接呼び（fib 6×）

wasm `call $fib` は `node_call_1(fib_idx, body=fib_body, a0=...)` に化ける。
caller SD\_ は body slot 経由で fib body の SD\_ を直接呼びたいが、
**body は自分自身** なので `is_specializing` フラグでサイクルが破断され、
最深 1 段だけ `body->head.dispatcher` 間接呼びが残る。これが fib のオーバ
ヘッドの大半。

**解候補** (= ASTro framework Phase 3):
- depth-bounded recursive specializer。N (e.g. 4) 段ぶんの caller SD\_ を
  「自身を直接呼ぶ」コードで unroll する。リーフは `is_specializing` で
  break するが、それ以外の N-1 段はフルインライン。
- SCC-aware hashing (`docs/todo.md` 末尾)。サイクル全体を 1 つの fixpoint
  hash + 単一 SD\_ で扱う。fib なら自己 SD が自分を direct call する。

### 11.2 typed dispatcher signature (Phase 4)

現在は全 SD\_ が `RESULT (CTX*, NODE*, slot*)` の **同一シグネチャ**。
小さな leaf でも 12B `RESULT` を返すコストがある。実関数は型を持っているので、
**特化された SD\_ は引数/返り値を C 型 (`int32_t / double / ...`) で受け取れる**
方が望ましい。loop ヘビー bench (sum / sieve) で残っている 2-3× ギャップは
おそらくここ。

### 11.3 var-arity call の SD 焼き込み

`node_call_var` は子を `WASTRO_CALL_ARGS[]` 経由で間接呼び。1024 引数まで対応する
代償として sibling-inline を失う。実コードでこの経路に落ちるのは稀だが、
embind / autogen bridge では発生する。luastro の
`luastro_specialize_side_array` 相当の post-process があれば SD で
潰せる (`docs/perf.md §4.4`)。

### 11.4 SIGSEGV ハンドラのマルチスレッド化

現状 single-CTX。複数モジュールを別スレッドで走らせるなら `siginfo_t::si_addr`
で CTX を解決する map が要る。

## 12. 教訓まとめ（言語非依存）

wastro で確認できた **「ASTro framework 任せで他言語にも汎用」な事実**:

1. **静的型言語は parser-pass 最適化の余地が小さい**。framework の SD\_
   特化だけで native 3× に到達する。
2. **RESULT-pair 返り値による多段脱出**は setjmp 不要にできる。
   wasm 以外でも break N / 例外/早期 return / non-local exit を持つ言語
   全般に効く。
3. **SIGSEGV-as-bounds-check** は最大可能アドレスが静的に分かる言語で
   効く（wasm / メモリ安全な静的言語の bounds check）。
4. **`union slot` で frame を表現** + **型ごとに別ノード kind** は SROA を
   gcc に確実に引き出させる。動的型言語では難しいが、静的型サブセットを
   切り出せる場面では有効。
5. **`@always_inline` は制御フロー系にも必要**。leaf ノードだけでなく、
   `loop / if / block / br` を inline させて初めて SD 内部で SROA が
   通る。
6. **`static inline EVAL` + `-rdynamic` セット**は他のフレームワーク間接呼び
   でも普遍。
7. **fixed-arity bucket + var-arity flat-array fallback** は可変要素を
   持つノード設計の標準パターン (call / br_table / format / switch)。

逆に **wastro 単独では限界**な項目:

1. 再帰関数の境界に残る間接呼び (fib 6× の主因)
2. 同一シグネチャ束縛のオーバーヘッド (loop bench の残り 2-3×)

これらはいずれも `lib/astrogen.rb` 側に手が要る。`docs/todo.md` と
`docs/runtime.md` 末尾参照。

---

## 関連ドキュメント

- `docs/runtime.md` — wastro の実行モデル全体
- `docs/done.md` — 実装済み機能インベントリ
- `docs/todo.md` — 未対応 / Phase 3-4 の framework 拡張プラン
- リポジトリ ルート `docs/perf.md` — 全サンプル横断の知見集
- `sample/castro/docs/perf.md` — 同種の静的型サンプル (C subset)
- `sample/ascheme/docs/perf.md` — tail-call 中心の動的言語事例
