# ASTro 性能向上知見集

ASTro でホスト言語を高速化する際の、これまでに各サンプル
（luastro / abruby / asom / ascheme / castro / wastro / naruby）で
得られた知見をフレームワーク横断でまとめたもの。

各サンプルの詳細は個別の `sample/<lang>/docs/{done,todo,perf,runtime}.md`
にあり、本ドキュメントは **「どのレイヤで何を回せば効くか」** の地図
として読むことを想定している。

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
| インライン flonum（指数 b62∈{3,4} のみ inline、それ以外は heap-box）| luastro `runtime.md §3` | mandelbrot 737→108 ms（6.8×）。ascheme でも採用 | 浮動小数演算が多い言語に汎用 |
| Pinned +0.0 セル（`luav_box_double(0.0)` が共有セルを返す）| luastro `runtime.md §3` | mandelbrot 88→81 ms (-9%) | アキュムレータ初期化頻度が高い言語で効く |
| Serial-keyed cache（再束縛検出を版数 1 つで賄う、二重ガードを 1 比較に潰す）| ascheme `perf.md §13,16` | sumloop 1.07×、ack 1.33× | フレームワーク的パターン（IC 一般） |
| RESULT 構造体返り（16B `{value, br}` で `rax:rdx`）| castro / abruby | nqueens 95→27 ms (3.5×)、matmul 11→3 ms (3.7×)。ただし loop_sum は -50% 悪化することもあるので局所判断要 | 制御フローを大量に持つ言語向け |

注意：
- inline flonum を入れても、heap-box への落ち込みパスを持っているなら
  そのアロケータ（`LuaHeapDouble` slab 化等）も併せて見ること。

## 2. メモリ・データ構造

| トリック | 元 | 効果 | 備考 |
|---|---|---|---|
| 配列 + ハッシュのハイブリッドテーブル（連続 int キーは dense、相対閾値で緩める）| luastro `runtime.md §7` | sieve `for i=2,N` パターンが配列部に落ちる | Lua/Ruby 系 |
| 文字列 intern プール（プロセス唯一、ポインタ等価＝値等価）| luastro `lua_runtime.c` | fan-in が大きい程効く。SD 内で再 intern 走らせない | 全言語に有効 |
| 捕捉ローカルの遅延 box 化（`LuaBox`）| luastro | 捕捉されない slot は box 不要 → frame 平坦のまま | クロージャを持つ全言語 |
| Leaf closure フレーム再利用（self tail-call は親フレーム上書き）| ascheme `perf.md §6` | loop 3.2×、fib 2.8× | tail-call が hot な言語で大効果 |
| Leaf closure に `alloca`（GC malloc 不要）| ascheme `perf.md §7` | ack 2.4×、fib 2.8×、tak 2.3× | 同上 |
| `sizeof(value)` 削減（jmp_buf を必要時のみ別アロケート）| ascheme `perf.md §9` | sobj 208→48 B、list bench 2.4× | 大きな union を持つ言語で要点検 |
| ペアサイズ専用アロケート（24B for cons cell vs 全体 48B）| ascheme `perf.md §10` | list 1.5× 追加 | 専用アロケータの典型 |
| 4 GB 仮想 pthread スタック（SD frame が ~1KB あるので深い再帰でガード破壊）| luastro `runtime.md §12` | Ackermann 等が走る | ASTro 共通インフラ |

## 3. インラインキャッシュ（`@ref`）

ASTro の IC は **ノード本体に `@ref` 演算子で副情報を埋める** のが定石。
構造ハッシュ計算では `0` を返す（`lua_gen.rb` の `LuaFieldIC *` 参照）
ことで「同形 AST が同 SD を共有しつつ、IC 状態だけは実行時可変」が成立する。

| 対象ノード | キャッシュ内容 | 効果 |
|---|---|---|
| `node_field_get`（luastro）| `(hash_cap, slot pos)` | nbody -23%、mandelbrot 不変 |
| `node_gref`（ascheme）| 解決済みグローバル index | strcmp 走査 50ns → indexed load 3ns |
| `node_arith_<op>`（ascheme）| `(PRIM_<op>_VAL, valid?)` | rebind 安全な arith fast path |
| `node_method_call`（abruby）| `(klass, method_serial, body, dispatcher)` | OO ディスパッチの典型 |
| `node_global_get`（**luastro 未実装**）| `(globals shape, slot pos)` | `print(...)` 系で効く見込み |
| `node_call_arg*`（**luastro 未実装**）| 解決済み closure | 自己再帰・library call に効く見込み |

設計指針：
- shape token は **常に変わらないキー**（`hash_cap` のような単調 / 構造値）を選ぶ。
- ヒット時に副情報のキー一致まで確認すること（`hash_cap` は同じでも別形があり得る）。
- ミス時は full lookup → IC 更新の順で書く（IC 内容を先に書いて落ちると古い IC が残る）。

## 4. AOT / SD 生成系

ここが ASTro 固有のレイヤで、最大のレバレッジがある。

### 4.1 `@always_inline` / `@noinline` / `@canonical=` 注釈

- `@always_inline`: ホット制御フローや小型ノード（`node_int_add`, `node_local_get`, `node_seq` 等）で必須。これがないと gcc が SD を独立関数として吐いて間接 call が残る。ascheme で `scm_apply_tail` に付けただけで fib 0.46→0.25 s。
- `@noinline`: 可変長子（`LUASTRO_NODE_ARR` 経由）を持つ親に限定する。**全 call ノードに付けるのは罠**（ascheme `perf.md §4`）— ディスパッチ鎖の inline を断ち切ってしまう。
- `@canonical=BASE`: swap_dispatcher 系列（`node_fixnum_plus → node_integer_plus → node_plus`）が同じ構造ハッシュを持って **同一 SD を共有** する仕組み。abruby / ascheme / asom で活用。

### 4.2 `@ref` インラインキャッシュ

ノード本体内に副情報を埋める。構造ハッシュは 0 を返す。
詳しくは §3。

### 4.3 dlsym で全 SD を引けるようにする

luastro が踏んだ最大の罠：

- **問題**: 子 SD を `static inline` で吐くと dlsym で見えず、子に対する
  `astro_cs_load` が常に miss → host binary の `DISPATCH_*` を呼ぶ →
  `.so` 境界往復で mandelbrot が ~50% を bounce に喰われる。
- **対処**: `luastro_export_sd_wrappers`（`node.c`）が
  生成済み `.c` を post-process し、
  - 中身の `SD_<hash>` を `SD_<hash>_INL` にリネーム（in-source の
    inline 鎖は維持）
  - 末尾に `__attribute__((weak)) RESULT SD_<hash>(...) { return SD_<hash>_INL(...); }`
    の wrapper を追記
- **効果**: mandelbrot 76→65 ms (-14%)、cs hit が 2 → ~80 に増加。

### 4.4 可変長子の SD 焼き込み

`@noinline` 親の子は `LUASTRO_NODE_ARR` 経由（typed `NODE *` 走査では
踏まれない）。`luastro_specialize_side_array` がそのリストを直接舐めて
`astro_cs_compile` する。mandelbrot 65→60 ms (-8%)。

### 4.5 残存 `DISPATCH_*` 計測

実測（luastro AOT-cached）：
- mandelbrot: 2/106 (98% hit)
- nbody: 19/534 (96.4% hit)
- fib: 6/29 (79% hit)

`OPTIMIZE` で `LUASTRO_TRACE_CS_MISS=1` を追えば kind / line / hash が
出る。fib では `node_make_closure` (`@noinline`) が 1 件、残り 5 件は
parser 後処理 `pf_finalize_local_refs` による kind ミューテーション
（`local_get → box_get` 等）でハッシュ整合が崩れるのが原因（要追跡）。
**教訓: `OPTIMIZE` で hash を確定させた後の kind 書き換えは避ける**
（あるいは `has_hash_value` を invalidate する）。

### 4.6 Hopt / Horg の二重ハッシュ（profile-aware）

abruby が採用。Horg は構造ハッシュ、Hopt は構造＋プロファイル情報を
混ぜたハッシュ。
- AOT 側は `SD_<Horg>` を吐く（プログラム間共有可）。
- PG 側は `PGSD_<Hopt>` を吐く（プロファイル特化）。
- `astro_cs_load` は PGSD を先に探し、なければ SD にフォールバック。

### 4.7 Profile-driven type-speculating SD（未実装、最大レバレッジ）

各 `node_int_*` / `node_flt_*` が観測した型・形状に応じ、ガード分岐
（残るが）の slow path を **末尾 deopt 呼び** に置換した SD を吐く。
SD のコードサイズが減り、親 SD への inline が効く。
luastro `todo.md` 筆頭、abruby に部分実装あり。

### 4.8 Direct cross-SD calls（castro）

castro は `SPECIALIZE_node_call` で被呼出し SD のシンボル名を直接
書き出し、ランタイムディスパッチを介さず C コール一発にする。
**`-Wl,-Bsymbolic`** がないと GOT 経由になるので必須。fib_big の
disasm が gcc -O3 とほぼ一致するレベルまでいく。

## 5. パーサレベル書き換え

EVAL body には触らないが AST 形は整えてよい。**新しいノード kind を
増やすのは OK**（その新 kind の EVAL body は素直に書く）。

| 書き換え | 元 | 効果 |
|---|---|---|
| 捕捉ローカル昇格（`local_get → box_get`）| luastro | 非捕捉 slot は frame 直 access、捕捉のみ box |
| 全捕捉解析（function 末で is_captured を確定）| luastro `pf_finalize_local_refs` | 上記の前提 |
| ループ融合（`for i=1,N do sum=sum+i end → node_numfor_int_sum`）| luastro | loop 39ms → 3ms (13×) |
| N 引数 call 特化（`node_call_arg{0,1,2,3}`）| luastro | metamethod / cfunc 経路を hot path から外す |
| Tail-return 畳み込み（return ノードを seq/if に折り畳む → setjmp 不要）| castro `perf.md §1` | fib_big 1.8×、ack/loop_sum/tak は setjmp ゼロ |
| Parse-time call-site index 化（`(call FUNC_IDX ...)` で名前解決を排除）| castro `perf.md §3` | fib_big 3.4× |
| break/continue 静的分類（`node_for_brk_only` / `node_for_brk` / `node_for`）| castro `perf.md §8` | break のないループは setjmp ゼロ → 内側がレジスタ割付可 |
| 文字列定数の parser intern（`LuaString *` 直接埋め）| luastro `runtime.md §11` | SD 内で再 intern 走らない |
| Pre-interned metamethod 名（`__index` 等プロセス起動時に確保）| luastro | 毎アクセスの `lua_str_intern("__index")` を消す |
| **ローカル定数畳み込み（再代入なし・キャプチャ書きなしの local）**| **luastro 検討中** | nbody の `SOLAR_MASS = 4*PI*PI` 系で global lookup 群を消せる |

注意：
- Lua の `load("...")` は別環境なので外側スコープのローカルは見えない →
  ローカル定数畳み込みは安全。
- Ruby 等の `eval` は同スコープが見えるので不可。**意味論を変えない範囲**
  という線は厳守。

## 6. ビルド・リンカ・ランタイムインフラ

| 設定 | 効果 / 理由 |
|---|---|
| ホスト `-O3 -fPIC -fno-plt -march=native` | デフォルト。fno-plt でホストの間接呼び 1 段減 |
| **ホスト `-rdynamic` 必須**（castro で 3.7× 効いた）| ホスト binary のシンボルを SD から dlsym で引くため |
| `code_store/Makefile` 側 `-Wl,-Bsymbolic`（任意）| 同 `.so` 内のシンボル参照を local bind に。GOT 往復排除（castro `perf.md §7`）|
| `dlopen` のパス名キャッシュ回避：`all.so` を世代別 `all.<N>.so` にハードリンクして dlopen | `code_store_quirks.md §1` |
| `all.tmp.so` → `mv all.so` の atomic rename | リンク中の中途半端な `.so` を見せない (`code_store_quirks.md §2`) |
| `dlclose` しない | 既存ノードが古いハンドルの関数ポインタを保持しているため (`code_store_quirks.md §3`)|
| `CCACHE_DISABLE=1` ベンチ時必須 | AOT-1st の cold-start を正しく測るため。デフォルト ccache が hit するとコンパイル時間ゼロに見える |
| 4GB 仮想 pthread スタック | SD frame ~1KB × 深い再帰対策（`runtime.md §12`）|
| 共通パラメタ拡張（`common_param_count = 3`）| luastro / wastro / castro：`(c, n, frame)` を全 EVAL の必須引数化 → reload/store 削減。castro loop_sum 5→2 ms |

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
| `@noinline` を全 call ノードに付ける（ascheme §4）| ディスパッチ鎖の inline を断つ。可変長子を持つ親に限定する |
| try-and-fallback で先に引数 compile（ascheme §15）| 不一致時に再 compile されて木が二重化。**安いキー一致を先に判定**して compile は最後 |
| ベンチで ccache 有効のまま | AOT 1st の数字が嘘になる |
| 再帰インライン（gcc -O3 が fib を 6 段展開している分の差）| ASTro レイヤでは効率的に再帰展開＋束縛管理ができていない。castro `todo.md` の懸案 |
| RESULT 構造体返り | call boundary は速くなるが、leaf node のコストが上がる場合あり (`perf.md §10`)。配置に注意 |
| dlclose して `.so` 入れ替え | 既存ノードが旧ハンドル経由の関数ポインタ保持。世代別 `all.<N>.so` で運用 (`code_store_quirks.md §3`)|

## 9. 大きな未踏領域

1. **Profile-driven 型推測 SD**（最大効果見込み・全言語横断）
2. **残存 `DISPATCH_*` の根絶**（`pf_finalize_local_refs` のような
   parser 後処理 mutation と OPTIMIZE のキャッシュ整合をどう取るか）
3. **再帰インライン**（gcc が踏める展開を ASTro でも踏めるように）
4. **独自ローダー**（`code_store_quirks.md §自前ローダーで解消する`）

---

## ベンチマークの読み方

各サンプルの `make bench` は典型的に以下を比較する：

| 列 | 内容 |
|---|---|
| `<lang>` | 純インタプリタ（`code_store/` を参照しない） |
| `<lang>-AOT-1st` | `-c`：bake + run を 1 プロセスで実行。**gcc コンパイル時間込み** の cold-start 測定。`code_store/` を毎反復削除 + `CCACHE_DISABLE=1` |
| `<lang>-AOT-cached` | bench setup で 1 度 bake（時間外）→ 計測時は plain run で warmed `all.so` を消費 |
| `lua5.4` / `luajit` / `gcc -O3` | 比較対象 |

「AOT-1st が遅い」のは正しい。バイト稼ぎたいなら AOT-cached を見る。

「インタプリタが速い」のは fused loop 等が parser-pass で効いているケース
（luastro の `node_numfor_int_sum` で loop ベンチが lua5.4 より 13× 速い等）。

---

## 関連ドキュメント

- `docs/idea.md` — ASTro 全体設計
- `docs/code_store_quirks.md` — dlopen 周りの罠と暫定対処
- `sample/<lang>/docs/perf.md` — 言語ごとの詳細（ascheme / castro が詳しい）
- `sample/<lang>/docs/done.md` / `todo.md` — 個別言語の進捗・残課題
- `sample/<lang>/docs/runtime.md` — 個別言語のアーキテクチャ
