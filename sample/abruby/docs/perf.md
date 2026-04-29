# abruby 性能チャレンジ記

abruby (a bit Ruby) の性能取り組みの章史。本ドキュメントは
**abruby 固有の経緯と設計判断** を時系列で残す。フレームワーク中立な
教訓は `docs/perf.md`（リポジトリ ルート）にまとめてあるので、
レイヤ横断の教訓を読みたい場合はそちらを参照のこと。

abruby は **CRuby の C extension** として実装されており、`VALUE` /
GC / Prism パーサ / 主要ビルトインクラス（Array / Hash / Bignum /
Float / Regexp 等）を CRuby と共有している。**ホスト言語のランタイム
インフラを丸ごと借りつつ、ASTro の部分評価で評価部だけを高速化する**
という珍しい設計。これが効きどころと制約の両方を生んでいる。

## 0. 全体像と現在地

`benchmark/report/20260420-8a3fd73.txt` (best of N、Linux x86_64、
CRuby 4.0.2、`-O3 -fPIC -fno-plt -march=native`):

| Bench | ruby | yjit | abruby+plain | abruby+aot | abruby+pgc | yjit/ruby | aot/ruby | pgc/ruby |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| while | 1.171 | 0.188 | 1.406 | 0.141 | 0.142 | 0.16× | 0.12× | 0.12× |
| collatz | 1.301 | 0.199 | 1.006 | 0.173 | 0.163 | 0.15× | 0.13× | 0.13× |
| nested_loop | 1.027 | 0.130 | 0.936 | 0.179 | 0.181 | 0.13× | 0.17× | 0.18× |
| gcd | 1.142 | 0.421 | 0.748 | 0.312 | 0.272 | 0.37× | 0.27× | 0.24× |
| ivar | 1.218 | 0.186 | 0.843 | 0.330 | 0.286 | 0.15× | 0.27× | 0.23× |
| fib | 1.330 | 0.183 | 0.845 | 0.379 | 0.320 | 0.14× | 0.29× | 0.24× |
| ack | 1.380 | 0.194 | 1.481 | 0.384 | 0.341 | 0.14× | 0.28× | 0.25× |
| tak | 1.407 | 0.203 | 1.581 | 0.518 | 0.449 | 0.14× | 0.37× | 0.32× |
| method_call | 1.452 | 0.235 | 0.790 | 0.332 | 0.284 | 0.16× | 0.23× | 0.20× |
| dispatch | 1.574 | 0.199 | 0.912 | 0.490 | 0.476 | 0.13× | 0.31× | 0.30× |
| nbody | 0.182 | 0.088 | 0.158 | 0.153 | 0.139 | 0.48× | 0.85× | 0.77× |
| mandelbrot | 0.907 | 0.342 | 0.889 | 0.673 | 0.389 | 0.38× | 0.74× | 0.43× |
| binary_trees | 0.457 | 0.255 | 0.765 | 0.696 | 0.665 | 0.56× | 1.52× | 1.45× |
| string | 0.318 | 0.266 | 0.658 | 0.645 | 0.690 | 0.84× | 2.03× | 2.17× |
| factorial | 0.682 | 0.423 | 0.919 | 0.809 | 0.747 | 0.62× | 1.19× | 1.09× |

形勢:

- **整数ループ／ループ系**（while / collatz / gcd / nested_loop）で
  YJIT を上回るか肉薄。tagged 算術 + parser-side fixnum specialization
  が綺麗に効く領域。
- **浮動小数算術**（mandelbrot / nbody）は AOT で ruby 比 0.74〜0.85×、
  PGC で 0.43〜0.77×。inline flonum + flonum 系 swap_dispatcher が効くが、
  YJIT との差は埋まらない。
- **再帰**（fib / ack / tak / method_call / dispatch）は YJIT に大差で
  負ける（fib で 1.7× 遅い）。method 境界のオーバヘッドと、method body
  inlining の不在が主因。
- **GC / allocation 律速**（binary_trees / string / factorial）は ruby より
  遅い領域もある。`String#+` 等の文字列再 alloc は CRuby ヘ丸投げで
  改善余地が小さい。

abruby+pgc が optcarrot で **86.5 fps**（ruby 45.6 fps の +90%）まで
出る。詳細は §11.5。

---

## 1. 算術 specialization の家系図 (`swap_dispatcher` ベース)

最初に整えた基盤。Prism から AST を組む段階では `node_plus(left, right, arg_index)`
（汎用 `+` ディスパッチ、`arg_index` はメソッドフォールバック時の引数
配置先）のままにせず、parser がパターンマッチして
`node_fixnum_plus / node_fixnum_minus / node_fixnum_mul / node_fixnum_div`
に降ろす。実行時に型不一致が観測されたら `swap_dispatcher` で
上位互換ノード（`node_integer_plus` Bignum 対応 → `node_plus` メソッド
ディスパッチ）に降格する。

```
node_fixnum_plus   ← 両 Fixnum を期待（AST 生成時のデフォルト）
   ↓ 1 度でも非 Fixnum を観測
node_integer_plus  ← Bignum も含む整数を期待（`rb_big_*` 直呼び）
   ↓ 1 度でも非 Integer を観測
node_plus          ← 汎用、メソッドディスパッチで `+` を引く
```

比較系（`<`, `<=`, `>`, `>=`）も `node_fixnum_lt` 系列、等値系
（`==`, `!=`）も `node_fixnum_eq` 系列、剰余 (`%`) も `node_fixnum_mod`
が同じ降格チェーンを持つ。さらに後から **Float 専用パス** を追加した
(§4)：`node_flonum_plus/minus/mul/div/lt/le/gt/ge`。`node_fixnum_*_slow`
で両オペランドが Float と分かったら `swap_dispatcher` で flonum 系に
**昇格**する。

設計上のポイント:

- `swap_dispatcher` はノード kind を入れ替えるが、**`@canonical=node_plus`**
  注釈で AST 構造ハッシュ（Horg）を共有する（ASTroGen 側のサポート）。
  AOT bake 後のコードストアでは `SD_<Horg>` を 1 個共有し、ノード自体は
  実行時 kind を切り替えても OK という二重構造。
- 算術ノードのバリエーション爆発（`+` だけで現状 7 ノード）はトレード
  オフを抱えている。**handler 方式でひとまとめにする案**は試行検討
  したが算術 fast path のレジスタ伝搬を手放すため棚上げ
  （`docs/todo.md` 末尾「handler 方式」参照）。

`bdda1f2 → 0ac3342 → 1bd1b4b → 66bebd0` の commit 系列で
`node_replace`（AST 書き換え）から `swap_dispatcher`（dispatcher
だけ書き換え）に移行した。AST 書き換えは EVAL 中に親が古いポインタを
保持していると stale read が起きる。dispatcher swap なら ALLOC ポインタ
不変で安全。

**教訓**: 子ノードの rewrite で親フィールドが更新される可能性がある
箇所（`EVAL_ARG` 中で子が自分自身の swap で replace される場合）は、
**DISPATCH に渡された stale なローカルパラメータではなく、
ノードのフィールドから再ロード** する。CLAUDE.md にも明記している
（rewrite 注意事項）。

## 2. メソッドインラインキャッシュ (`@ref method_cache`)

**最初に大効きした最適化**。各 call ノード（`node_func_call` /
`node_method_call`）に `struct method_cache` を `@ref` で埋め込み、
hit 条件:

```c
mc->klass == AB_CLASS_OF(recv) && mc->serial == abm->method_serial
```

`mc->body` と `mc->dispatcher` をキャッシュすることで method 構造体経由
の間接参照 2 段を省略する。`abm->method_serial` は per-machine
（per-`abruby_machine` インスタンス）で、メソッド定義 / `include` で
インクリメントされ全 IC を一発無効化する。

設計上の選択:

- IC ハッシュ寄与 0（`@ref` の `abruby_gen.rb` 拡張）。同形 AST が
  AOT 後も同 SD を共有しつつ、IC 状態だけは実行時可変。
- メソッド名は parse 時に CRuby `rb_intern` で intern 済 `ID`
  ポインタ比較。`09d8c4f` で `const char *` 比較を全廃。
- miss path（`node_method_call_slow`）は `noinline` 付き。最初は
  `cold` 属性も付けていたが、polymorphic dispatch（bm_dispatch）の
  icache を悪化させる（cold セクションに飛ばされて L1 hit が落ちる）
  のが観測されたので **`cold` を外して `noinline` のみ** にした
  (`7fcb86a`、bm_dispatch -16%)。

**教訓**: `cold` を miss path に付けるかどうかは **caller の polymorphism**
で決まる。monomorphic ならゼロ近似でいいが、3 クラス循環など
polymorphic 寄りの場合は cold セクションへの jmp が icache miss を生む。

## 3. インライン flonum と Float 算術 fast path

mandelbrot / nbody が method dispatch 経由 Float 算術で ruby 比
3.34× / 2.01× だった (baseline)。
**CRuby の Flonum タグ**（`xxxx_xx10` の即値、IEEE-754 を 3 ビット
ローテートで埋める）を活用し、`node_flonum_plus/minus/mul/div/lt/le/gt/ge`
を追加。`node_fixnum_*_slow` が両オペランドの Float を観測したら
`swap_dispatcher` で flonum 系に昇格 (`8ceb814`)。

`flonum_*_calc` は `abruby_float_new_wrap` で **Flonum 範囲外の
double のみヒープ Float に退避**。inline flonum で済む範囲は
レジスタ完結で `vmulsd` 1 命令。

**効果**: mandelbrot -44%、nbody -36%（一発で最大の改善）。

#### 後日 revert: `node_fixnum_*` body から inline Flonum 分岐を削除 (`7e5e712`)

`node_fixnum_plus/minus/...` には初期実装で **inline Float×Float ブランチ**
を埋めていた（fixnum_slow 経由せずに直接 Float 演算する fast path）。
mandelbrot のような Float 一辺倒なベンチには効くが、整数ドミナント
(optcarrot) の SD で `*`, `<`, `+` の各 site のコードサイズが ~3 倍
（90 → 25 insn）に膨らみ、整数しか踏まないコードまで Flonum decode/encode
の不要コードを持つことになっていた。

`swap_dispatcher` で動的に `node_flonum_*` に昇格する仕組みが既にある
ので、`node_fixnum_*` body から Flonum 分岐を全削除。clang optcarrot
PGC: 86.97 → 89.67 fps (+3.1%)。pure-Float hot loop は最初の数イテレーション
で swap が走るのでわずかに不利になるが許容範囲。

**教訓**: 多態用 fast path を **手書き fall-through** で抱えるか、
**動的 swap** で別 kind に振るかは慎重に選ぶ。動的 swap が常用の
仕組みとして成立しているなら、手書き fast path はコードサイズ ×
SD 数だけ icache に効く。abruby は動的 swap を選んだ。

## 4. ivar インラインキャッシュ + shape-based 直接インデックス

`ab_id_table` ベースの ivar アクセスは per-object alloc + 線形探索が
重かった。`abruby_object` を再構成 (`dc796f1` → `e68d947`):

```c
struct abruby_object {
    struct abruby_class *klass;
    enum abruby_obj_type obj_type;
    uint16_t ivar_cnt;
    VALUE *extra_ivars;          // heap overflow (NULL for ≤4 ivars)
    VALUE ivars[4];              // inline 4 slots
};
```

クラス側は `ivar_shape` テーブル（ID → slot index）を持ち、全 instance
が同じ layout を共有。ノードに埋めた `struct ivar_cache { klass, slot }`
で gate して、`obj->ivars[slot]` または `obj->extra_ivars[slot - 4]`
に直接アクセス。

**効果**: ivar -32%、binary_trees -20%、object 系全般改善。nbody の
Body クラスは 7 ivars あるので 4 inline + 3 extra heap。

#### shape_id 化 MVP（`76b43ff`、`c3a9e36`）→ pre-shape より -9% optcarrot で revert pending

`ic->klass` ポインタ比較を `shape_id` 整数比較に置き換えるバリアント
を試した（`T_DATA.flags` の上位 32 bits に `shape_id` を埋める）。
fast path のガードは整数比較 1 発に縮むはずだが、

| metric | pre-shape | post-shape | Δ |
|---|---:|---:|---:|
| optcarrot fps | 84 | 76.26 | **-9%** |
| cycles | 11.2B | 12.0B | +7% |
| L1 dcache miss | 362M | 381M | +5% |

退化要因:

1. `ivar_set` の transition (pre_shape → post_shape) は overflow slot で
   slow path 確定。初期化時に @a..@p (16 個以上) を set すると毎回
   `abruby_shape_for` 呼びとハッシュ lookup が走る。optcarrot の PPU /
   CPU state 初期化で目立つ。
2. fast path の `ic->shape_id != 0` 追加チェックで +1 insn/call。
3. ivar_set fast path に transition 処理が入って分岐コードが長くなった。

**結論（保留）**: shape_id 単独ではダメ。**SPECIALIZE 時に shape_id を
リテラル化** するか、**Overflow slot の fast-path grow** を入れて
やっと win に転じる見込み。`benchmark/report/20260417-shape-mvp.md`
に詳細。

**教訓**: 「shape-based に切り替えれば速い」は前提条件付きで、
literal-fold や transition fast-path までセットで実装しないと、
むしろ hot path が長くなって退化する。

## 5. attr_reader / attr_writer の dispatch フルスキップ

`bi.x` (attr_reader) も通常の method_cache dispatch 経由で frame build
+ indirect call を払っていた。`abruby_class_add_method` で body の
AST 形（`node_ivar_get` 単独、または `node_ivar_set(name, lvar_get(0))`）
を検出して method type を `ABRUBY_METHOD_IVAR_GETTER/SETTER` に
マークし、`method_cache_fill` で receiver klass の shape から slot を
**事前解決**。`dispatch_method_frame` の先頭で type をチェックし、
**frame 構築を完全スキップ** して `obj->ivars[mc->ivar_slot]` を
直接アクセス。

**効果**: nbody -6% 追加（既に大きく改善済の上で）。

**教訓**: getter/setter の AST 形を parse 時に検出して**メソッドタイプ
として保存** すれば、dispatch path で type-driven にフレーム省略できる。
動的言語でも getter/setter は静的検出が安価。

## 6. ab_id_table ハイブリッド化 + inline storage

メソッド／定数／ivar shape／グローバル変数すべてを動的ハッシュ
テーブル `ab_id_table` で管理。線形探索は tiny table（< 4 entry）
だと hash より速いが、大テーブル（builtin クラスの 30+ methods）
だと遅い。

`939f878` で：

- `capa ≤ 4` は **packed linear** （cnt 個の連続エントリを線形比較）
- `capa > 4` は **open-addressing + Fibonacci ハッシュ**
- struct に `inline_storage[4]` を埋め込み、小テーブルは別途 alloc 不要

**効果**: bm_object -28%（Point 作成で ivars table の別 alloc が消えた）。

**教訓**（`docs/perf.md §2` にも反映）: 小〜中容量の動的ハッシュは
**inline 4 + heap overflow** + **小テーブルは linear scan / 大テーブルは
hash** のハイブリッドが標準形。alloc 削減と cache locality の二段で
効く。

## 7. RESULT state ビットフラグ化と非ローカル return

すべてのノード評価の戻り値は `RESULT { VALUE value; unsigned int state; }`
で、state は 2 レジスタ（rax + rdx）に乗る。最初は state を enum
（NORMAL / RETURN / RAISE / BREAK）で持っていたが、メソッド境界の
`UNWRAP_ALSO(RETURN, r)` が比較 + 分岐 + RESULT 再構築で 4-6 cycles
かかっていた。

`760aebc` で **bit flag** に変更：`RETURN=1, RAISE=2, BREAK=4, NEXT=8`。
メソッド境界の RETURN catch は

```c
r.state &= ~RESULT_RETURN;     // 1 命令
```

になる。while -16%、他薄く効く。

#### 非ローカル return の RESULT skip-count 方式 (`1201eba`)

ブロック内 `return` は **block を定義した メソッドから** 脱出する
（block 内 method 境界を飛び越える）。素朴に実装するなら全 frame 上に
`frame_id_counter` / `return_target_frame_id` を載せて毎 push で
counter を bump する設計だが、block を持たない call の hot path に
追加コストを払うことになる。

採用した設計：`RESULT.state` の **上位ビット**（`RESULT_SKIP_SHIFT=16`）
に「何回 method boundary で skip するか」を埋め込む。

- ブロック状態判定: `abruby_in_block(c) ≡ c->current_block_frame == c->current_frame`。
  yield が `c->current_block_frame = c->current_frame` をセットしてから
  block body を評価。block 内から method を call すると新 frame が push
  され `c->current_frame` がシフトするため、自動的に false に切り替わる。
- `node_return`:
  - block 内: `c->current_frame` から `c->current_block->defining_frame`
    まで `prev` を辿って間の method frame 数を skip-count に埋める。
  - それ以外: skip-count = 0（最も近い method boundary で catch）。
- method 境界:
  - `r.state == 0` ならゼロチェックだけで終了（**非ブロック経路は
    完全フリー**）。
  - `r.state & RESULT_RETURN` で skip > 0 → `RESULT_SKIP_UNIT` を 1 つ
    減らして propagate。
  - skip == 0 → `RESULT_RETURN_CATCH_MASK` で RETURN + skip bits を
    まとめてクリア。

**教訓**: 「frame push 時に追加コスト 0」を目標に、**例外的経路だけ
chain walk を払う** 設計。同種は wastro `RESULT { value, br_depth }`、
castro の `escape_target` ポインタ。多段 escape を持つ言語全般に
普遍的なパターン。

## 8. NodeHead スリム化 (72B → 56B)

NodeHead から `parent`(8) / `jit_status`(4) / `dispatch_cnt`(4) を除去し、
フィールドを **cold / warm / hot zone** に再配置 (`705b8cb`)。
- cold (SPECIALIZE / HASH / GC のみ): hash_value, dispatcher_name, rb_wrapper
- warm: flags, kind, line
- hot (union データと同キャッシュライン): dispatcher

NODE 全体は 144B → 128B。**dispatcher と union データが常に同一
キャッシュラインに収まる** よう配置することで、`(*dispatcher)(c, n)`
からの即時の operand 読みが L1 hit を保つ。

ASTroGen に `ASTRO_NODEHEAD_PARENT` 等の条件付きガードを追加し、
他のサンプル（node の親ポインタを必要とする言語）は影響なし。

**教訓**（`docs/perf.md §1.1 候補`）: NodeHead は必須最小限。
**dispatcher と union 先頭を同キャッシュライン** に置くため、
warm zone のサイズを意識して設計する。

## 9. Specialized call kinds + prologue 関数ポインタ (Phase 1)

call の mtype 特化を 2 階層で実装 (`706b1cf` → `8a1c51d` → `4a9cf9a`)。

### 9.1 prologue 関数

`static inline __attribute__((always_inline))` でマークされた小型関数。
argc 特化 prologue（`prologue_ast_simple_0/1/2/n`、`prologue_ast_complex`、
`prologue_cfunc`、`prologue_ivar_getter/setter`）を `method_cache_fill`
で type に応じて選択し、`mc->prologue` に格納。

argc 特化 prologue は argc を **compile-time 定数** として持つので、
`ctx_update_sp(frame.fp + REQ)` 等が定数畳み込みで消える。

### 9.2 特化 call kind

`node_call0/1/2_{ast,cfunc,ivar_get,ivar_set}` を `NODE_DEF` で定義。
初回 cache fill 時に `maybe_specialize_call_kind` が `n->head.kind` を
generic から特化版に `swap_dispatcher`。特化版の EVAL は
`mc->prologue` 間接呼び出しをせず、対応する prologue を **名前で直接呼ぶ**。
`always_inline` で展開されるため間接呼び出しが消える。

設計上の選択:

- 特化版を `NODE_DEF` で定義する理由：ASTroGen の specialize 機構が
  子 dispatcher（recv = `node_self` など）を SD\_ 関数ポインタとして
  代入し inline できる。`static RESULT dispatch_call1_ast(c, n)` のような
  plain function だと `recv->head.dispatcher(c, recv)` が runtime 値の
  読み取りになり specialize 不可。
- `swap_dispatcher` は最初 NodeKind 全体を入れ替える実装だった
  （`8a1c51d` で 48 個の auto-generated 関数が増えた）が、後に
  **dispatcher pointer だけ書き換えて kind は元のまま** にする方式に
  戻した（4a9cf9a）。kind 多重化は icache pollution
  （ブロック付き 6 kind 追加で tak +6%, sieve +2% の回帰）の元なので。

### 9.3 polymorphic backoff

cache miss 時は `specialized_call_miss` が `mc->demote_cnt` を bump し、
generic dispatcher を直接呼ぶ（recv/args の double EVAL を回避）。
`demote_cnt >= ABRUBY_CALL_POLY_THRESHOLD (=2)` で megamorphic 判定、
**以後 specialize しない**（bm_dispatch のような 3 クラス循環で
swap_dispatcher が往復するのを防止）。

**効果**: plain mode で fib -14%, method_call -15%, ack -11%。

### 9.4 Phase 1 が compiled モードで限定的な理由

SD\_（ASTroGen 生成の AOT C コード）は `EVAL_node_call1(c, n, recv,
recv_dispatcher, ...)` を **ハードコード** で呼ぶ。runtime に
`swap_dispatcher` で `EVAL_node_call1_ast` に切り替えても、SD\_ は元の
関数名を焼いているため変わらない。**compiled モードが plain と同等
〜若干遅い** のはこれが原因。Phase 2 (callee body 特化、`docs/todo.md`
末尾) で解決される予定。

**教訓**: 「runtime swap_dispatcher」と「parse-time specialize で
SD\_ に焼く」が **二重の状態を持つ**。後者が優先するので、後者を
profile に応じて再 bake する仕組み（PGC）が無いと runtime swap が
compiled モードに伝わらない。

## 10. PGO: Hopt / Horg 2 段ハッシュ (`e8c666c`, 2026-04-17)

コードストアの鍵を 2 系統に分離。

| | 計算 | 特性 | 用途 |
|---|---|---|---|
| **Horg** (origin) | kind canonical 名 + 非 storageless operand の構造 | swap_dispatcher でも不変 | AOT の SD_ 鍵、PGC 索引キーの一部 |
| **Hopt** (optimized) | 実 kind 名 + 全 operand（baked prologue 等を含む） | profile 状態を反映 | PGC の SD_ 鍵 |

- `NODE_DEF @canonical=node_plus` で specialized variant を canonical
  family に所属させる（`node_fixnum_plus`, `node_flonum_plus` →
  canonical `node_plus` → Horg 同値）。
- `NodeHead` に `hash_org` / `hash_opt` の 2 スロット + キャッシュフラグ。
- PGC 時のみ `abruby_pgo_prologue_name_for(n)` などが Hopt に寄与
  （storageless operand を hash する）。

コードストア:
```
code_store/
  c/SD_<Horg>.c          ← AOT (--compile モードで生成)
  c/SD_<Hopt>.c          ← PGC (--pgc モードで at_exit 時に生成)
  hopt_index.txt         ← (Horg, file, line) → Hopt の多対一マップ
  all.so
```

`astro_cs_load(n, file)`: `(Horg, file, line)` キーで Hopt 引き →
`SD_<Hopt>` dlsym。なければ `SD_<Horg>` dlsym (AOT フォールバック)。

#### `--pgc` を 1 パス + 終了時 bake に簡素化 (`dde8e55`, 2026-04-17)

従来の 2 パス（stub → profile → recompile）を廃止し、`abm.eval` 完了
直後に entry を bake。`Hopt != Horg`（profile が乗ったもの）は PGC、
`Hopt == Horg` は AOT。eval 中の例外で bake がスキップされる
（crashed run の profile を永続化しない）。次プロセスは on_parse で
cs_load(entry, file) を呼び、索引経由で PGC を即採用。

**効果**: fib AOT (0.40s) → PGC (0.34s) で約 14% 改善、
optcarrot plain 45.6 fps → AOT 72.0 fps → PGC 86.5 fps。

**教訓**（`docs/perf.md §4.6` に反映済）: PGO は **2 段ハッシュ + canonical
family** で AOT/PGC を共存させる。 1 パス bake は実装も運用も単純で、
2 パスより組みやすい。

## 11. 蓄積された小さな打ち手

### 11.1 メソッド名の parse-time intern (`4df32bb`)

`const char *` 比較を全廃し、parse 時に CRuby の `rb_intern` で `ID` に
変換、ID ポインタ比較で済ませる（`09d8c4f`）。`ab_id_table` が
ID キーを直接受けるので lookup も早い。

### 11.2 Bignum 演算の `rb_big_*` 直呼び出し

`integer_*_calc` は最初 `rb_funcall(x, rb_intern("+"), 1, y)` で method
lookup + frame を毎回作っていた。`rb_big_plus/minus/mul/div` を直接
呼ぶ形に切り替えて factorial -7%。

### 11.3 String#+ / String#* のバッファ事前確保

`s += "x"` で `rb_str_new` + `rb_str_cat` が realloc を撃っていた。
`rb_str_buf_new(total_size)` で結果サイズを最初に指定 (`e9b8ff8`)。
string -20%。

### 11.4 GC mark の immediate skip + `:initialize` キャッシュ (`dce0e88`)

perf が GC で 25%+ を示していた。`abruby_data_mark` で
`RB_SPECIAL_CONST_P(v)` なら `rb_gc_mark` 呼ばずスキップ。
`Class#new` 経路の `rb_intern("initialize")` を `id_cache.initialize`
に格納して per-call の intern を消す。binary_trees -4%, object -6%。

### 11.5 Object#== の identity 短絡 (`6ce43f2`)

`@left == nil` のような `==` が method dispatch 経由で `node_arith_fallback`
3.19% を喰っていた（perf）。`node_eq` / `node_neq` の入口で `lv == rv`
なら即 `Qtrue`/`Qfalse`。`node_arith_fallback` でも method が
`ab_object_eq` に解決された場合は dispatch せず identity 比較。
binary_trees -2% 追加。

**教訓**: メソッドが Object#== にフォールバックすることが多いなら、
identity check で先回りすると hot path から method dispatch が消せる。

### 11.6 EVAL_* を `static inline` に

ASTroGen 生成の `EVAL_node_*` は `static` のみだったが、`static inline`
を付けると specializer が出力する SD\_ ラッパーから cross-node inline
が効きやすくなる（ack -21%, tak -10%, fannkuch -10%）。

ただし `always_inline` を付けると plain mode で **+20% 悪化**
（code bloat で icache miss）。**`inline` キーワード止まり** が正解。

**教訓**: `always_inline` は **小型 leaf** と **rare-but-essential
制御フロー** に絞る。全 EVAL に付けるのはアンチパターン。

### 11.7 caller_node 方式のフレーム

各 frame が「自分がどこから呼ばれたか」（call-site の NODE）を記録
する。backtrace は `f->caller_node->line` + `f->prev->method->name` を
ペアにして組み立てる。**frame に source_file を持つ必要がない**
（caller_node の line だけで足りる、file は entry にぶら下げる）ため
frame サイズが縮む。

`PUSH_FRAME(node)` / `POP_FRAME()` マクロで dispatch_method_frame を
経由しないインライン例外（0除算等）も backtrace に位置を記録。

### 11.8 frame.klass 削除 (`760aebc`)

`super` 解決のためだけに全メソッド呼び出しで 1 store されていた。
`abruby_method::defining_class` に所属クラスを移し、
`find_super_method` は `frame->method->defining_class->super` から walk。
frame は `{prev, method, caller_node, block, self, fp, entry}` 7 ワード。

### 11.9 CTX フィールドの frame 移行

`c->self`, `c->fp` を `abruby_frame` に移動し CTX から廃止。frame
push/pop で自動的に save/restore されるため、`node_method_call` の
手動 save/restore コードが消える。以前は frame +24B のリグレッションが
出ていたが、frame サイズを抑えた最終形（上記 7 ワード）で導入し性能
維持。

### 11.10 `current_block_frame == current_frame` 不変条件

「block 実行中かどうか」を `c->current_block_frame == c->current_frame`
の 1 比較で判定する設計。yield が `current_block_frame = current_frame`
にセット → block 内 method call で frame が push されると自動的に
false に切り替わる → block 内 method 内では「block 状態」を持ち越さない。
**`dispatch_method_frame` は block 状態を一切 save/restore しなくて済む**。

**教訓**（`docs/perf.md §1.x 候補`）: 「現在の文脈は X か」の状態を
別フィールドで持たず、**既存フィールド間の不変条件** で表現できる
場合は、save/restore コストがゼロになる。

## 12. 試したが破棄／保留したアイデア (cautionary tales)

| § | 試行 | 結果 | 理由 |
|---|---|---|---|
| (報告) | `node_lvar_fixnum_inc` 等 super-instruction (`i += k` 融合) | revert | specializer が PE で展開するので compiled mode では無意味、plain mode の伸び小 |
| (報告) | T2 `needs_frame = false` による frame elision | 不採用 | 「呼び先が raise しない」を一般判定不可。代わりに T2' で frame.klass 削除 |
| (報告) | `EVAL_*` の `always_inline` | revert | plain mode で ack +20% 悪化。`inline` キーワードのみが正解 |
| (報告) | `DISPATCH_*` の `hot` 属性 | revert | gcc が指示通り cluster しても icache 改善せず |
| §4 | shape-based ivar_cache MVP | -9% optcarrot 退化 | transition slow path + fast path +1 insn。SPECIALIZE 焼き込み無しでは win に転じない |
| §1 | `swap_dispatcher` で kind 全体を入れ替え | revert | 48 個の auto-generated 関数が増え icache pollution。dispatcher pointer のみ書き換える方式に変更 |
| §9 | `node_call0_b/1_b/2_b` (block 持ち) の mtype 特化 | revert | tak +6%, sieve +2% の icache 圧由来回帰。`dispatch_setup_args` が支配的でそもそも mtype 分岐コストが小さい |
| (`a611ef0`) | Cluster bake (method inlining 自動化) | abandon (branch 保存) | bm_method_call -18.5%、bm_ivar -15.9% は出るが optcarrot は ±2%。size_cap >= 10 で IPC 崩壊 (always_inline で gcc 限界)。**method inlining 単体では実コードで 2× 不可** |
| (報告) | 反復子の融合ノード手書き (`Integer#times`, `Array#each`, `Array#map` + block) | revert | N iterator で N ノード必要、scale せず。frame 共有が `binding` 等で破綻。正解は PE + method inlining |
| (報告) | `Object#==` を含む dispatch 経路の identity-shortcut なし | -3% binary_trees | identity 短絡を入れて解決 (§11.5) |
| §11.6 | `cold` 属性を miss path に | revert | polymorphic dispatch で cold セクション飛びが icache miss 増 |
| (報告) | T1 自己再帰 devirtualize | pending | astrogen 側変更が大きく着手せず |
| (報告) | T5 PG 特化 (klass 定数焼き込み) | pending | `--pg` モード整備が前提、後に Hopt/Horg 2 段ハッシュ + bake observed prologue として一部実現 (`dde8e55`) |

#### Cluster bake (method inlining) の詳細所見 (`a611ef0`)

`-O3 -fPIC` で `static inline always_inline` の callee body を caller の
.c に co-emit する仕組み（`PGIB_<HOPT>` 命名）を作って測ったが:

| size_cap | optcarrot wall | IPC |
|---:|---:|---:|
| 0 (off) | 2.727 | 2.51 |
| **5 (default)** | **2.725** | **2.58** |
| 10 | 2.858 | 2.51 |
| 100 | 3.172 | 2.21 |
| uncapped | 2.978 | 2.17 |

**uncapped で hot function PGSD が 1580 行 → 5358 行**（asm 4579 行
→ 25826 行）に爆発。**always_inline は大きな body の inline には向かない**。
`inline` に下げると baseline まで下がる（cluster 外で gcc heuristics
が落ちるため）。

optcarrot で効かない理由:

1. ASTro は method 内を既に inline 済（1 method = 1 関数に畳まれている）。
   method inlining で追加で稼げるのは method 境界 (~20 insn) だけ。
2. ivar accessor は既に specialize 済（`prologue_ivar_getter`, frame 無し
   直読）。inline する余地が無い。
3. **libruby (`rb_ary_*`, GC, alloc) が profile 29%**。abruby 外で触れない。
4. hot method 同士の境界を inline すると register allocator が崩壊。

**教訓**（`docs/perf.md §8` cautionary tales に追加）: method inlining
**単体** で 2× は出ない。**ASTro の AST inline + ivar/arith specialize
が既に深い** ので、method 境界に残っているのは call-overhead の数 %
だけ。次の桁の改善は libruby を内製化するか、PGO 駆動の deep speculation
（型推測 SD で Array#push の cfunc を inline する等）が必要。

## 13. 残課題と未踏

### 13.1 SD\_ 内の `EVAL_node_call1` ハードコード (Phase 1 → Phase 2)

§9.4 の通り。SD\_ の文字列レベルで kind 名を焼いているので、runtime
swap が反映されない。

**解候補**:
- runtime に `c->abm->method_serial` を guard 条件とする特化 dispatcher
  を生成
- naruby の L1 キャッシュ機構を astro 本体に移植
- `struct method_cache *` のフィールドを定数化できるよう `abruby_gen.rb`
  を拡張

### 13.2 ロード時ノードフィールド解決

`uint32_t` (params_cnt 等) は即値埋め込み済、`NODE *` は dispatcher
直接呼び。残る `ID`（メソッド名等）は `n->u.xxx.name` で毎回 load。
`SD_init_xxx(abm, intern)` を作って **SD\_ ロード時に static 変数へ
解決** する案 (`docs/todo.md`「ノードフィールドのロード時解決」)。
EVAL シグネチャから `NODE *n` を除去できればレジスタ 1 本空く。

発展形は **copy & patch (Xu & Kjolstad, OOPSLA 2021)** の patch 部分応用：
ELF relocation を活用して .o 中のプレースホルダを実行時に書き換え、
ID を真の即値（`movabs`）にする。CPython JIT (PEP 744) と同じ手法。

### 13.3 ローカル変数のレジスタ化

`c->fp[i]` の pointer aliasing で gcc LICM が hoist できない。
specialize レベルで C ローカル変数にマッピングすれば回避可能だが、
EVAL body の構造変更が必要で ASTro 原則「EVAL body 不可侵」と衝突する。

### 13.4 `ctx_update_sp` 廃止 → `entry->stack_limit` ベース

prologue / node_scope / yield / Proc#call で毎回
`if (new_top > c->sp) c->sp = new_top` を実行している。prologue だけで
4 insn / call。**設計案** は `abruby_entry::stack_limit` に「locals
+ params + call-arg temps の最大使用量」を設定し、GC mark は frame
chain を walk して各 `[fp, fp + entry->stack_limit)` を mark
（`docs/todo.md` 参照）。実装は efa8e43 リグレッション解消後に着手予定。

### 13.5 Profile-driven 型推測 SD（最大未踏）

`docs/perf.md §4.7` で全サンプル横断の最大未踏領域として上げられている。
abruby は基盤（Hopt/Horg, observed prologue bake）まで揃っているので
最も着手しやすい立場。

## 14. ASTro user 視点の総括（abruby 編）

### 14.1 効いた打ち手の構造

最大の効きどころは **「parse 時に kind を生成して swap_dispatcher で
動かす」** パターン。fixnum/integer/flonum 系列、call の mtype 特化、
attr_reader/writer 検出、すべて framework に手を入れず node.def +
parser だけで完結している。

ascheme `perf.md §10.3`、wastro `perf.md §11.4` と同じ結論：

- **framework (`lib/astrogen.rb`) を触らずに、parser と node.def だけで
  2-7× を取れる**。abruby は plain → AOT で 4× 程度、PGC でさらに
  +10〜20%。

`@canonical=` 注釈による **AST 構造ハッシュの共有** が支えになっていて、
swap_dispatcher で kind が動的に変わってもコードストアは同 SD を
再利用できる。

### 14.2 abruby 特有の制約：ホスト言語ランタイムの呼び出しコスト

abruby は CRuby C extension なので、

- **VALUE 表現を CRuby と共有** → Bignum / Float / String のヒープ
  オブジェクトを CRuby と渡し合える。
- **Array / Hash / Regexp / Bignum 演算を CRuby に丸投げ** →
  実装コスト激減、ただし `rb_ary_push` などの **cfunc 呼び出しが
  hot path に残る**。optcarrot で profile 29% が `rb_ary_*` 等。
- **GC を CRuby と共有** → `T_DATA` ベースの統一構造で Ruby 側からも
  abruby オブジェクトが見える。**ただし mark 関数が cfunc** なので
  hot loop の immediate skip 程度しか抑えられない。

これは「hot path で libruby に触れた瞬間、abruby の最適化が頭打ちに
なる」 ことを意味する。method inlining 探索 (§12 cluster-bake) が
optcarrot で効かなかった本質的な理由でもある。

### 14.3 parser-pass と PGO を分けて回す価値

abruby が他サンプルと違うのは **plain / AOT (Horg) / PGC (Hopt) の 3 系統が共存**
すること。コードストアが世代別 `all.<N>.so` で運用され、Horg と Hopt
がインデックス越しに共存する設計（§10）は、

- **parser-pass で確定する Horg は 1 度焼けば全プロセス共有**
- **profile を反映する Hopt は run ごとに at_exit で追記、次プロセスで自動採用**

という運用プロファイルを実現している。Hopt にだけ profile-aware な
prologue 焼き込みが入るので、AOT path はクリーンなまま PGC path で
hot site を盛り上げる、という分離が綺麗にできた。

### 14.4 YJIT に届かない領域

| 種別 | 例 | YJIT との差 | 残された道筋 |
|---|---|---|---|
| 純粋再帰（method 境界が hot） | fib / ack / tak | 1.5〜2× | method inlining + lvar レジスタ化（framework 改修） |
| iterator + block | each / times | YJIT 並み〜やや上 | Phase 2 callee body specialize |
| GC / alloc 律速 | binary_trees / string | 同等以下 | libruby を内製化、または ASTro 内で alloc fast-path |
| Float-heavy | nbody / mandelbrot | 1.5〜2× | lvar レジスタ化、SROA を阻害する fp aliasing 解消 |

これらはいずれも `lib/astrogen.rb` 側の改修（recursive specializer /
inlining / typed signatures）が前提。abruby 単体で詰めるのは構造的に
困難。

### 14.5 まとめ

abruby は **「ホスト言語のランタイムを再利用しつつ、評価部だけ ASTro
で部分評価する」** というモデルで：

- 整数ループ系は YJIT に肉薄／凌駕（`while`, `gcd`, `nested_loop`）
- 再帰・iterator・GC 律速は YJIT に大差で負ける
- optcarrot は ruby +90% (45.6 fps → 86.5 fps)、yjit (186 fps) には届かない

の構図に落ち着いた。次の桁を取るには framework 側に踏み込むか、
ホスト言語との境界を再交渉する必要がある。

---

## 関連ドキュメント

- `CLAUDE.md` — 開発ガイド + アーキテクチャ概要
- `docs/runtime.md` — ランタイムデータ構造の全体像
- `docs/done.md` — 実装済み機能（高速化セクションが本ドキュメントの
  サマリ）
- `docs/todo.md` — 未着手最適化と設計メモ（特に「ノードフィールドの
  ロード時解決」「prologue リファクタリング」「call 特化 Phase 2」
  「PG / AOT 結合とコードストア鍵設計」）
- `benchmark/report/` — 各 phase の生 bench data
  - `optimization_report.md` — 2026-04-11 大改修の経緯（§1〜§13 相当）
  - `20260417-shape-mvp.md` — shape_id 化 MVP の退化分析（§4 後半）
  - `20260415_optcarrot_c_mode_perf.md` — optcarrot warm run の perf 分析
  - `20260420-8a3fd73.txt` — 最新ベンチ（§0 表の元データ）
- `docs/report/20260419_method_inlining_exploration.md` — method
  inlining cluster-bake の探索記録（§12 cluster bake の出典）
- リポジトリ ルート `docs/perf.md` — 全サンプル横断の知見集
- `sample/ascheme/docs/perf.md` / `sample/wastro/docs/perf.md` /
  `sample/castro/docs/perf.md` — 同種チャレンジ記
