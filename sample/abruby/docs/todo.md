# abruby TODO: 未実装の Ruby 機能

実装済み機能は [done.md](done.md) を参照。

## optcarrot 対応

benchmark/optcarrot/bin/optcarrot-bench を動かすために必要な機能。
優先度順にリストアップ。

### 致命的（これがないと何も動かない）

- [x] ブロック / yield（block 基盤、`{ ... }` / `do...end`, `yield`, `block_given?`, `next`, `break`, 非ローカル `return`, closure, super block forwarding）
- [x] Proc / lambda / `&block` パラメータ（`Proc.new`, `proc`, `lambda`, `Proc#call`, `Proc#[]`, `Proc#arity`, `Proc#lambda?`, `def f(&blk)`, `f(&proc)` — `->` 記法と `&:symbol` は未対応）
- [x] Fiber（`Fiber.new`, `Fiber#resume`, `Fiber.yield`, `Fiber#alive?` — CRuby Fiber API でスタック管理）
- [x] `case / when`（if/elsif チェーンに desugar、=== メソッド対応）
- [x] `attr_reader` / `attr_writer` / `attr_accessor`
- [x] デフォルト引数 (`def f(a, b = 1)`)

### 重要（主要な処理パスで使用）

- [x] `Integer#times`
- [x] `Integer#[]`（ビットインデックス `num[bit]`）
- [x] Array: `each`, `each_with_index`, `map`/`collect`, `select`/`filter`, `reject`, `flat_map`/`collect_concat`, `fill`, `flatten`, `clear`, `replace`, `concat`, `join`, `min`, `max`, `sort`, `inject`/`reduce`, `uniq`, `compact`, `transpose`, `zip`, `pack`, `all?`, `any?`, `none?`
- [x] Hash: `each`/`each_pair`, `each_key`, `each_value`, `fetch`, `merge`, `delete`, `dup`, `compare_by_identity`
- [x] String: `split`, `strip`, `chomp`, `bytes`, `unpack`, `bytesize`, `start_with?`, `end_with?`, `tr`, `=~`, `%`, `sum`, `to_sym`/`intern`
- [ ] String: `gsub`, `sub`, `match`, `scan`, `[]`, `[]=`
- [x] `super`（bare super で引数転送、super(args) で明示的引数、super() で引数なし）
- [x] `private` / `public` / `protected` / `module_function`（定義のみ、アクセス制御は no-op）
- [x] `until` ループ（基本形は実装済み。`begin...end until` は未対応）

### 中程度（特定機能で必要）

- [x] `eval`（ローカル変数は外部スコープ不可）
- [x] `Struct.new`（最小限、attr_accessor 付きクラス生成）
- [x] `method(:name)`（Method オブジェクト、`call`/`[]` で呼び出し可能）
- [x] `const_get` / `const_set`（動的定数参照）
- [x] `is_a?` / `kind_of?` / `instance_of?`
- [ ] `defined?`
- [ ] クラス変数 (`@@var`)
- [x] 可変長引数 受け取り (`def f(*args)`) — `**kwargs` は未対応。呼び出し側 splat も実装済
- [ ] `&:symbol`（ブロック引数のシンボル記法）

### 標準ライブラリ

- [x] File I/O（join, binread, dirname, basename, extname, expand_path, exist?, readable?, read — CRuby File への facade）
- [ ] Zlib（ROM 解凍）
- [ ] Marshal（シリアライゼーション）

## その他の未実装機能

### 制御構造
- [ ] `for .. in`
- [x] `next`（block body から、値付き/値なし両対応）

### ブロック・Proc・lambda
- [x] `Proc.new` / `proc` / `lambda` — block の heap escape、closure 環境保持
- [x] `&block` 引数 / `&proc_var` 転送
- [ ] `->` lambda リテラル構文
- [ ] `&:symbol` sugar
- [ ] block 内 default / *args / **kwargs パラメータ
- [ ] `_1`, `_2` 番号付きパラメータ
- [ ] `redo`

### メソッド
- [ ] キーワード引数 (`def f(a:, b: 1)`)
- [ ] クラスメソッド (`def self.foo`)
- [ ] `alias` / `alias_method`
- [x] `respond_to?`

### クラス・モジュール
- [ ] `prepend`
- [ ] `extend`
- [ ] ネストしたクラス/モジュール (`class A::B`)
- [ ] シングルトンメソッド / 特異クラス
- [ ] `ancestors`

### 変数・定数
- [ ] クラス変数 (`@@var`)

### ビルトインメソッドの差分

#### Integer
- [x] `even?`, `odd?`, `step`
- [ ] `upto`, `downto`
- [ ] `bit_length`, `between?`

#### Float
- [ ] `nan?`, `infinite?`, `finite?`
- [ ] `truncate`

#### String
- [x] `start_with?`, `end_with?`, `tr`, `%`
- [ ] `[]=`（部分文字列代入）
- [ ] `sub`, `gsub`, `match`, `scan`

#### Array
- [x] `sort`, `compact`, `uniq`, `uniq!`, `shift`, `unshift`, `join`, `inject`/`reduce`
- [x] `transpose`, `zip`, `rotate!`, `flat_map`/`collect_concat`, `pack`
- [x] `min`, `max`, `fill`, `clear`, `replace`, `concat`, `slice`, `slice!`
- [x] `all?`, `any?`, `none?`, `each_with_index`
- [ ] `delete`, `delete_at`, `count`
- [ ] `each_slice`

#### Hash
- [x] `each_key`, `each_value`, `merge`, `delete`, `fetch`, `dup`, `compare_by_identity`
- [ ] `to_a`, `default`

### 未実装クラス
- [x] File（最小限 facade: join, binread, dirname, basename, extname, expand_path, exist?, readable?, read）
- [x] GC（thin facade: disable, enable, start）
- [x] Process（clock_gettime）
- [ ] IO（File の基底）
- [ ] Comparable, Enumerable
- [ ] Numeric (Integer/Float の共通親)

## 高速化

### 実測状況（2026-04-11 時点, `abruby+compiled` vs `ruby`）

- **勝っているベンチ**: while 0.08×, collatz 0.10×, method_call 0.18×, fib 0.21×, gcd 0.21×, nested_loop 0.19×, dispatch 0.30×, ack 0.30×, tak 0.26×, ivar 0.23×, sieve 0.47×, hash 0.70×, object 0.84×
- **同等**: nbody 1.11×
- **負けているベンチ**: mandelbrot 1.93× (Float 特化済だが method dispatch 含む残存コスト),
  binary_trees 1.97× (T_DATA 大量確保が主因), factorial 1.18× (Bignum 乗算), string 2.79× (`s += "x"` による allocation churn)

### 残る課題

### メソッドディスパッチ

- [x] メソッド名インターン化 — 全メソッド名をパース時に intern し、ID ポインタ比較に置換。実装済み
- [x] インラインキャッシュ — `node_method_call` に `struct method_cache` を `@ref` で埋め込み、ヒット時に `abruby_class_find_method` を完全スキップ。グローバルシリアルでメソッド定義・include 時に無効化
- [x] メソッドテーブルのハッシュ化 — `ab_id_table` を hybrid 実装に変更（小テーブルは packed linear、大テーブルは open-addressing Fibonacci hash）。IC ミス時のメソッド探索・builtin クラスのメソッド参照を高速化
- [x] **prologue 関数ポインタ** — argc 特化 4 種 (`prologue_ast_simple_0/1/2/n`) + `prologue_ast_complex` + `prologue_cfunc` + `prologue_ivar_getter/setter`。すべて `always_inline`
- [x] **call node の mtype 特化** (Phase 1) — 初回 cache fill で mc->prologue を確定後、`node_call0/1/2` を `_ast` / `_cfunc` / `_ivar_get/set` へ `swap_dispatcher` (8 特化 kind)。特化版の EVAL では `mc->prologue` 間接呼び出しが消え、prologue 本体がインライン化される。plain モード fib -14%, method_call -15%, ack -11%。compiled モードは SD_ が `EVAL_node_call1` を直接呼ぶため効かない (Phase 2 の課題)
- [x] **polymorphic backoff** — `mc->demote_cnt` (uint8_t) で特化 demote 回数を追跡。閾値 `ABRUBY_CALL_POLY_THRESHOLD` (=2) 超えたら以後 specialize しない。bm_dispatch のような 3-class 循環で swap 往復を防止

### AOT / JIT（ASTro 部分評価）

- [ ] AOT ベンチマーク測定 — `make bench` で plain vs compiled の性能差を把握。compiled テストは通っているので基盤は動作済み
- [ ] ループ選択的 JIT — `dispatch_cnt` 閾値超えノードをバックグラウンドで gcc コンパイル → dlopen → swap_dispatcher
- [ ] specialize でのノードフィールドのロード時解決 — 後述「ノードフィールドのロード時解決」を参照
- [ ] specialize でのメソッドインライン化 — 現在 `node_def` は `@noinline` で specialize をブロック。型フィードバックと組み合わせてメソッドボディを展開できれば、method lookup + frame push/pop が消える
- [ ] specialize でのローカル変数レジスタ化 — gcc LICM はポインタエイリアスで `c->fp[i]` をホイストできない。specialize レベルで C ローカル変数にマッピングすれば回避可能
- [ ] ガード削除 — ループ内の型安定変数に対し、ループ入口で1回だけ型チェックし、ボディ内の FIXNUM_P チェックを除去（Truffle/Graal の speculation + deopt パターン）

### CTX フィールドの frame 移行

- [ ] `c->self`, `c->fp` 等を `abruby_frame` に移動し、CTX から廃止する。frame push/pop で自動的に save/restore されるため、`node_method_call` の手動 save/restore が不要になりコードが簡潔になる。`c->current_frame->self` / `c->current_frame->fp` と間接参照が1段増えるが、gcc が基本ブロック内でレジスタに保持できれば実質コストは小さい。specialize でのローカル変数レジスタ化と組み合わせれば LICM 問題も回避可能。**検証 (2026-04-14)**: `abruby_frame` に `saved_self`/`saved_fp`/`saved_cref` を追加し dispatch_method_frame に `recv_self` パラメータを渡す方式を試行。frame struct の 24B 増大により fib +23%, method_call +17%, tak +12% のリグレッション。frame サイズがキャッシュに収まらないケースでコストが顕在化。specialize でのレジスタ化なしでは見送り

### インタプリタ改善

- [ ] スーパーインストラクション — 頻出パターン（`while(lvar < const)`, `lvar = lvar + num` 等）を融合ノードに。AOT 無効時のインタプリタ高速化
- [x] NodeHead スリム化 — `parent`(8), `jit_status`(4), `dispatch_cnt`(4) を除去、フィールドを cold/warm/hot ゾーンに再配置。72B→56B (NODE: 144B→128B)。dispatcher と union データが常に同一キャッシュラインに収まる。ASTroGen に `ASTRO_NODEHEAD_PARENT` 等の条件付きガードを追加
- [ ] 末尾呼び出し最適化 — `return method_call(...)` パターンを検出しフレーム再利用。再帰のスタック消費削減

### メモリ・GC

- [ ] abruby_object の ivar inline slots をクラスごとに可変化 — 現在は全オブジェクト固定 4 slots (`ABRUBY_OBJECT_INLINE_IVARS`)。`klass->ivar_shape.cnt` を見て alloc 時にちょうどいいサイズを確保し、`extra_ivars` を不要にする。flexible array member で `ivars[]` をクラスの shape に合わせれば、ivar 0 個のクラス (binary_trees) は 0 bytes、ivar 7 個のクラス (nbody Body) は 56 bytes inline で heap alloc なし。クラス再オープンで shape が伸びた場合のみ realloc
- [ ] ノードのアリーナアロケータ — 個別 malloc → バンプポインタ。同一スコープのノードが隣接しキャッシュ局所性向上
- [ ] VALUE スタック遅延 GC マーク — 現在 10,000 スロット全体をマーク。`fp + frame_size` までに限定

### オブジェクトシステム

- [x] ivar インラインキャッシュ — `node_ivar_get/set` に `struct ivar_cache *@ref` を埋め込み、
  `(klass, slot)` ガードで `obj->ivars.entries[slot]` を直接参照。ivar/nbody で大幅に効く
- [ ] シェイプベース ivar アクセス — ivar の名前線形探索を固定オフセットに。CRuby のオブジェクトシェイプと同様の手法。
  現在の ivar IC はキャッシュエントリが object 毎の entry slot を示すが、
  shape なら class 単位で共有できる。binary_trees 等の大量オブジェクト生成で更なる高速化余地
- [ ] case/when ジャンプテーブル — 現在 if/elsif チェーンに desugar。整数リテラル when はジャンプテーブル化

### 機能追加（最適化の前提条件）

- [ ] ブロック / yield + インライン化 — optcarrot 必須。specialize でブロック呼び出しを展開できればイテレータのオーバーヘッド除去。**method inlining の基盤が先に必要**。20260412 に iterator ごとに融合ノードを手書きする方式を試したが、N イテレータで N ノードが必要になり scale せず、かつフレーム共有が `binding` 等で破綻するため差し戻し (詳細: `docs/report/20260412_phase1_block_speedup.md`)。正しい方向は PE + inlining で (1) cfunc iterator または (2) AST で書き直した iterator を inline 展開

## ノードフィールドのロード時解決

### 背景

現在の SPECIALIZE（ASTro 部分評価による C コード生成）は、ノードフィールドの型によって扱いが異なる:

- `uint32_t`（`params_cnt`, `arg_index` 等）→ **即値として埋め込み済み**（`fprintf(fp, "%u", n->u.node_xxx.field)`）
- `ID`（`name` 等）→ **ノードから毎回読む**（`fprintf(fp, "n->u.node_xxx.name")`）
- `NODE *`（子ノード）→ dispatcher 関数を直接呼ぶ（解決済み）
- `@ref`（`method_cache`, `ivar_cache` 等）→ ノードから毎回読む

ID がノードから読まれている理由: CRuby の `ID` は `rb_intern` でプロセスごとに動的に割り当てられる値であり、AOT コンパイル時には値が確定しない。コードに即値として埋め込むと、別プロセスでの .so 再利用ができなくなる。

### 目的

specialize された関数（SD_xxx）から、`EVAL_node_xxx` に渡す引数のうちノードフィールド経由のものをすべて解決し、**EVAL 関数が `NODE *n` を受け取る必要をなくす**。

これにより:
- レジスタが1本空く（`n` 引数が不要）
- ノードへのポインタチェイスがなくなる
- EVAL 関数がコンパクトになり、インライン化されやすくなる

### 設計: static 変数 + per-SD init 関数

各 SD_xxx に対応する `SD_init_xxx` を生成し、dlopen 後にホスト側が呼ぶ。

**生成コード側** (SPECIALIZE が出力):
```c
// SD_xxx ごとに必要な ID・@ref を static 変数として宣言
static ID __sd_xxx_name;
static struct method_cache __sd_xxx_mc;

// init: 文字列名を受け取り、ホストの rb_intern で解決
void SD_init_xxx(struct abruby_machine *abm, ID (*intern)(const char *)) {
    __sd_xxx_name = intern("foo");
    // method_cache は初期状態のまま (実行時に fill される)
}

// specialize された dispatcher
RESULT SD_xxx(CTX *c, NODE *n) {
    // n は受け取るが、EVAL には渡さない
    return EVAL_node_func_call(c,
        __sd_xxx_name,    // static 変数 (ロード時に解決済み)
        2,                // uint32_t 即値 (コンパイル時に解決済み)
        5,                // uint32_t 即値
        &__sd_xxx_mc);    // static 変数 (@ref)
}
```

**ホスト側** (dlopen 後):
```c
// SD_xxx を dlsym するタイミングで、init も一緒に呼ぶ
dispatcher = dlsym(handle, "SD_xxx");
init = dlsym(handle, "SD_init_xxx");
if (init) init(abm, rb_intern);
```

ポイント:
- .so 側が必要な ID の文字列名を自分で持つ（ELF の動的リンカと同じ発想）
- ホスト側は `abm` と `rb_intern` を渡すだけで、個々の SD が何を必要としているか知らなくてよい
- 複数の SD が all.so に蓄積される構成でも、SD 単位で独立に init できる

### 解決対象のフィールド型

| フィールド型 | 現状 | ロード時解決後 |
|---|---|---|
| `uint32_t` | 即値埋め込み済み | 変更なし |
| `NODE *` (子ノード) | dispatcher 直接呼び出し済み | 変更なし |
| `ID` | `n->u.xxx.name` (毎回ロード) | static 変数 (init で解決) |
| `@ref` (method_cache 等) | `&n->u.xxx.mc` (毎回ロード) | static 変数 |
| `line` (デバッグ用) | `n->line` | 即値埋め込み可 |

全フィールドが解決されれば、EVAL 関数のシグネチャから `NODE *n` を除去できる。

### 発展: copy & patch 方式による即値パッチ

static 変数方式では ID は依然としてメモリロード1回が必要。これを真の即値（`mov rdi, 0x12345`）にするには、copy & patch（Xu & Kjolstad, OOPSLA 2021）の **patch 部分**を応用する。

copy & patch は本来「事前コンパイル済み stencil を並べてプログラムを組み立て（copy）、穴を埋める（patch）」コンパイル手法。abruby は SPECIALIZE でカスタム C コードを生成するので copy 部分は不要だが、patch の仕組み — ELF relocation を活用して .o 中のプレースホルダを実行時の値で書き換える — はそのまま使える。

手順:
1. SPECIALIZE で生成した C コードにプレースホルダ定数を埋め込む
2. gcc で .o にコンパイル（.so ではなく）
3. .o からマシンコードと ELF relocation エントリを抽出
4. ロード時: コードを実行可能バッファにコピーし、relocation に従い ID・クラスポインタ等の実行時の値でパッチ
5. mprotect で PROT_READ|PROT_EXEC に変更

利点:
- ELF relocation を使うので、自前のリロケーションテーブルを発明する必要がない
- ID が真の即値になり、メモリロードが完全に消える
- CPython JIT（PEP 744, Brandt Bucher）が同じ手法で実績あり

static 変数方式で `n` の持ち回りを消した後、プロファイルでメモリロードがボトルネックと判明した場合に検討する。

### 自前ローダーの設計検討

即値パッチ方式を採用する場合、dlopen/dlsym は使わず .o を自前でロードする。dlopen の枠に縛られない分、abruby に最適化した設計が可能だが、以下の設計判断が必要。

#### dlopen/dlsym vs 自前ローダーのトレードオフ

| | dlopen/dlsym | 自前ローダー |
|---|---|---|
| シンボル解決 | `.gnu.hash` で O(1)、数十万シンボルでも高速 | 自前でハッシュテーブル等を実装する必要あり |
| 定数のパッチ | 不可（リンカが relocation を解決済み） | ELF relocation をそのまま使える → 真の即値 |
| デバッグ | gdb/perf がそのまま使える | perf map ファイル等の対応が必要 |
| 実装コスト | ほぼゼロ（OS 提供） | ELF パース + メモリ管理を自前で書く |

**重要**: .o の relocation オフセットは .o 内の位置を指す。リンカが .so を作る過程でコード配置が変わるため、.o の relocation 情報を dlopen 済みの .so に適用することはできない。つまり「dlopen で楽にシンボル解決 + 自前で即値パッチ」のいいとこ取りは不可。即値パッチをやるなら、ローダー全体を自前にする必要がある。

#### 設計の決定事項

**1. コンパイル単位**
- (a) メソッドごとに個別の .o → ロードが独立、差分更新が容易
- (b) 全メソッドを1つの .o にまとめる → ビルドが単純、関数間の最適化が効く可能性
- (c) 現在の all.so と同様に蓄積 → 既存の仕組みとの互換性

**2. シンボル解決**
- (a) .o の `.symtab` + `.strtab` をパースし、自前ハッシュテーブルを構築
- (b) シンボル名に規則を設けてインデックスで直接算出（例: `SD_{node_hash}` → 配列のインデックスに変換）
- (c) SPECIALIZE 時にシンボルのオフセットをメタデータとして記録しておく

**3. メモリ管理**
- (a) .o 全体を1回 mmap して、関数のオフセットで参照
- (b) 関数ごとに個別の実行可能バッファにコピー（断片化するが、関数単位の入れ替えが可能）

**4. relocation の処理**
- ELF の `.rela.text` セクションを読み、relocation type に応じてパッチ
- x86-64 で主に必要な relocation type:
  - `R_X86_64_64`: 64bit 絶対値（ID, ポインタ → `movabs` の即値）
  - `R_X86_64_32`: 32bit 絶対値
  - `R_X86_64_PC32`: 32bit PC-relative（関数間呼び出し）
- `libelf` を使うか、ELF ヘッダを直接読むか（構造は単純）

**5. 参考実装**
- **CPython JIT (PEP 744)**: `Tools/jit/` に stencil 抽出 + パッチのコードあり。Python スクリプトで .o をパースし、C ヘッダとしてシリアライズ
- **OpenJ9 Shared Classes Cache**: AOT コードの validation + relocation の 2 フェーズロード
- **Linux kernel module loader**: `.ko` (relocatable .o) をカーネル空間にロードし、シンボル解決 + relocation パッチ

## prologue リファクタリング

### 背景と動機

現在の `dispatch_method_frame` は全メソッドタイプ (AST / CFUNC / IVAR_GETTER / IVAR_SETTER) を1つの巨大な関数で処理している。問題:

1. **引数チェックがない** — argc が多すぎても少なすぎてもエラーにならず、多い場合はスタック上の他フレームの領域を踏み潰す危険がある
2. **specialize 時のコード膨張** — `dispatch_method_frame` は `static inline` なので、specialize すると全 call site に全タイプの分岐コードが展開される。ivar アクセスの call site にも CFUNC 分岐のコードが出る
3. **caller/callee の責務が不明確** — frame push/pop、self save/restore、argc の Qnil 埋めが caller と callee に散在

### 設計方針: CRuby 式の prologue 関数ポインタ

CRuby では `vm_call_iseq_setup` / `vm_call_cfunc` / `vm_call_ivar` 等のメソッドタイプ別呼び出し関数を `struct rb_callcache` に記録し、cache hit 時に直接呼び出す (参照: `vm_insnhelper.c`)。同じ方式を abruby に導入する。

```
現状:
  call site → dispatch_method_frame → mtype 分岐 → frame push → body or cfunc → frame pop

提案:
  call site → mc->prologue(c, call_site, mc, argc, arg_index)
              ↑ メソッドタイプごとの専用関数ポインタ
```

### prologue 関数のシグネチャ

```c
// 非 block 版
typedef RESULT (*method_prologue_t)(
    CTX *c, NODE *call_site,
    const struct method_cache *mc,
    unsigned int argc, uint32_t arg_index);

// block 版
typedef RESULT (*method_prologue_blk_t)(
    CTX *c, NODE *call_site,
    const struct method_cache *mc,
    unsigned int argc, uint32_t arg_index,
    const struct abruby_block *blk);
```

EVAL の `(CTX*, NODE*)` 固定シグネチャでは argc 等を渡せないため、prologue は EVAL とは別の関数ポインタにする。`method_cache` に `prologue` と `prologue_blk` の2フィールドを追加し、`method_cache_fill` でメソッドタイプに応じて設定する。

### 6つの prologue 関数

| 関数 | mtype | frame push | block | argc チェック |
|---|---|---|---|---|
| `prologue_iseq` | AST | する | なし | argc != params_cnt → ArgumentError |
| `prologue_cfunc` | CFUNC | する | なし | argc != params_cnt → ArgumentError |
| `prologue_ivar_getter` | IVAR_GETTER | しない | - | argc != 0 → ArgumentError |
| `prologue_ivar_setter` | IVAR_SETTER | しない | - | argc != 1 → ArgumentError |
| `prologue_iseq_with_block` | AST | する | BREAK demote | 同上 |
| `prologue_cfunc_with_block` | CFUNC | する | BREAK demote | 同上 |

各 prologue は、現在の `dispatch_method_frame` / `dispatch_method_frame_with_block` から該当メソッドタイプの処理を抽出したもの。

- **frame push/pop**: iseq/cfunc の prologue 内で行う。ivar はフレームなし (現状と同じ)
- **fp/cref の save/restore**: iseq prologue 内で行う
- **RESULT_RETURN skip-count**: iseq/cfunc prologue 内で処理
- **RESULT_BREAK demote**: _with_block 版のみ
- **self の save/restore**: prologue には含めない。call site 側の責務 (現状と同じ)

### call site の変更

```c
// node_method_call (cache hit):
VALUE save_self = c->self;
c->self = recv_val;
RESULT r = mc->prologue(c, n, mc, params_cnt, arg_index);
c->self = save_self;

// node_func_call (cache hit):
RESULT r = mc->prologue(c, n, mc, params_cnt, arg_index);

// block 付き (cache hit):
RESULT r = mc->prologue_blk(c, n, mc, params_cnt, arg_index, &blk);
```

`dispatch_method_frame` は `mc->prologue(...)` への1行 wrapper になるか、最終的に削除。

### block 付き ivar の扱い

`prologue_blk` は ivar 系で NULL。block 付き ivar 呼び出し (`obj.x { ... }`) は非 block 版の `mc->prologue` にフォールバック。ivar accessor は yield しないので BREAK は起きず安全。

### apply/splat の扱い

`node_func_call_apply` / `node_method_call_apply` は argc が実行時に変わるため、prologue の argc チェックが毎回走る。将来の PG specialize で call site の argc が定数化されれば、prologue ごとインライン展開されてチェックが消える。

### dispatch_method_with_klass (miss パス / super / method_missing)

一時的な `method_cache` を構築して `mc->prologue(...)` を呼ぶ。現状と同じ構造で、`dispatch_method_frame` の代わりに `mc->prologue` を呼ぶだけ。

### PG specialize との連携 (将来)

PG specialize では prologue の関数ポインタが定数になるので、gcc がその prologue 関数をインライン展開する。例:
- `prologue_ivar_getter` がインライン化 → `obj->ivars[slot]` への直接アクセスのみのコードが出る
- `prologue_iseq` がインライン化 → frame push + body dispatcher 呼び出しのコードが出る (mtype 分岐なし)
- method inlining と組み合わせれば body の中身まで展開される

### 実装手順

各ステップで `make test && make debug-test` が通ること。

1. `context.h` に prologue typedef を追加、`method_cache` にフィールド追加 (NULL 初期化)
2. 6つの prologue 関数を `node.def` に書く (現在の dispatch_method_frame から抽出、まだ未使用)
3. `method_cache_fill` で method type に応じて prologue/prologue_blk を設定
4. `dispatch_method_frame` の中身を `return mc->prologue(...)` に置換 ← **ビッグバン**
5. `dispatch_method_frame_with_block` も同様に置換
6. 各 prologue に argc チェックを追加 (ArgumentError)
7. (任意) hot path で `mc->prologue(...)` を直接呼び出し、wrapper 関数を削除

Step 1-3 は非機能変更 (prologue を書いて設定するだけ、まだ呼ばれない)。Step 4 が本番切り替え。

### 発展: handler 方式 (cache check も callee 側)

prologue リファクタリングの先にある、さらに踏み込んだ設計。call site のコードを間接呼び出し1個に削減する。

#### call site を究極まで小さくする

現状の call site は cache check (klass + serial 比較) + hit/miss 分岐がある。これを handler に全部任せる:

```c
// call site のコード (これだけ)
mc->handler(c, n, mc, recv, argc, arg_index)
```

handler は初期状態で `generic_handler`（method lookup → cache fill → dispatch）。初回呼び出し後、handler 自体を prologue_iseq 等に書き換える。prologue 側が cache check も担う:

```c
prologue_iseq(c, call_site, mc, recv, argc, arg_index) {
    klass = AB_CLASS_OF(recv);
    if (mc->klass != klass || mc->serial != abm->method_serial)
        return generic_handler(...);  // cache miss → 再 lookup
    // cache hit: frame push → body → frame pop
}
```

#### 算術演算ノードの統一

この方式の最大の利点は、**算術/比較ノードのバリエーション爆発を解消できる**こと。

現在 `+` 演算のために `node_plus` / `node_fixnum_plus` / `node_fixnum_plus_slow` / `node_fixnum_plus_overflow` / `node_integer_plus` / `node_flonum_plus` / `node_flonum_plus_slow` の 7 ノードが存在し、実行時に `swap_dispatcher` で AST を書き換えて切り替えている。`-`, `*`, `/`, `%`, `<`, `<=`, `>`, `>=`, `==`, `!=` も同様で、node.def の大部分がバリエーションで埋まっている。

handler 方式なら `node_plus` 1つで済む:

```c
NODE_DEF
node_plus(CTX *c, NODE *n, NODE *left, NODE *right, uint32_t arg_index,
          struct method_cache *mc@ref)
{
    VALUE lv = EVAL_ARG(c, left);
    VALUE rv = EVAL_ARG(c, right);
    return mc->handler(c, n, mc, lv, rv, arg_index);
}
```

handler が型ガードと method redefinition チェックを自由に組み合わせる:

```
handler_fixnum_plus:
  if (FIXNUM_P(lv) && FIXNUM_P(rv) && serial == cached)
    → tagged add (overflow check 付き)
  else
    → generic_plus_handler (method lookup → dispatch_method_with_klass)

handler_flonum_plus:
  if (FLONUM_P(lv) && FLONUM_P(rv) && serial == cached)
    → flonum add
  else
    → generic_plus_handler
```

利点:
- **node.def が劇的にシンプルになる** — 算術/比較/等値で ~50 ノード削減、`node_plus` 1つ + handler 群に統一
- **swap_dispatcher による AST 書き換えが不要** — handler の差し替えだけで型特化が完結
- **型ガードの自由度が高い** — `(Fixnum, Float)` 等の混在ケースも handler で表現可能
- **PG specialize で handler が定数化** → gcc が fixnum fast path をインライン展開、型ガードが定数畳み込みで消える
- **メソッド再定義への対応が自然** — serial 不一致で generic に fallback、再 fill で適切な handler に戻る

#### 検討: handler 統一の限界と現実的な落とし所

算術演算の handler 統一には **レジスタ vs fp のトレードオフ** がある。

現在の `node_fixnum_plus` は子ノードの EVAL_ARG 結果を C レジスタ上で直接演算する (fp を経由しない):
```c
VALUE lv = EVAL_ARG(c, left);   // レジスタ
VALUE rv = EVAL_ARG(c, right);  // レジスタ
return lv + rv - 1;             // fp 触らない
```

一方 handler 方式では、handler のシグネチャが統一されているため、引数を fp 経由で渡す必要がある:
```c
c->fp[arg_index] = EVAL_ARG(c, arg0);  // fp に store
mc->handler(c, n, mc, 1, arg_index);    // handler が fp から load
```

handler シグネチャに値を直接渡す (`handler(c, n, mc, recv, arg0)`) ことで回避できるが、引数の数ごとに handler 型が変わり（0/1/2/それ以上 × block 有無 = 8 種類以上）、prologue との組み合わせで複雑さが爆発する。

**現実的な方針**:

1. **メソッド呼び出し** (`obj.foo(x)`, `super`, `yield`) → handler 方式を適用。fp 経由は元々やっているのでオーバーヘッドなし
2. **算術/比較の fast path** (`a + b` の fixnum/flonum) → 現行の専用ノード方式を維持。レジスタで完結する利点が大きい
3. **算術ノードのバリエーション整理** → handler 統一ではなく、現行の 7 段階 (plus / fixnum_plus / fixnum_plus_slow / fixnum_plus_overflow / integer_plus / flonum_plus / flonum_plus_slow) を 2〜3 段に整理する方向で
4. **将来**: method inlining が入り call 自体が消せるようになれば、算術の handler 統一も再検討可能。inlining で handler がインライン展開されれば、fp 経由のコストも C コンパイラの最適化で消える可能性がある

## call 特化 Phase 2 (callee body 特化)

Phase 1 で mtype ごとの prologue 特化 (node_call*_ast, _cfunc, _ivar_*) は実装済 (done.md 参照)。
Phase 2 は「特定 method の body / dispatcher / required_params_cnt を定数化した dispatcher を
生成」する方向。現状 abruby には runtime JIT がないため、naruby の `astro_jit_replace_dispatcher`
機構を移植する案が本命。

### Phase 1 が compiled モードで効かない問題

SD_ (ASTroGen 生成の specialize 済 C コード) は `EVAL_node_call1(c, n, recv, recv_dispatcher, ...)`
をハードコードで呼ぶ。runtime に `swap_dispatcher` で `EVAL_node_call1_ast` に切り替えても、SD_ は
元の関数名を焼き込んでいるため変わらない。compiled モードが benchmark で plain と同じか若干遅いまま
なのはこれが原因。

Phase 2 での解決策:
- [ ] `c->abm->method_serial` を guard 条件とする特化 dispatcher の runtime 生成
- [ ] naruby の L1 キャッシュ (.so 生成 + ロード) 機構を astro 本体に移植
- [ ] `NaRubyNodeDef::Node::Operand#build_specializer` 相当の拡張を `abruby_gen.rb` に追加し、
  `struct method_cache *` のフィールドを定数化できるようにする

### 設計上の約束 (Phase 1 → Phase 2 への橋渡し)

特化 dispatcher は **`NODE_DEF` で定義する** (`static RESULT dispatch_call1_ast(c, n)` のような
plain 関数にしない)。理由:
- `NODE_DEF` が生成する `EVAL_node_call1_ast` は子 dispatcher (recv/arg0) をパラメータとして受け取る
  → ASTroGen の specialize 機構がこれらを SD_* 関数ポインタに置き換えて inline 化できる
  (例: `recv = node_self` なら SD_ で self の読み取りが定数化される)
- plain 関数で `recv->head.dispatcher(c, recv)` と書くと runtime 値の関数ポインタ読み取りで
  specialize 不可。今は plain モードで同等でも Phase 2 を組む際に不都合

icache 圧は 8 kind × 6 自動生成関数 = 48 関数で問題なかった。将来ブロック付き (_b × 3 × 3 = 9) まで
拡張する際も許容範囲と見込むが、実測で回帰が出るようなら ASTroGen への kind alias 機構追加を検討。

### 現時点での制約

abruby のメソッド動的再定義 (test_method_override) と相性の問題で closed-world AOT は採らない。
Phase 1 の効果が十分でない場合にのみ Phase 2 を検討。

Phase 1 未対応の残りノード (優先度低):
- [ ] `node_call` (argc >= 3) の mtype 特化
- [ ] `node_call0_b/1_b/2_b` (block-carrying) の mtype 特化 — **検討済 (2026-04-15)**:
  試作したが `dispatch_setup_args` が支配コストで効果薄、かつ 6 kind 追加で他のベンチ (tak +6%,
  sieve +2%) に icache 圧由来の回帰が出たため revert。iterator の高速化には Phase 2 の callee
  body 特化が必要
- [ ] `node_apply_call` (splat) の cfunc 特化

## ランタイム・内部実装

- [x] ~~abruby オブジェクトの free（現在リーク前提）~~ → `RUBY_DEFAULT_FREE` で GC sweep 時に解放
- [x] メソッド/ivar/定数テーブルの動的拡張 → `ab_id_table` (hybrid hash table) に移行済み
- [ ] スタックオーバーフロー検出
