# ASTro 性能向上知見集

ASTro でホスト言語を高速化する際にレイヤ横断で再利用できる知見集。
**フレームワーク中立な記述** を目指し、効果の数値や具体実装を引用するときは
どのサンプル言語で観測したか（`luastro` / `abruby` / `asom` / `ascheme` /
`castro` / `wastro` / `naruby`）を明示している。

各言語サンプルの詳細は個別の `sample/<lang>/docs/{done,todo,perf,runtime}.md`
にある。perf 専用の文書を持っているのは現状 **castro**
(`sample/castro/docs/perf.md`)、**ascheme** (`sample/ascheme/docs/perf.md`)、
**wastro** (`sample/wastro/docs/perf.md`)、**abruby**
(`sample/abruby/docs/perf.md`) の 4 言語。本ドキュメントから
`castro perf.md §N` のように引用する。

本ドキュメントは **「どのレイヤで何を回せば効くか」** の地図として読むことを
想定している。

## 0. 大原則

ASTro の理念上、最適化は以下の順で考える：

1. **EVAL body は触らない／触らずに済むように設計する。**
   速度はディスパッチャ特化（部分評価）から獲得するのが正道。
   EVAL body に手書き fast path を積むのはアンチパターン。
2. **SD の品質はビルド・リンク・dlsym 経路の整備で決まる。**
   特化 C ソースが綺麗でも、`.so` 経由で呼ぶ瞬間に GOT 経由間接呼びに
   なれば台無し。`-rdynamic` / `-Wl,-Bsymbolic` / wrapper post-pass
   は「特化したのに速くならない」を潰すための土台。
3. **インラインキャッシュ（`@ref`）は構造ハッシュを 0 で寄与させる。**
   ノード本体は構造的に不変、副情報だけが実行時可変、という分離が
   コードストア再利用と動的キャッシュの両立点。
4. **パーサ後処理で AST 形を整える** のは EVAL body 改変ではない。
   ループ融合、N引数 call 特化、tail-return 畳み込み、捕捉ローカル
   昇格などは parser-pass で済むので推奨。
5. **計測しないと進めない。**
   AOT は ccache・dlopen キャッシュ・分岐予測など罠が多い。
   `CCACHE_DISABLE=1` と「`code_store/` 削除 → bake → 別プロセス
   起動で計測」の cold-start 測定を基本にする。

---

## 1. 値表現

| トリック | 元 | 効果 | 一般性 |
|---|---|---|---|
| 8 バイトタグ付きワード（low3=tag, nil=0x00, fixnum=0x01, flonum=0x02, ptr=0x00 で非 0） | luastro `context.h`、ascheme `sobj` | calloc 領域がそのまま nil 初期化済になる、ポインタ比較が値比較に使える | フレームワーク的パターン |
| インライン flonum（指数 b62∈{3,4} のみ inline、それ以外は heap-box）| luastro `runtime.md §3` | luastro: mandelbrot 737→108 ms（6.8×）。ascheme でも採用 | 浮動小数演算が多い言語に汎用 |
| Pinned +0.0 セル（共有 heap-double セル）| luastro `lua_runtime.c::luav_box_double(0.0)` | luastro: mandelbrot 88→81 ms (-9%) | アキュムレータ初期化頻度が高い言語で効く |
| Serial-keyed cache（再束縛検出を版数 1 つで賄う、二重ガードを 1 比較に潰す）| ascheme `perf.md §13,16` | ascheme: sumloop 1.07×、ack 1.33× | フレームワーク的パターン（IC 一般） |
| Serial-keyed lazy parent-chain cache（深い lref の `e->parent` 走査を depth=2 以上で版数キーキャッシュに）| ascheme `perf.md §17` | ascheme: sieve 1.10×、list 1.12×、fannkuch 1.05× | 多段ネストレキシカルスコープを持つ言語に汎用 |
| RESULT 構造体返り（16B `{value, br}` で `rax:rdx`）| castro / abruby / wastro | castro: nqueens 95→27 ms (3.5×)、matmul 11→3 ms (3.7×)。ただし loop_sum は -50% 悪化することもあるので局所判断要（`castro perf.md §10`）| 制御フローを大量に持つ言語向け |
| **`RESULT.state` ビットフラグ + skip-count**（メソッド境界で `r.state &= ~MASK` の 1 命令、非ローカル return は state 上位 bits に「何枚 method boundary を skip するか」を埋め込み）| abruby `abruby perf.md §7`（`760aebc` + `1201eba`）| while -16%。block 内 `return` だけが frame chain walk を払い、**通常 return は frame push に追加コスト 0**。block 持ちでない call の hot path も汚さない | 多段 escape を持つ全言語（Ruby の block 内 return、Scheme の named-let escape、wasm `br N` と同型） |
| 静的型 frame の **union slot 表現**（`union { i32; i64; f32; f64; raw; }` で各 use-site が typed メンバアクセス）| wastro `context.h:51` | mandelbrot/nbody の inner loop が SROA で xmm/gpr レジスタ化、AOT 後の load/store メモリトラフィック消失 | 静的型サブセット全般（wasm / castro 系 / Java/Go bytecode） |

注意：
- inline flonum を入れても、heap-box への落ち込みパスを持っているなら
  そのアロケータ（`LuaHeapDouble` slab 化等）も併せて見ること。
- `union` で書くべきで、`uint64_t[]` + `memcpy` reinterpret では gcc の
  alias 解析が躊躇うことがある（wastro で確認）。

## 2. メモリ・データ構造

| トリック | 元 | 効果 | 備考 |
|---|---|---|---|
| 配列 + ハッシュのハイブリッドテーブル（連続 int キーは dense、相対閾値で緩める）| luastro `runtime.md §7` | luastro: sieve `for i=2,N` パターンが配列部に落ちる | Lua/Ruby 系 |
| **動的ハッシュテーブルの 2 段構成**（`capa ≤ 4` は packed linear 比較、それ以上は open-addressing + Fibonacci ハッシュ。struct に `inline_storage[4]` を埋め込み小テーブルは別 alloc 不要）| abruby `ab_id_table` (`939f878`, `5eff7bd`) | abruby: bm_object -28%（Point ivars table の別 alloc が消えた）。methods / 定数 / ivar shape / globals の **全動的テーブルに均一適用** | クラス・モジュール・ivar shape・グローバル等、小〜大の動的テーブルを多用する全言語 |
| **インスタンス変数の inline-N + heap overflow レイアウト**（`{klass, ivar_cnt, *extra_ivars, ivars[4]}`、クラスの `ivar_shape` (name → slot) で全 instance が同形）| abruby `abruby perf.md §4`（`dc796f1` + `e68d947`）| abruby: ivar -32%、binary_trees -20%、object 系全般。ivar 0..4 のクラス（小オブジェクト）は heap alloc なし、5 以上のクラス（nbody Body 7 ivars）だけ overflow heap | shape-based ivar を持つ動的言語（Ruby / Lua / Python）。ivar 数の分布が小～中に偏るほど効く |
| **GC mark の immediate skip**（mark 関数で `RB_SPECIAL_CONST_P(v)` なら `rb_gc_mark` を呼ばずスキップ。`:initialize` 等の hot intern は per-machine cache）| abruby `abruby perf.md §11.4`（`dce0e88`）| abruby: binary_trees -4%, object -6%。leaf node の mark が大量に走るベンチで効く | 即値タグを持つホスト言語上のインタプリタで mark コールバックが hot 経路にいる場合 |
| 文字列 intern プール（プロセス唯一、ポインタ等価＝値等価）| luastro `lua_runtime.c` | fan-in が大きい程効く。SD 内で再 intern 走らせない | 全言語に有効 |
| 捕捉ローカルの遅延 box 化（`LuaBox`）| luastro | 捕捉されない slot は box 不要 → frame 平坦のまま | クロージャを持つ全言語 |
| Leaf closure フレーム再利用（self tail-call は親フレーム上書き）| ascheme `perf.md §6` | ascheme: loop 3.2×、fib 2.8× | tail-call が hot な言語で大効果 |
| Leaf closure に `alloca`（GC malloc 不要）| ascheme `perf.md §7` | ascheme: ack 2.4×、fib 2.8×、tak 2.3× | 同上 |
| `sizeof(value)` 削減（jmp_buf を必要時のみ別アロケート）| ascheme `perf.md §9` | ascheme: sobj 208→48 B、list bench 2.4× | 大きな union を持つ言語で要点検 |
| ペアサイズ専用アロケート（24B for cons cell vs 全体 48B）| ascheme `perf.md §10` | ascheme: list 1.5× 追加 | 専用アロケータの典型 |
| 4 GB 仮想 pthread スタック（SD frame が ~1KB あるので深い再帰でガード破壊）| luastro `runtime.md §12` | luastro: Ackermann 等が走る | ASTro 共通インフラ |
| **SIGSEGV-as-bounds-check** （最大可能アドレスを覆う仮想予約 + 外周 PROT_NONE → OOB は SIGSEGV → handler で trap）| wastro `main.c:80-133`、`perf.md §2` | sieve/heapsort/mandelbrot の load/store が AOT 後に 1 命令 (`mov`) に縮む | アドレス上限が静的に分かる言語（wasm の u32+offset、メモリ安全な静的言語）。動的拡張テーブルには適用不可 |
| **mprotect ベースの動的拡張**（仮想予約済み領域内で `PROT_NONE → R/W` に上げるだけ。ベースアドレス不変保証を実装コスト 0 で取得）| wastro `node.def:1599`（`memory.grow`） | grow がほぼ free（syscall + キャッシュ更新）、host 側の握ったポインタが invalidate されない → FFI が単純化 | 上記 SIGSEGV パターンとセットで使う |

## 3. インラインキャッシュ（`@ref`）

ASTro の IC は **ノード本体に `@ref` 演算子で副情報を埋める** のが定石。
構造ハッシュ計算では `0` を返す（`lua_gen.rb` の `LuaFieldIC *` 参照）
ことで「同形 AST が同 SD を共有しつつ、IC 状態だけは実行時可変」が成立する。

| 対象ノード | キャッシュ内容 | 効果 |
|---|---|---|
| `node_field_get`（luastro）| `(hash_cap, slot pos)` | nbody -23%、mandelbrot 不変 |
| `node_gref`（ascheme）| 解決済みグローバル index | strcmp 走査 50ns → indexed load 3ns |
| `node_arith_<op>`（ascheme）| `(PRIM_<op>_VAL, valid?)` | rebind 安全な arith fast path |
| `node_method_call`（abruby）| `(klass, method_serial, body, dispatcher, prologue, ivar_slot, demote_cnt)` | OO ディスパッチの典型。`body` と `dispatcher` のキャッシュで method 構造体経由の間接 2 段を省略。`ivar_slot` は attr_reader/writer の事前解決スロット。`demote_cnt` は polymorphic backoff（§3 注）|
| `node_ivar_get/set`（abruby）| `(klass, slot)` | `obj->ivars[slot]` 直接読み書き。class 単位の `ivar_shape` で全 instance が同形 |
| `node_global_get`（**luastro 未実装**）| `(globals shape, slot pos)` | `print(...)` 系で効く見込み |
| `node_call_arg*`（**luastro 未実装**）| 解決済み closure | 自己再帰・library call に効く見込み |

設計指針：
- shape token は **常に変わらないキー**（`hash_cap` のような単調 / 構造値）を選ぶ。
- ヒット時に副情報のキー一致まで確認すること（`hash_cap` は同じでも別形があり得る）。
- ミス時は full lookup → IC 更新の順で書く（IC 内容を先に書いて落ちると古い IC が残る）。
- **miss path に `cold` 属性を付けるのは polymorphic dispatch で罠**。
  abruby `node_method_call_slow` で観測：cold セクションへの jmp が
  3 クラス循環ベンチ（bm_dispatch）の icache miss を増やし、`cold` を
  外して `noinline` のみにしたら -16% 回復（`abruby perf.md §11.6` /
  `7fcb86a`）。**monomorphic 想定なら cold OK、polymorphic なら hot zone
  に置く** が正しい。
- **polymorphic backoff** : 特化した kind から再 demote する回数
  （`mc->demote_cnt`）が閾値（abruby は 2）を超えたら再特化を諦める。
  3 クラス循環で swap_dispatcher が往復するのを防ぐ。abruby `f0e1095`。
  generalize: 「special → generic への降格コストが non-zero」な IC では
  必須の安全装置。

## 4. AOT / SD 生成系

ここが ASTro 固有のレイヤで、最大のレバレッジがある。

### 4.1 `@always_inline` / `@noinline` / `@canonical=` 注釈

- `@always_inline`: ホット制御フローや小型ノード（int 加算、local 読み書き、seq 等）で必須。これがないと gcc が SD を独立関数として吐いて間接 call が残る。ascheme で `scm_apply_tail` に付けただけで fib 0.46→0.25 s（`ascheme perf.md §14`）。**特に `loop / if / block / br_if` のような中型制御フローノード**にも付ける必要がある — wastro で `node_loop` が `EVAL_node_loop.isra.0` の out-of-line copy になり、内側ループ越しの SROA が断たれて mandelbrot で frame slot がメモリに落ちていた事例（`wastro perf.md §5`）。leaf だけでなく「rare-but-essential な制御フロー」も対象。
- `@noinline`: 可変長子（言語固有の side array、例: luastro の `LUASTRO_NODE_ARR`）を持つ親に限定する。**全 call ノードに付けるのは罠**（`ascheme perf.md §4` で観測：ディスパッチ鎖の inline を断ち切ってしまう）。
- `@canonical=BASE`: swap_dispatcher 系列（例: `node_fixnum_plus → node_integer_plus → node_plus`）が同じ構造ハッシュを持って **同一 SD を共有** する仕組み。abruby / ascheme / asom で活用。

### 4.2 `@ref` インラインキャッシュ

ノード本体内に副情報を埋める。構造ハッシュは 0 を返す。
詳しくは §3。

### 4.3 dlsym で全 SD を引けるようにする

**luastro で踏んだ罠**：

- **問題**: 子 SD を `static inline` で吐くと dlsym で見えず、子に対する
  `astro_cs_load` が常に miss → host binary の `DISPATCH_*` を呼ぶ →
  `.so` 境界往復で mandelbrot が ~50% を bounce に喰われる。
- **対処**: luastro `node.c::luastro_export_sd_wrappers` が
  生成済み `.c` を post-process し、
  - 中身の `SD_<hash>` を `SD_<hash>_INL` にリネーム（in-source の
    inline 鎖は維持）
  - 末尾に `__attribute__((weak)) RESULT SD_<hash>(...) { return SD_<hash>_INL(...); }`
    の wrapper を追記
- **効果**: luastro mandelbrot 76→65 ms (-14%)、cs hit が 2 → ~80 に増加。

同種の問題は他の言語サンプルでも発生しうるので、`@noinline` を多用する
言語では同じ post-process が必要になる可能性が高い。

### 4.4 可変長子の SD 焼き込み

`@noinline` 親の子は side array 経由（typed `NODE *` 走査では踏まれない）。
luastro では `lua_parser.c` の `LUASTRO_NODE_ARR` がこれにあたり、
`luastro_specialize_side_array` がそのリストを直接舐めて
`astro_cs_compile` する。luastro mandelbrot 65→60 ms (-8%)。

### 4.5 残存 `DISPATCH_*` 計測

luastro AOT-cached での実測：
- mandelbrot: 2/106 (98% hit)
- nbody: 19/534 (96.4% hit)
- fib: 6/29 (79% hit)

luastro では `OPTIMIZE` で `LUASTRO_TRACE_CS_MISS=1` を追えば kind / line /
hash が出るよう仕込んである（同種の trace は他言語にも入れられる）。
fib では `node_make_closure` (`@noinline`) が 1 件、残り 5 件は
parser 後処理 `pf_finalize_local_refs` による kind ミューテーション
（`local_get → box_get` 等）でハッシュ整合が崩れるのが原因（要追跡）。
**教訓（言語非依存）: `OPTIMIZE` で hash を確定させた後の kind 書き換えは
避ける**（あるいは `has_hash_value` を invalidate する）。

### 4.6 Hopt / Horg の二重ハッシュ（profile-aware）

abruby が採用。Horg は構造ハッシュ、Hopt は構造＋プロファイル情報を
混ぜたハッシュ。
- AOT 側は `SD_<Horg>` を吐く（プログラム間共有可）。
- PG 側は `PGSD_<Hopt>` を吐く（プロファイル特化）。
- `astro_cs_load` は PGSD を先に探し、なければ SD にフォールバック。

### 4.6.1 PGC at_exit 1 パス bake（2 パス stub→profile→recompile を避ける）

abruby `--pgc` は 1 パス + プロセス終了時 bake で組まれている
（`dde8e55`、`abruby perf.md §10`）。

- `abm.eval` 完了直後に entry を bake。`Hopt != Horg`（profile が乗った
  もの）は PGC `SD_<Hopt>.c` + `hopt_index.txt`、`Hopt == Horg` は AOT
  `SD_<Horg>.c`。
- eval 中の例外で bake は **スキップ**（crashed run の profile を
  永続化しない）。
- 次プロセスは on_parse で `cs_load(entry, file)` を呼び、索引経由で
  PGC を即採用。

abruby fib は AOT (0.40s) → PGC (0.34s) で約 14% 改善、optcarrot は
plain 45.6 fps → AOT 72.0 fps → PGC 86.5 fps（PGC は AOT より +20%）。

**教訓**（言語非依存）: PGO は「stub → profile → recompile」の 2 パスに
する必要は無い。**1 パスで実行 → at_exit に bake → 次プロセスで pick up**
で十分機能する。crashed run の profile は永続化しない条件分岐が要点
（不安定な特化を後続に持ち越さない）。

### 4.7 Profile-driven type-speculating SD（未実装、最大レバレッジ）

各算術ノードが観測した型・形状に応じ、ガード分岐（残るが）の slow path を
**末尾 deopt 呼び** に置換した SD を吐く。SD のコードサイズが減り、親 SD
への inline が効く。`luastro docs/todo.md` 筆頭、abruby に部分実装あり。

### 4.7.1 mtype-specialized prologue + NODE_DEF ラッパ（abruby Phase 1）

OO 言語のメソッドディスパッチは「AST メソッド / cfunc / ivar getter /
ivar setter」など複数の method type を持つ。これを `mc->mtype` で
runtime 分岐する素朴な実装は、cache hit hot path に毎回分岐コストを
払うことになる。

abruby は 2 階層で消した（`706b1cf` → `8a1c51d` → `4a9cf9a`、
`abruby perf.md §9`）：

1. **prologue 関数**（`prologue_ast_simple_0/1/2/n`、`prologue_ast_complex`、
   `prologue_cfunc`、`prologue_ivar_getter/setter`）を `static inline
   __attribute__((always_inline))` で書く。argc 特化版は argc を
   compile-time 定数として持つので `ctx_update_sp(frame.fp + REQ)` 等が
   定数畳み込みで消える。
2. **特化 call kind**（`node_call0/1/2_{ast,cfunc,ivar_get,ivar_set}`）
   を `NODE_DEF` で定義。初回 cache fill 時に `swap_dispatcher` で
   generic から特化版へ。特化版の EVAL は `mc->prologue` 間接呼び出し
   をせず **対応する prologue を名前で直接呼ぶ** → `always_inline` で
   展開され間接呼び出しが消える。

設計上の選択：**特化版を `NODE_DEF` で定義する**（plain `static RESULT
dispatch_call1_ast(c, n)` にしない）。理由は ASTroGen の specialize
機構が子 dispatcher（recv = `node_self` など）を SD\_ 関数ポインタとして
代入し inline できるため。plain function で `recv->head.dispatcher(c,
recv)` と書くと runtime 値の関数ポインタ読み取りで specialize 不可。

**効果**（plain mode）: fib -14%, method_call -15%, ack -11%。

**教訓** (言語非依存):

1. mtype 分岐は call hot path に毎回乗るので、 **K 種類の mtype × N 種類
   の argc** だけ kind を増やしても win に転じる。
2. 特化版を **plain function ではなく NODE_DEF にする**。子 dispatcher
   特化を引き出すために。
3. polymorphic backoff（`demote_cnt` 閾値）を必ず付ける。3 クラス
   循環で swap が往復しないように。

### 4.7.2 swap_dispatcher の粒度: dispatcher-only か kind-level か（cautionary）

abruby Phase 1 の最初の実装は `swap_dispatcher` で **NodeKind 全体** を
入れ替えていた。これだと kind ごとに ASTroGen が `DISPATCH/MARKER/HASH/
DUMP/REPLACER/SPECIALIZE` の 6 関数を吐くため、8 特化 kind × 6 = **48 個
の auto-generated 関数** が増えて icache pollution が起きる
（`abruby perf.md §12`、`8a1c51d` で導入 → `4a9cf9a` で revert）。

正解は **dispatcher pointer のみ書き換え、kind は元のまま**。

```c
// swap_dispatcher 後も n->head.kind は元の generic kind を保つ。
// 特化された動作は dispatcher 関数の中で完結。
n->head.dispatcher = specialized_dispatcher;   // 書き換え
// n->head.kind = ...                          ← 触らない
```

miss path で demote するときは `n->head.dispatcher = n->head.kind->default_dispatcher`
で生 default に戻す。

副次的利点：
- DUMP / GC mark / hash / specialize 全てが **元の kind 経由で動く** ので、
  「pseudo-kind」を framework に教える必要がない。
- **plain-mode のゲインは全保持**（fib -14%, method_call -15%, ack -11%）。
  失っているのは specialize された dispatcher 関数のシンボル分布が
  flat になる点だけ（ICache 容量に対して + 8 関数で済む）。

**教訓** (言語非依存): ノードの「実行時の振る舞いだけを変えたい」場合、
NodeKind を増やすのは罠。**dispatcher pointer の書き換えで足りる** 範囲は
そこに留める。ASTroGen の auto-generated 関数群が icache を圧迫する
規模になりうる。

### 4.8 Direct cross-SD calls（castro）

castro は `SPECIALIZE_node_call` で被呼出し SD のシンボル名を直接
書き出し、ランタイムディスパッチを介さず C コール一発にする。
**`-Wl,-Bsymbolic`** がないと GOT 経由になるので必須。castro fib_big の
disasm が gcc -O3 とほぼ一致するレベルまでいく（`castro perf.md §7`）。

## 5. パーサレベル書き換え

EVAL body には触らないが AST 形は整えてよい。**新しいノード kind を
増やすのは OK**（その新 kind の EVAL body は素直に書く）。

| 書き換え | 元 | 効果 |
|---|---|---|
| 捕捉ローカル昇格（`local_get → box_get`）| luastro | 非捕捉 slot は frame 直 access、捕捉のみ box |
| 全捕捉解析（function 末で is_captured を確定）| luastro `lua_parser.c::pf_finalize_local_refs` | 上記の前提 |
| **アンチパターン**：単一構文に張った融合ノード（例：`for i=N,M do sum=sum+i end` 専用 kind）| luastro が以前持っていた `node_numfor_int_sum`（撤去済み）| 一見ベンチ上は劇的に効いて見えるが、gcc が SCEV で閉形式化して loop が消えるため **インタプリタの品質を測れなくなる**。汎用性も無く ASTro の理念にも反するので使わない |
| N 引数 call 特化（fixed-arity call 専用ノード）| luastro `node_call_arg{0,1,2,3}` | metamethod / cfunc 経路を hot path から外す |
| **Fixed-arity 0..N bucket + var-arity flat-array fallback**（可変要素を持つノードは固定 bucket と side array の二段。hot path は sibling-inline、tail は `(index, count)` で flat 配列を indirect）| wastro `node_call_0..4` + `node_call_var` (`WASTRO_CALL_ARGS`)、wasm `WASTRO_BR_TABLE`、luastro `LUASTRO_NODE_ARR` | wastro: 0..4 引数で実モジュールの 99%+ をカバー、SD 鎖が collapse する。1024 引数まで kind 爆発なしに対応 | call / br_table / switch / format args など可変要素を持つ全ノード |
| **forward-ref body slot fixup の二段化**（ALLOC 時は body=NULL、parser 後処理で実 body をセット → そのあと OPTIMIZE）| wastro `main.c:194-240`（`PENDING_CALL_BODY` + `wastro_fixup_call_bodies`）| caller→callee body slot 経由の sibling-inline が前方参照込みで成立 | 前方参照を許す言語全般（wasm / Ruby `def` / JS hoist / C 関数宣言）|
| Tail-return 畳み込み（return ノードを seq/if に折り畳む → setjmp 不要）| castro `perf.md §1` | castro: fib_big 1.8×、ack/loop_sum/tak は setjmp ゼロ |
| **Self-tail-call → 直接 goto（parser pass で `node_loop` + `node_self_tail_call_K` に降ろす）**| ascheme `perf.md §18,§19` | ascheme: sum **2.7×**、sumloop **2.6×**、loop **4×**、ack 1.30×、nbody 1.33× | tail-call を持つ全言語に汎用。フレームワーク (`astrogen.rb`) は触らず、parser + node.def だけで完結 |
| Parse-time call-site index 化（`(call FUNC_IDX ...)` で名前解決を排除）| castro `perf.md §3` | castro: fib_big 3.4× |
| break/continue 静的分類（`node_for_brk_only` / `node_for_brk` / `node_for`）| castro `perf.md §8` | break のないループは setjmp ゼロ → 内側がレジスタ割付可 |
| 文字列定数の parser intern（intern 済みポインタを AST に直接埋め）| luastro `runtime.md §11` | SD 内で再 intern 走らない |
| Pre-interned metamethod 名（プロセス起動時に確保）| luastro `LUASTRO_S___INDEX` 等 | 毎アクセスの `lua_str_intern("__index")` を消す |
| **getter / setter の AST 形を method-add 時に検出**（`def x; @x; end` / `def x=(v); @x = v; end` を `ABRUBY_METHOD_IVAR_GETTER/SETTER` としてマーク → `method_cache_fill` で slot 事前解決 → dispatch path で frame 構築フルスキップ）| abruby `abruby perf.md §5`（`f8df623`）| abruby: nbody -6% 追加。`obj.x` (attr_reader) も hot path で `obj->ivars[slot]` 直接読みに | shape-based ivar を持つ動的言語の attr_reader/writer / property 形式アクセサ全般 |
| **ローカル定数畳み込み（再代入なし・キャプチャ書きなしの local）**| luastro `lua_parser.c::pf_fold_constants` | luastro: nbody の `SOLAR_MASS = 4*PI*PI` 系で global lookup 群を消せる。**ただし numfor.limit 位置は除外（§5.2）** |

注意：
- Lua の `load("...")` は別環境なので外側スコープのローカルは見えない →
  ローカル定数畳み込みは安全。
- Ruby 等の `eval` は同スコープが見えるので不可。**意味論を変えない範囲**
  という線は厳守。

### 5.1 Tail-call トランポリン消去（parser → node_loop / node_self_tail_call_K）

ASTro の理念「EVAL body は触らない」に沿って tail-call ループから
trampoline を撤廃する技法。**SD 生成器に手を入れる必要なし** ——
parser が自己末尾呼出を検出して新ノードに lower するだけ。

ascheme で実装したパターン（`ascheme perf.md §18,§19`）:

1. **新ノード 2 種を追加**:
   - `node_loop(body, nparams)`: body を `for(;;)` で回し、
     `c->loop_continue=1` を catch して frame slot に args を書き戻して
     再進入。
   - `node_self_tail_call_K(args[K])`: args を評価して `c->loop_args[]`
     に書き、`c->loop_continue=1` を立てて return。

2. **parser で 2 パターンを認識**:
   - **named-let** (`(let NAME ((p v)..) body)`): NAME は単一代入の
     ローカル束縛（実装内部の set! のみ）。compile_call で
     `(call_K (lref D 0) ...)` の形を `target_scope` 一致でマッチ。
   - **トップレベル define** (`(define (f params) body)`): f は global。
     `(call_K (gref f) ...)` を **シャドウなしの global symbol** で判定。
     gref_cache を持って `(set! f g)` 後は cache->serial mismatch で
     slow path（scm_apply_tail）に落ちる。

3. **入れ子 lambda は cleared**: compile_lambda が
   `CURRENT_SELF_CALL` を save+clear するので、body 内に escape する
   closure があってもそこから loop に戻ろうとしない（env identity が
   違うため対象外）。

実装の落とし穴（ascheme で踏んだ）:

- **named-let の init 式は wrapper の lambda frame で評価される**。
  syntactically は外側 scope だが、AST 構造としては
  `((lambda (NAME) (set! NAME ...) (NAME inits...)) UNSPEC)` の
  inits 位置 = wrapper lambda body 内。したがって init compile 時の
  scope は **outer_scope (wrapper を含む)** にしないと depth が 1 段
  浅くなる。chibi の `product` (call/cc + named-let) で発覚。
- **inner shadow** (`(let loop (..) (let ((loop ..)) (loop ..)))`):
  内側の loop は別物。lex_lookup_full で resolved scope を取り、
  registered scope と一致するときだけパッチ。
- **lib/astrogen.rb は一切触らない**。ノード定義は node.def に書き、
  hot path 実装は EVAL body 内で完結。framework から見れば普通の
  ノードとして specialize 対象。

#### 教訓（言語非依存）

- tail-call トランポリンを ELIMINATE するのに framework 改造は不要。
  **新しいノード kind を増やして parser で降ろす** だけで足りる。
- 「self-tail-call の identity」を判定するキーは scope（lex モード）/
  globals_serial（global モード）の二択で十分。シャドウ検知に
  resolved scope の identity を使うのは安価で正確。
- パッチを当てる前段で **CURRENT_SELF_CALL を nest lambda が clear**
  しないと、escape closure 経由で誤マッチが起こる。
  「token を一度だけ next compile_lambda に渡す」明示変数
  (`SELF_CALL_FOR_NEXT_LAMBDA`) を入れたほうが堅い。
- Scheme の named-let / 単一束縛 letrec / `(define (f ..) body)` の
  3 パターンが代表例。同等の構文を持つ言語（Lua の forward-declared
  function、Ruby の `def` 等）でも適用余地がある。

### 5.2 ループ bound の定数畳み込みは罠（gcc codegen surprise）

**luastro で観測した事例**だが、タグ付き値表現を持つ全言語で起こりうる
パターン。luastro の定数畳み込み (`lua_parser.c::pf_fold_constants`) は
ack/fib/tak で確実な勝ち（-7〜-15%）だが、**sieve だけ -O3 で +40% 退化**
するという一見不可解な現象に遭遇した。stand-alone `.so` 入れ替え試験で
安定再現。

#### 原因

gcc -O3 の **ループ誘導変数選択** の差。
ループ bound（例：`local N = 10000000; for i=2,N do ...end`）を fold して
`for i=2, 10000000 do ...end` の形にすると、gcc は limit が定数だと知って
**タグ付き形式でカウンタを保持** する：

```asm
.L153:
    addq    $2, %rbx                ; i_tagged += 2
    cmpq    $20000002, %rbx         ; immediate cmp
    je      .L150
.L156:
    movq    %rbx, %rsi
    orq     $1, %rsi                ; LUAV_INT = (i*2) | 1 — 2 µops
    movq    %rsi, 16(%rbp)          ; frame[var]
    ...
```

無 fold だと limit は runtime 値なので **raw 形式でカウンタを保持**：

```asm
.L209:
    incq    %r12                    ; i++
    cmpq    %r12, %r13              ; reg cmp
    jl      .L206
.L205:
    leaq    1(%r12,%r12), %rsi      ; LUAV_INT = (i << 1) + 1 — 1 µop (LEA!)
    movq    %rsi, 16(%rbx)
    ...
```

per-iteration で +1 µop（mov+or vs lea）。N=10M で ~70ms、N=50M で ~70ms
の差になる。`-O0/-O1/-Os` だと両者ほぼ同等で、退化は -O3 限定。
試した他の打ち手は本質を外していた：
- `-fno-tree-loop-distribute-patterns`：gap は縮むが他ベンチを悪化（局所無効化必要）
- `-fno-ipa-cp` / `-fno-ipa-cp-clone`：`.isra.0` クローン抑止、~10% 回復
- `__attribute__((noclone))`：上記と同程度

#### 解決（luastro での実装）

luastro `lua_parser.c::pf_fold_constants` で **`node_numfor` の `limit`
演算子位置にある local_get は fold しない** という carve-out を入れた
（commit `33d0f78`）。`numfor` 入口で余分な frame load が 1 回走るが
one-shot なので無視できる（fold 対象は数十回〜程度のループ前提なら気に
ならない）。

同 epoch A/B（luastro AOT-cached）：

| bench | fold OFF | fold w/ carve | fold w/o carve |
|---|---|---|---|
| ack    | 0.546s | 0.522s | 0.583s |
| fib    | 0.296s | 0.293s | 0.293s |
| nbody  | 0.498s | 0.470s | 0.492s |
| **sieve** | **0.346s** | **0.350s** | **0.503s** |
| tak    | 0.607s | 0.596s | 0.608s |

carve-out 入れて全 bench で fold-off 同等以上、sieve の退化は完全解消。

#### 教訓（言語非依存）

- gcc -O3 のループ最適化は「定数だと知ると逆に遅い asm を吐く」局所最適化が
  ありえる。**タグ付き値表現** を持つ言語だとなおさら踏みやすい
  （ASTro 全サンプル該当）。
- fold は意味論で安全でも **全 use-site に等しく適用していいわけではない**。
  use-site の context（特に hot loop の bound 位置）によっては抑制すべき。
  抽象パターン：「ループの **bound** に当たる位置への定数伝播は要警戒」。
- 「ASTro の理念は EVAL body を触らない」一方、parser pass で AST 形を整える
  ときも、特化 SD で gcc がどう codegen するかまで見て影響を測らないと、
  パフォーマンスの足を撃つことがある。今回はその好例。

## 6. ビルド・リンカ・ランタイムインフラ

| 設定 | 効果 / 理由 |
|---|---|
| ホスト `-O3 -fPIC -fno-plt -march=native` | デフォルト。fno-plt でホストの間接呼び 1 段減 |
| **ホスト `-rdynamic` 必須** | ホスト binary のシンボルを SD から dlsym で引くため。castro で 3.7× 効いた（`castro perf.md §2`）|
| `code_store/Makefile` 側 `-Wl,-Bsymbolic`（任意）| 同 `.so` 内のシンボル参照を local bind に。GOT 往復排除（`castro perf.md §7`）|
| `dlopen` のパス名キャッシュ回避：`all.so` を世代別 `all.<N>.so` にハードリンクして dlopen | `docs/code_store_quirks.md §1` |
| `all.tmp.so` → `mv all.so` の atomic rename | リンク中の中途半端な `.so` を見せない（`docs/code_store_quirks.md §2`）|
| `dlclose` しない | 既存ノードが古いハンドルの関数ポインタを保持しているため（`docs/code_store_quirks.md §3`）|
| `CCACHE_DISABLE=1` ベンチ時必須 | AOT-1st の cold-start を正しく測るため。デフォルト ccache が hit するとコンパイル時間ゼロに見える |
| 4GB 仮想 pthread スタック | SD frame ~1KB × 深い再帰対策（luastro `runtime.md §12`）|
| 共通パラメタ拡張（`common_param_count = 3`）| luastro / wastro / castro：`(c, n, frame)` を全 EVAL の必須引数化 → reload/store 削減。castro loop_sum 5→2 ms（`castro perf.md §5`）。wastro は ASTroGen を override する形で実装 (`wastro_gen.rb`) — node.def 側に追加ボイラープレート不要 |
| `static inline EVAL` + ホスト `-rdynamic` セット | castro / wastro 共通。SD\_ → EVAL → 子 SD\_ の鎖が `.so` 境界を一度も跨がない直接呼びに collapse する |
| **Boehm GC pre-grow + lazy collect**（`GC_expand_hp(64MB)` + `GC_set_free_space_divisor(1)`）| ascheme `perf.md §20`：list 1.14×、fannkuch 1.13×、fib35 1.14×、nbody 1.10×、deriv 1.08× | Boehm を使う全言語に **直接適用可能**。`perf stat` で `sys` 時間 / page-faults を見て、heap 成長による mmap が支配的なら効く。**確認方法**：`perf stat -e cycles,page-faults ./<lang>-bench`、page-faults × 4KB ≈ heap 成長量 |

## 7. プロファイル駆動 (PG) 全般

- `--pg-compile`: 一度普通に走らせて `head.dispatch_cnt` を集め、
  閾値（ascheme は 10）以上のエントリだけ AOT bake。コールド重要視
  プログラムでビルド時間を圧縮できるが、全部 hot な bench では効果
  薄め。
- `swap_dispatcher` ファミリ（abruby）：型ガード失敗で kind を
  上位互換ノードに rewrite。canonical hash で SD を共有しつつ
  graceful fallback を実現。
- 観測された型/形状を `@ref` に貯めて bake 時に読む型推測 SD は最大
  の未着手領域（§4.7）。

## 8. やってみたが戻したこと（cautionary tales）

| 失敗 | 教訓 |
|---|---|
| `@noinline` を全 call ノードに付ける（`ascheme perf.md §4`）| ディスパッチ鎖の inline を断つ。可変長子を持つ親に限定する |
| try-and-fallback で先に引数 compile（`ascheme perf.md §15`）| 不一致時に再 compile されて木が二重化。**安いキー一致を先に判定**して compile は最後 |
| ベンチで ccache 有効のまま | AOT 1st の数字が嘘になる |
| 再帰インライン（gcc -O3 が fib を 6 段展開している分の差）| ASTro レイヤでは効率的に再帰展開＋束縛管理ができていない（`castro docs/todo.md` の懸案）|
| RESULT 構造体返り（castro / abruby 採用）| call boundary は速くなるが、leaf node のコストが上がる場合あり（`castro perf.md §10`）。配置に注意 |
| dlclose して `.so` 入れ替え | 既存ノードが旧ハンドル経由の関数ポインタ保持。世代別 `all.<N>.so` で運用（`docs/code_store_quirks.md §3`）|
| ループ bound への定数伝播（fold） | luastro sieve で +40% 退化（§5.2）。タグ付き値表現を持つ言語はループの bound 位置への fold を抑制すべし |
| **`no_tail_call` flag による trampoline 省略**（ascheme で試行）| compile_lambda が body の tail 位置に call_K がないことを検出して closure に flag を立て、scm_apply が `for(;;)` をスキップ。**理論的には fib で 10% 出るはずが実測 wash**。gcc -O3 と分岐予測が「単発反復」を既に最適化していて、追加の cache check 分岐が wash する。**教訓**：trampoline overhead の per-iter コストは現代 CPU では既に ~1cycle 以下になっていることが多く、static elision が効かないことがある |
| **Known-global call の closure-field cache**（ascheme §19 試作で revert）| `(fn args)` の fn が non-shadowed global 時に `(closure, body, dispatcher, env, leaf, has_rest, nparams)` を call site にキャッシュして scm_apply の closure-field walk をスキップ。**5-6 fields のキャッシュ check 分岐が closure 直読み（既に L1 cached）より重く、ack で -42%**。**教訓**：「キャッシュ追加は必ずしも win ではない」。元の load が L1 hit している場合、cache の cold load + 検証分岐がむしろコスト増。`fast_eligible` 単一フラグに圧縮しても elision 不足で wash |
| **method inlining 単体で 2× を狙う**（abruby cluster-bake 探索、`abruby perf.md §12`、`a611ef0`）| caller PGSD が monomorphic non-self callee の body を `PGIB_<HOPT>` として同 .c に co-emit する仕組みを実装。**bm_method_call -18.5%、bm_ivar -15.9%** だが **optcarrot は ±2%**（hot method body 数百行 → `always_inline` で gcc register allocator 崩壊、IPC 2.58 → 2.17）。**教訓**：ASTro は既に method 内を AST inline 済みなので、method 境界で稼げるのは call overhead の数 % のみ。実コードでは libruby (Array/GC/alloc) の 30% を触れない限り 2× に届かない。**method inlining は「small helper を tight loop で呼ぶ」コードでは win、それ以外では wash** |
| **shape_id-based ivar IC を SPECIALIZE 焼き込み無しで導入**（abruby `76b43ff`、`abruby perf.md §4`）| `ic->klass` ポインタ比較を `shape_id` 整数比較に置換 → fast path のガードは整数比較 1 発になるはずだが、`ic->shape_id != 0` 追加チェック (+1 insn/call) と ivar_set transition の slow path（abruby_shape_for + ハッシュ lookup）で **optcarrot -9% 退化**（cycles +7%、L1 dcache miss +5%）。**教訓**：「shape-based に切り替えれば速い」は前提条件付きで、**SPECIALIZE 時に shape_id をリテラル化**＋**Overflow slot の fast-path grow** までセットでようやく win に転じる |
| **swap_dispatcher で NodeKind 全体を入れ替える**（abruby `8a1c51d`、`4a9cf9a` で revert）| Phase 1 mtype 特化の最初の実装で 8 特化 kind × `DISPATCH/MARKER/HASH/DUMP/REPLACER/SPECIALIZE` の 6 関数 = **48 個の auto-generated 関数追加** → tak +6%, sieve +2% の icache 圧。dispatcher pointer のみの書き換えに変更して全ゲイン保持。**教訓**：実行時の振る舞いだけ変えたいなら NodeKind を増やすのは罠（§4.7.2） |
| **inline fast-path を generic node body に内蔵**（abruby `7e5e712`、`abruby perf.md §3`）| `node_fixnum_plus/minus/...` body に Float×Float 用 inline 分岐を埋めていたが、整数ドミナント (optcarrot) の SD 各 site で `*`,`<`,`+` のコードサイズが ~3 倍（90 → 25 insn）に膨張。動的 `swap_dispatcher` で `node_flonum_*` に昇格する仕組みが既にあるので削除して clang optcarrot PGC +3.1%。**教訓**：動的 swap が常用で成立しているなら、**generic node に手書き fast path を抱えるのはアンチパターン**。SD コードサイズ × site 数だけ icache に効く |
| **`cold` 属性を polymorphic miss path に**（abruby `7fcb86a`、`abruby perf.md §11.6`）| `node_method_call_slow` を `cold` で冷セクションに追いやっていたが、3 クラス循環ベンチ（bm_dispatch）で cold 飛びの jmp が icache miss 増加 → `cold` を外して `noinline` のみに戻したら -16%。**教訓**：cold は monomorphic 想定なら OK、polymorphic 想定なら hot zone に置く |
| **EVAL\_\* に `always_inline`**（abruby、最終的に `static inline` 止まり）| `static` だけでは specializer の SD\_ ラッパーから cross-node inline が効きにくい → `static inline` で ack -21%, tak -10%, fannkuch -10%。ただし `always_inline` を付けると plain mode で **+20% 悪化**（code bloat で icache miss）。**教訓**: 全 EVAL に `always_inline` はアンチパターン。**小型 leaf** と **rare-but-essential 制御フロー** に絞る（wastro `perf.md §5` と同根）|
| **子ノードの rewrite で stale な親フィールドを使う**（abruby CLAUDE.md 注意事項）| `EVAL_ARG` 中に子ノードが自身の `swap_dispatcher` で replace され、親ノードのフィールドが更新されることがある。**stale なローカルパラメータ（DISPATCH 引数）を使うと古い子ポインタを参照する**。教訓: 「rewrite 後に親フィールドを再ロード」を node.def 規則として明文化する。abruby は `n->u.node_xxx.left` で再ロード |
| **SD\_ 内の kind 名ハードコード越しに runtime swap_dispatcher を効かせる**（abruby `abruby perf.md §9.4`）| ASTroGen 生成の SD\_ は `EVAL_node_call1(c, n, recv, recv_dispatcher, ...)` を文字列レベルで焼くため、runtime に `swap_dispatcher` で `EVAL_node_call1_ast` に切り替えても compiled モードでは反映されない。abruby では Phase 1 の plain ゲインが compiled モードで失われる原因に。**教訓**：「runtime swap」と「parse-time specialize で SD\_ に焼く」は **二重の状態を持つ**。後者を profile に応じて再 bake する仕組み（PGC）が無いと runtime swap は compiled モードに伝わらない。Phase 2 で解決される予定 |

## 9. 大きな未踏領域

1. **Profile-driven 型推測 SD**（最大効果見込み・全言語横断）
2. **残存 `DISPATCH_*` の根絶**（`pf_finalize_local_refs` のような
   parser 後処理 mutation と OPTIMIZE のキャッシュ整合をどう取るか）
3. **再帰インライン**（gcc が踏める展開を ASTro でも踏めるように）
4. **独自ローダー**（`code_store_quirks.md §自前ローダーで解消する`）

## 10. ascheme チャレンジ記（言語事例研究）

R5RS Scheme on ASTro の最適化遍歴を **どこで何が効いたか** の観点で
まとめる。詳細は `sample/ascheme/docs/perf.md` の §1–§20。

ascheme は最初期から R5RS 準拠（数値タワー、tail-call、call/cc、
delay/force、ports、多値、quasiquote）を抱え込んでいるので、
最適化対象として現実的なオーバヘッド分布を持つ。chibi-scheme との
比較は §0 で立てた表（`sample/ascheme/perf.md §17 末尾`）参照。

### 10.1 効いた打ち手（インパクト順）

| § | 打ち手 | 効果 | 一般性 |
|---|---|---|---|
| 18 | named-let → for(;;) goto（parser pass） | sum 0.54s→0.20s（**2.7×**）、sumloop 2.6× | 全 tail-call 言語 |
| 19 | global 自己 tail-call → goto（parser pass） | loop 0.20s→0.05s（**4.0×**）、ack 1.30× | 全 tail-call 言語 |
| 15 | parser の try-and-fallback で引数 compile を遅らせる + `aot_compile_and_load` の同 hash dedup-skip 撤廃（古いコードに**致命的バグ**：実 NODE の dispatcher patch 漏れで全 SD bypass）| sum 2.7×、sumloop 2.4× | parser-pass 持つ全言語 / 「同 hash 共有 SD」と「同 NODE pointer」を混同しない |
| 17 | lref 親チェイン lazy cache（depth ≥ 2）| sieve 1.10×、fannkuch 1.05× | 多段スコープ全言語 |
| 20 | Boehm GC pre-grow + lazy collect | list 1.14×、fannkuch 1.13×、fib35 1.14× | Boehm 利用全言語 |
| 16 | arith_cache の二重 check を serial 1 つに圧縮 | sumloop 1.07×、cps_loop 1.06× | フレームワーク的 IC 設計 |
| 14 | `scm_apply_tail` を node.h に inline + `__attribute__((always_inline))`| sum 0.54s→0.07s 全体寄与の起点 | フレームワーク的 |
| 13 | gref / arith cache の serial 化 | ack 1.3×（rebind safety 維持）| IC 一般 |
| 6  | leaf closure の `alloca` frame | fib 2.8×、tak 2.3× | tail-call 言語 |
| 4  | self-tail-call の frame 再利用 | loop 3.2×、sum 1.9× | tail-call 言語 |
| 3  | Ruby 流 inline flonum | mandel 6×、nbody 1.7× | 浮動小数多用言語 |

### 10.2 試して効かなかった打ち手（cautionary tales）

| § | 試行 | 結果 | 理由 |
|---|---|---|---|
| —  | env-skip in scm_apply トランポリン（frame-reuse 経路で `c->next_env == c->env` を skip） | wash | 追加した分岐が分岐予測の利得を相殺 |
| 19 試作 | known-global call の closure-field cache | 全体 -3〜-12%（ack -42%）| キャッシュ自体が cold L1 で、closure の direct 読みが既に L1 hit していて検証分岐が dead weight |
| —  | `no_tail_call` flag で trampoline 省略 | wash | gcc -O3 + 分岐予測が trampoline の単発反復を既に最適化しており、追加 flag check が cancel out |

### 10.3 ASTro user 視点の総括

最大の教訓は **「framework (`lib/astrogen.rb`) を触らずに、parser と
node.def だけで 2-7× を取れる」** こと。本当に効いた最適化はどれも
EVAL body 不可侵原則を守った範囲：

- §18 / §19: parser が新 kind に降ろし、framework は普通に specialize。
- §17: node.def 内の helper 関数の構造変更（hot path に if-chain 追加）。
- §20: GC 初期化パラメータ調整、framework と無関係。

逆に **盛大に外した試行はどれも「キャッシュ追加でハードに賭けた」もの**：
- 既に L1 にいる closure 直読みに割り込もうとして遅くなる。
- gcc + 分岐予測が既に消化している分岐を、明示 elision してかえって遅くなる。

この経験から、ascheme で適用しなかった "PGO 駆動デヴァーチ"
（§4.7 / §9.1）も **「現状の hot path がどこで時間を使っているかを
`perf stat` / `perf record` で見てから」** 着手すべきだろう。

最後の勝因が **`perf stat` で sys 時間と page-faults を見たこと**
（§20）だったのは示唆的：CPU が処理しているのは命令だけではなく、
カーネル経路のコストも測らないと打ち手の優先順位を間違える。

---

## 11. wastro チャレンジ記（言語事例研究）

WebAssembly 1.0 を ASTro 上で実装した wastro の性能取り組みを **「静的型
言語が ASTro framework 標準特化だけでどこまで native に迫れるか」**
の観点でまとめる。詳細は `sample/wastro/docs/perf.md` の §1–§12。

wastro は **ascheme / luastro / abruby と異なり、parser-pass の最適化を
ほとんど入れていない** にもかかわらず AOT cached で native の 3× に
到達する。これは ascheme の §10.3 で出た総括 「framework を触らず
parser + node.def だけで 2-7× を取れる」の **逆極端** ── parser-pass すら
触らず framework 標準特化だけで 3× を取った事例。

### 11.1 効いた打ち手（インパクト順、wastro 視点）

| § | 打ち手 | 効果 | 一般性 |
|---|---|---|---|
| 2 | SIGSEGV-as-bounds-check（8GB 仮想予約 + PROT_NONE 外周）| sieve / heapsort / mandelbrot の load/store が 1 命令化、wasmtime と互角レンジ | アドレス上限が静的に分かる言語 |
| 3 | `union wastro_slot` で frame を表現 + 型ごとに別 kind の `local.get_<T>` | mandelbrot の inner loop で zx/zy/zx2/zy2/iter が xmm/gpr 同時保持 | 静的型サブセット全般 |
| 1 | RESULT-pair 返り値（`{value, br_depth}` で `rax:rdx`）| `block / loop / if` が 3 行で書ける、setjmp が hot path から消える | 多段 break/escape を持つ全言語 |
| 5 | 制御フロー (`loop / if / block / br_if`) に `@always_inline` | mandelbrot の `node_loop` が `EVAL_node_loop.isra.0` out-of-line 化を防ぐ | leaf だけでなく中型制御フローにも適用すべき |
| 4 | fixed-arity 0..4 + var-arity flat-array fallback | 実モジュールの 99%+ が hot tier、SD 鎖が collapse | 可変要素を持つ全ノード |
| 6 | `static inline EVAL` + `-rdynamic` | SD\_ → EVAL → 子 SD\_ が `.so` 境界を一度も跨がない | フレームワーク間接呼び全般 |
| 9 | mprotect ベースの `memory.grow`（base address 不変）| grow がほぼ free、host 側のポインタが invalidate されない | 動的拡張する単一データ領域全般 |
| 7 | call body slot fixup の二段化（forward-ref 対応）| caller→callee SD direct call が前方参照込みで成立 | 前方参照を許す全言語 |

### 11.2 やっていない（やる必要がない）打ち手

wasm の言語 semantics が **静的型 + linear control flow** なので、他言語で
hot だった以下の打ち手は wastro では不要:

- プロファイル駆動の型推測 SD（型は静的に決まる）
- インラインキャッシュ `@ref`（動的ディスパッチがない）
- 数値タワー / box 化（i32/i64/f32/f64 で FIX）
- ローカル定数畳み込み（`i32.const N` で表現済み）
- ループ融合（wasm は generic `loop + br_if` のみ）
- Tail call → goto（wasm 1.0 に末尾呼び出し最適化なし）
- Leaf closure 用 alloca frame（VLA 一発で C スタックに乗る）

つまり wastro は **「framework 標準特化が直接効く設計の言語」** で
parser-pass 系最適化の引き出しを使い切らずに 3× に到達している。

### 11.3 wastro 単独では超えられない壁

| 課題 | bench での現れ | 必要な framework 拡張 |
|---|---|---|
| 再帰境界の間接呼び（`is_specializing` でサイクル破断後の closing edge）| fib 6× ギャップ | Phase 3: depth-bounded recursive specializer / SCC-aware hashing (`sample/wastro/docs/todo.md` 末尾) |
| 全 SD が `RESULT (CTX*, NODE*, slot*)` 統一シグネチャ | sum / sieve の残り 2-3× | Phase 4: typed dispatcher signatures (引数 / 返り値を C 型で授受) |
| var-arity call の sibling-inline ロス | 稀。embind / autogen bridge で発現 | luastro `luastro_specialize_side_array` 相当の post-process |

これらは ascheme / luastro / castro 全てに共通する未踏領域 (§9 で言及)。
**wastro が静的型ゆえに parser-pass を使い切らないこと**で、framework
側のボトルネックが綺麗に露出している点が示唆的。

### 11.4 ASTro user 視点の総括（wastro 編）

- **静的型 + linear control flow なら、framework 標準特化だけで native 3×**
  に到達する。これは ASTro の妥当性検証として大きい。
- **「parser-pass を入れる動機がない」言語では、framework のボトルネック
  （再帰境界、統一シグネチャ）が直接見える**。逆に luastro / ascheme は
  parser-pass で hot path を最適化してしまうので framework のボトルネック
  が露出しにくい。
- ascheme §10.3 と合わせると、二極の事例が揃った：
  - ascheme: parser + node.def で 2-7× → framework 触らず
  - wastro: framework 標準特化だけで 3× → parser ほぼ触らず
- 残り 2-6× を取るには **どちらの言語も** ASTro framework 側の改修
  （Phase 3-4）が必要、と双方が示している。

---

## 12. abruby チャレンジ記（言語事例研究）

Ruby サブセット言語 abruby を **CRuby の C extension として** 実装した
立場から、「**ホスト言語のランタイム（VALUE / GC / Prism / Array /
Bignum / Float / Regexp）を借りつつ評価部だけ ASTro で部分評価する**」
モデルでどこまで行けるかを整理する。詳細は
`sample/abruby/docs/perf.md` の §1–§14。

abruby は ascheme と wastro の間にいる：

- ascheme（§10）: framework 触らず parser + node.def で 2-7×
- wastro（§11）: parser ほぼ触らず framework 標準特化で native 3×
- **abruby**: **両方使う**（parser-pass で算術 specialize + framework
  標準特化）が、**ホスト libruby が profile 30% を占める ので桁を上げる
  には libruby 内製化が必要**

### 12.1 効いた打ち手（インパクト順）

| § (abruby) | 打ち手 | 効果 | 一般性 |
|---|---|---|---|
| §1 | swap_dispatcher 家系図（fixnum → integer → flonum / generic）+ `@canonical=` で AST hash 共有 | 全 OO bench で 2-3× | OO 言語の算術特化全般 |
| §2 | method_cache @ref（klass + method_serial + body + dispatcher）| OO ディスパッチ全般、ack -29% など | OO 言語の hot dispatch |
| §3 | inline flonum + flonum 系列 swap_dispatcher（fixnum_*_slow が Float 観測で flonum_* に昇格）| mandelbrot -44%, nbody -36% | 浮動小数の多い動的言語 |
| §4 | ivar IC + `inline 4 + heap overflow` レイアウト + class shape | ivar -32%, binary_trees -20% | shape-based ivar を持つ動的言語 |
| §5 | attr_reader/writer detection（method-add 時に AST 形を見る）| nbody -6% 追加 | property/getter/setter を持つ全 OO 言語 |
| §6 | ab_id_table ハイブリッド + inline storage | bm_object -28% | 動的テーブル全般 |
| §7 | RESULT bit-flag + 非ローカル return 用 skip-count | while -16%、非 block 経路は追加コスト 0 | 多段 escape を持つ全言語 |
| §8 | NodeHead スリム化 + cold/warm/hot zone | キャッシュ効率改善（多 bench で薄く効く）| 全 ASTro 言語 |
| §9 | mtype-specialized prologue + NODE_DEF ラッパ + polymorphic backoff | plain mode で fib -14%, method_call -15% | OO 言語の dispatch 特化 |
| §10 | Hopt/Horg 2 段ハッシュ + 1 パス at_exit bake | optcarrot 45.6 fps → 86.5 fps | 全 ASTro 言語の PGO |

### 12.2 試して効かなかった打ち手（cautionary tales）

| § (abruby) | 試行 | 結果 | 理由 |
|---|---|---|---|
| §12 | Cluster bake (method inlining 自動化) | bm_method_call -18.5% だが optcarrot ±2% | always_inline が大きな body で gcc register allocator 崩壊。**ASTro は既に AST inline 済 + libruby 30% で頭打ち** |
| §4 | shape_id-based ivar IC（SPECIALIZE 焼き込み無し）| optcarrot -9% | transition slow path + fast path +1 insn。literal-fold とセットでないと win 無し |
| §1 | swap_dispatcher で NodeKind 全体を入れ替え | tak +6%, sieve +2% icache 圧 | 48 個の auto-generated 関数増。dispatcher pointer のみ書き換えに変更 |
| §3 | `node_fixnum_*` body に inline Float 分岐 | SD コードサイズ 3 倍に膨張 | 動的 swap で `node_flonum_*` に昇格する仕組みがあるなら不要 |
| §11.6 | `cold` 属性を miss path に | bm_dispatch -16% 退化 | polymorphic dispatch で cold 飛びの jmp が icache miss 増 |
| §11.6 | EVAL_* に `always_inline` | plain mode +20% | code bloat で icache miss。`inline` 止まりが正解 |
| (報告) | iterator 融合ノード手書き（`Integer#times`, `Array#each` + block）| revert | N iterator で N ノード、scale せず。frame 共有が `binding` で破綻 |

### 12.3 ASTro user 視点の総括（abruby 編）

ascheme §10.3 / wastro §11.4 に続く 3 つ目の「framework に手を入れない範囲で
どこまで行けるか」事例。abruby の特殊事情:

- **ホスト言語のランタイムを借りる** ことで **実装コストが激減**：CRuby の
  Prism パーサ・GC・Array/Hash/Bignum/Regexp/Float をそのまま使える。
  abruby は 2 ヶ月で OO + Fiber + 例外 + Proc + 22 クラス + optcarrot
  実行可能まで到達した。
- **代償として hot path に libruby cfunc が残る**：optcarrot で
  `rb_ary_*` / `rb_gc_mark_*` / `gc_sweep_step` で profile 30%。
  abruby で触れない領域。
- **method inlining 単体で 2× は出せない** ことが cluster-bake 探索で
  実証された。これは ASTro の AST 内 inline が深い + libruby 不可触の
  二重制約。

二極事例と合わせると：

- ascheme: parser + node.def で 2-7× → framework 触らず（pure 動的）
- wastro: framework 標準特化だけで 3× → parser ほぼ触らず（pure 静的）
- **abruby: parser + node.def + Hopt/Horg PGO で plain → AOT で 4×、PGC でさらに +20%
  だが libruby 30% で頭打ち（embedded host モデル）**

abruby の経験が示す **embedded interpreter モデル特有の教訓**:

1. **ホスト VALUE 表現を共有する** と Bignum / Float のヒープ
   オブジェクトを host と渡し合える。reuse の最大の利点。
2. **ホスト cfunc の profile が hot path に居座る** と最適化が頭打ち
   になる。method inlining ですら抜けられない。
3. **GC mark の immediate skip / per-machine intern cache** など
   ホスト連携の小技は地道に効く（合計 -10% 程度）。
4. abruby のような embedded 言語で次の桁を取るには、**ホスト cfunc を
   ASTro 側に取り込む**（Array#push を `node_*` で書き直す）か、
   **PGO 駆動の type-speculating SD**（§4.7）でホスト境界の cfunc 呼び
   自体を inline するか。後者は全サンプル横断の最大未踏領域なので、
   abruby 単独で解ける問題ではない。

---

## ベンチマークの読み方

各サンプルの `make bench` は典型的に以下を比較する（言語ディレクトリ配下で
走らせるので `<lang>-` プレフィックスは付けない）：

| 列 | 内容 |
|---|---|
| `plain` | 純インタプリタ（`code_store/` を参照しない） |
| `AOT-1st` | `-c`：bake + run を 1 プロセスで実行。**gcc コンパイル時間込み** の cold-start 測定。`code_store/` を毎反復削除 + `CCACHE_DISABLE=1` |
| `AOT-cached` | bench setup で 1 度 bake（時間外）→ 計測時は plain run で warmed `all.so` を消費 |
| `lua5.4` / `luajit` / `gcc -O3` 等 | 比較対象（リファレンス実装） |

「AOT-1st が遅い」のは正しい。性能を稼ぎたいなら AOT-cached を見る。

「`plain` が比較対象より速い」のは parser-pass の単純化が効いているケース
（例: 比較対象が bytecode VM で loop opaque、luastro 側は AST が直 C ループに
落ちて gcc の SCEV が走る）。**この種の「ベンチ上の劇的差」は怪しんでよい**：
インタプリタの品質ではなく、ホスト C コンパイラが何をできたかを測っている
ことが多い。撤去された `node_numfor_int_sum` の loop ベンチ 13× がその典型例
（§5 「アンチパターン」行を参照）。

---

## 関連ドキュメント

- `docs/idea.md` — ASTro 全体設計
- `docs/code_store_quirks.md` — dlopen 周りの罠と暫定対処
- `sample/<lang>/docs/perf.md` — 言語ごとの詳細（ascheme / castro / wastro / abruby が詳しい）
- `sample/<lang>/docs/done.md` / `todo.md` — 個別言語の進捗・残課題
- `sample/<lang>/docs/runtime.md` — 個別言語のアーキテクチャ
