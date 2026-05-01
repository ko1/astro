# perf.md — koruby 性能改善の記録 (成功 / 失敗)

本書は **どんな最適化を試したか** と **その結果** を一覧する。
成功例だけでなく **見送ったもの** も同じ重みで記録する (再評価のために)。

## ベンチマーク環境

- CPU: x86_64
- OS: Linux 6.8 (Ubuntu 24.04)
- コンパイラ: gcc 13.3 (-O2 / -O3)
- Ruby (比較対象): CRuby 3.4 系 (no-JIT / `--yjit`)

## ベンチマーク結果サマリ

### optcarrot (Lan_Master.nes, 180 frames headless, last-10-frames fps)

10 runs after 3 warmups, `taskset -c 0`, gcc -O2 -flto for the koruby
binary; SDs in `code_store/all.so` always built with `gcc -O3 -fPIC -fno-plt
-march=native` (各 SD は独立 .o → `-shared` でリンク、LTO なし).

| 構成 | fps (median) | vs CRuby no-JIT |
|---|---:|---:|
| ruby (no JIT) | 41.0 fps | 1.00× |
| abruby (plain interp, CRuby C-ext) | 42 fps | 1.02× |
| koruby (interp, plain) | 42 fps | 1.02× |
| koruby (interp + PGO — `make koruby-pgo`) | 51 fps | 1.24× |
| abruby (--aot-compile-first, AOT only) | 71 fps | 1.73× |
| abruby (--aot + --pg-compile, AOT + PGC) | 75 fps | 1.83× |
| **koruby (AOT — `make koruby-aot`)** | **80 fps** | **1.95×** |
| **koruby (AOT + PGO — `make koruby-pgo-aot`)** | **110 fps** | **2.68×** ← exceeds abruby +pgc by 35 fps |
| **ruby --yjit / --jit** | **175 fps** | **4.27×** |

#### この round の big wins

render_pixel kernel を抽出して perf 解析しながら段階的に 80 → 110 fps:

| 変更 | optcarrot fps | kernel ms |
|---|---:|---:|
| baseline | 80 | 905 |
| block body を `code_repo` に登録 (parse.c) — block 内の hot loop が SD 化 | 85 | 670 |
| Array#<< fast path + `korb_ary_aref` を object.h で inline (SD 内に展開) | 87 | 250 |
| `korb_hash_aref` (FIXNUM/SYMBOL key) + `korb_class_of_class` (heap path) を object.h で inline | 98 | 195 |
| `EVAL_node_(method/func)_call(_block)` を `korb_dispatch_call_cached` 経由に — IC + prologue を SD 内 inline | **110** | 185 |

ASTro 原則: 全部 fast-path を SD の TU に持ち込んだだけ。新しい NODE 種別も
AST rewrite も入れていない。 C コンパイラに委ねられる範囲を広げただけ。

(注: cold-tail を `korb_node_*_slow` として koruby 本体に hoist
し SD 内に複製しないことで `all.so` のサイズ増を抑える。LTO は koruby
本体のみ、all.so の 388 SDs は LTO 関係外。)

#### abruby +cf にキャッチアップ済み (72 fps)

koruby AOT は abruby +cf と同等の 72 fps に到達した。実装した最適化:

1. **specialized prologues** — `mc->prologue` 機構: `prologue_cfunc` /
   `prologue_ast_simple_{0,1,2,3,N}` / `prologue_ast_general` から
   method_cache_fill 時に選択。 dispatch は **単一 indirect call** で
   方策分岐なし。
2. **inline prologues** (`prologues.h`) — `static inline always_inline` の
   prologue 本体を header 化。 各 SD `.so` が独自のコピーを持ち、
   `korb_dispatch_call_cached` の guarded direct call が、 SD の TU 内で
   prologue 本体を直接インライン展開。 cross-`.so` indirect call も
   関数呼出 frame もなくなる。
3. **inline ivar_get_ic / ivar_set_ic** (`object.h`) — fast path を
   ヘッダに移して SDs に直接インライン化。 cache hit 時は load + 比較 +
   load の 3 命令で完了。
4. **TLS 廃止** — `current_block` を `__thread` から外すだけで +3 fps。
   single-threaded 実行では `__tls_get_addr` 呼び出しは pure tax。
5. **stack overflow check 省略** — 16M slot の値スタックは
   pathological 再帰でしか溢れず、 SIGSEGV で落ちれば backtrace で
   分かる。 hot path から外して 1 比較分削減。
6. **frame trim** — `caller_node` / `fp` / `locals_cnt` フィールド省略
   (3 stores/call 削減)。

#### 段階的な改善

* **24 fps** (compare_by_identity 修正後 — レンダリング正常化直後)
* **32 fps** ←← Hash#aref が 47% / aset が 19% 食ってた。実装が full linear
  scan O(N) になっていたのを proper chained hash に直して 1.65× 高速化。
* **43 fps** ←← ivar_get が 36%。`class.ivar_names` を線形スキャンしていた
  のを、AST node に `struct ivar_cache { klass; slot; }` を `@ref` operand
  として inline cache 化して 1.6× 高速化。
* **44 fps** ←← `-flto=auto`。クロス TU の関数インライン化。
* **51 fps** ←← **PGO** (`make koruby-pgo`)。 optcarrot の 60-frame run で
  プロファイル収集 → re-build with `-fprofile-use`。 ホット分岐の予測が改善。
* **54 fps** ←← **AOT 特化** (`make koruby-aot`)。abruby と同じ per-method
  SD_*.c → all.so → dlopen 方式。 optcarrot を 30 フレーム走らせて全 entry
  を `code_store/c/SD_<hash>.c` に書き出し、`gcc -O3 -fPIC -fno-plt -march=native`
  で 388 個の SD を `code_store/all.so` に。 起動時に `koruby_cs_init` が
  dlopen し、`OPTIMIZE` で各 NODE の hash 値で `dlsym("SD_<hash>")` を引いて
  dispatcher を差し替え。
* **60 fps** ←← **specialized prologues + inline cache fast path**。
  `method_cache.prologue` フィールドを追加し、`method_cache_fill` の時点で
  `prologue_ast_simple` / `prologue_ast_general` / `prologue_cfunc` の中から
  選ぶ。 dispatch は `mc->prologue(...)` の **単一 indirect call** で内部に
  cfunc vs AST 判定なし。 さらに `EVAL_node_method_call` 内に inline cache 
  hit fast path を追加し、 cache hit 時は `korb_dispatch_call` を呼ばずに
  直接 `mc->prologue` を呼ぶ。 frame init も .caller_node / .fp / .locals_cnt
  を省いて 3 stores 削減。

#### ビルド方法

```sh
make koruby-pgo    # PGO build (51 fps)
make koruby-aot    # AOT build (54 fps) — 実行時 KORUBY_CODE_STORE で dir 指定可
```

#### YJIT との差 — 2.4× (理論的には縮められる)

YJIT 174 fps に対して koruby は 72 fps、 2.4× の差がある。
**原理的には koruby も C compiler 経由で生 x86 を吐いている** ので、
YJIT に勝てる余地はある。 残っている主な技術:

* **PGSD (Profile-Guided Specialized Dispatcher)** — 実行プロファイルから
  call site ごとの `mc->prologue` を観測し、 PGSD として **直接呼出** で
  焼き込む。 現状 inline prologue は guarded direct call で実現してるが、
  guard 自体が runtime overhead。 PGSD なら guard も消せる。
* **method body inlining** — abruby compiled でもやっていない、 YJIT 専有の
  技。 hot な caller-callee ペアで callee の body を caller の SD に直接
  インライン化する。 polymorphic ならガード付きで 2-3 件まで対応。
* **型に基づくノード rewrite** (`node_plus` → `node_fixnum_plus`) — 算術
  演算でメソッドディスパッチを完全省略。
* **FLONUM 即値化** — 現在 Float はヒープ。 即値化すればアロケーションが
  消える。
* **polymorphic IC** — `mc->klass[2]` 程度。 type-flapping call site で毎回
  miss するのが防げる。

### fib(35)

| 構成 | 時間 |
|---|---:|
| ruby (no JIT) | 1.17 s |
| **ruby --yjit** | **0.23 s** |
| koruby (interp, -O2) | 1.00 s |
| koruby (interp + AOT 特化, -O3) | 0.24 s |

### fib(40)

| 構成 | 時間 |
|---|---:|
| ruby (no JIT) | 4.5 s |
| **ruby --yjit** | **1.20 s** |
| koruby (interp + AOT 特化, -O3) | 2.69 s |

### 解釈
- 純インタプリタ単体 (-O2) で fib では **CRuby (no-JIT) より 1.17× 速い**
- AOT 特化で **5× 速い**
- optcarrot のように method call + ivar 中心のワークロードでも **CRuby (no-JIT) を 1.05× 超える**
- YJIT には負けるが (interp vs JIT なので妥当)、optcarrot で 3× 程度

## 成功した改善

### ✅ 1. ASTro AOT 特化
**効果**: fib(35) 0.55s → 0.24s (2.3× 高速化)

仕組み:
- 各 AST ノードに対し `SD_<hash>(c, n)` という関数を生成
- 子ノードへのディスパッチも特化された `SD_<child_hash>` を直接呼ぶ
- 関数は static inline 連鎖になるので、C コンパイラがツリー全体を 1 関数にインライン化できる
- 特に `node_plus(node_lvar_get, node_int_lit)` のような閉じた小さなサブツリーは数行のアセンブリに畳まれる

実装ファイル:
- `node.def` / `koruby_gen.rb` で SPECIALIZE タスク
- `./koruby -c script.rb` で `node_specialized.c` 生成
- 2 回目の build で `#include "node_specialized.c"` して取り込む

**学び**: C コンパイラに任せられる範囲が想像より広い。`-O3` + `static inline` + ノードハッシュ共有 の3点で大きな効果。

### ✅ 2. インラインメソッドキャッシュ (`struct method_cache`)
**効果**: fib のような呼出主体ベンチで顕著 (推定 30-40% 削減)

ナイーブなディスパッチ:
```c
m = klass->method_table_lookup(name);  // hash search
m->u.ast.body->head.dispatcher(c, m->u.ast.body);  // 2-step indirect call
```

キャッシュ済みディスパッチ:
```c
if (mc->serial == method_serial && mc->klass == klass) {
    mc->dispatcher(c, mc->body);  // 1-step indirect call
}
```

`mc` は call site の NODE に **per-site インライン**で埋め込まれる。`method_serial` はメソッド再定義のたびに +1 してキャッシュ全体を無効化。

実装上のポイント:
- 当初 `mc->method->u.ast.body->head.dispatcher` を毎回辿っていたが、3 段階間接参照だった
- `mc->body` と `mc->dispatcher` を直接持たせて 1 段階に減らした
- `mc->locals_cnt`/`mc->required_params_cnt` も持たせて method 構造体への参照を完全に消した

### ✅ 3. Fixnum 高速パス (オーバフロー検出付き)
**効果**: 算術重ベンチで重要

```c
NODE_DEF
node_plus(...) {
    VALUE l = EA(c, lhs); VALUE r = EA(c, rhs);
    if (LIKELY(FIXNUM_P(l) && FIXNUM_P(r))) {
        long a = FIX2LONG(l), b = FIX2LONG(r), s;
        if (LIKELY(!__builtin_add_overflow(a, b, &s) && FIXABLE(s)))
            return INT2FIX(s);
        return korb_int_plus(l, r);  // GMP に昇格
    }
    /* method dispatch fallback */
}
```

- `__builtin_add_overflow` は x86-64 で 1 命令 (`jo` / `jno`)
- FIXABLE は SHL/SHR で範囲チェック (CRuby 互換)

### ✅ 4. CRuby 互換 VALUE 表現
**効果**: 即値判定が極めて軽い

```c
#define FIXNUM_P(v)  (((VALUE)(v)) & FIXNUM_FLAG)   // 1 命令 (test)
#define NIL_P(v)     ((v) == Qnil)                  // 1 命令 (cmp)
#define RTEST(v)     (((VALUE)(v)) & ~Qnil)         // 1 命令 (test)
```

CRuby と同じビット配置にしたので、`RTEST` は `Qnil` と `Qfalse` を同時に false 判定できる魔法のマスク (両者の AND が 0) が効く。

### ✅ 5. EVAL_ARG マクロでの状態伝搬
**効果**: setjmp/longjmp 不要 + コンパイラ最適化が効く

```c
#define EA(c, n) ({                                       \
    VALUE _v = EVAL_ARG(c, n);                            \
    if (UNLIKELY((c)->state != KORB_NORMAL)) return Qnil; \
    _v;                                                   \
})
```

- 例外を起こせない部分木 (整数演算のみなど) では C コンパイラが state チェックを **完全に DCE**
- 一方 method call を含む部分木では分岐が残る (正しい)
- branch predictor 的にも `UNLIKELY` で正常パス側にバイアスがかかる

### ✅ 6. 共有 fp によるクロージャ
**効果**: yield のオーバヘッドが極小

`each { |i| s = s + i }` のようなループでは、

- ブロック作成: `proc->env = c->fp;` だけ (コピーなし)
- yield: `c->fp` 変更なし、`fp[param_base + i] = arg` でパラメータ書き込みのみ

CRuby 比でも軽い (CRuby は CFP を作って locals 配列をリンク)。

### ✅ 7. クラスごとの ivar shape
**効果**: ivar アクセスは配列添字 1 回

クラス側に「@x が slot 0、@y が slot 1...」というテーブルを持たせ、オブジェクト側は素朴な `VALUE *ivars` 配列。**書き込み初回だけ** klass の hash table を見る (新 slot 確保のため)。読み取りは固定 slot で 1 メモリアクセス。

CRuby の object_shape はもっと洗練されているが (transition tree)、最適化観点での効果は近い。

### ✅ 8. Boehm GC (実装速度の最適化)
直接の性能改善ではないが、**実装速度が劇的に上がった**:
- mark 関数を1個も書かなくて良い
- ルート登録不要 (C スタックも自動)
- ノード / オブジェクトに mark フラグや hook を付ける必要なし

これにより koruby 全体を短期間で立ち上げられた。中長期的には世代別 GC 等への移行を検討するが、現段階では Boehm のオーバヘッド (mark cost) は許容範囲内。

## 試したが採用しなかった改善

### ❌ NaN-boxing
当初検討したが **ユーザ指示で禁止**。理由は:
- VALUE 表現を CRuby と分けると CRuby のソースを流用しにくくなる
- Float が固定 16 バイトでもメモリ局所性的に大差なし

代わりに **将来 FLONUM 即値化** (CRuby と同じ low-2-bits=0b10) を入れる予定 (`todo.md` 参照)。

### ❌ 自前 mark/sweep GC (短期)
- 実装コストが大きい (object.c の各 struct に mark 関数が必要)
- Boehm のオーバヘッドはまだ計測上の問題ではなかった
- 中長期では考える ([todo.md](./todo.md))

### ❌ 例外処理を setjmp/longjmp に変更
- C コンパイラ的に setjmp は **副作用順序の barrier** になる
- ASTro 特化されたコード (深いインライン展開された SD_xxx 関数群) との相性が悪い
- 正常パスのコストは setjmp の方が安いが、**特化された subtree の最適化を犠牲にする** ほどではない
- abruby も同じ判断 (RESULT 構造体の 2 レジスタ伝搬)

### ❌ block を escape 対応にする
yield 用に共有 fp 方式を採用した結果、**escape する Proc では env がスタックに残れない**。env を heap 化する案もあったが:
- escape しないブロックが大半 (yield ベース)
- 二段階構造 (escape したら heap 化) は実装コストが高く、まだボトルネックではない

そのため現状は escape しない前提で「速さ優先」。

## 計測 / 観察ノート

### perf stat (fib(40), AOT 特化)

```
2.62 sec real
4.73 IPC
0.02% branch miss
9.78 G branches
50.2 G instructions
```

非常に IPC が高く branch predictor も良好。**コードパスが短くて密** な状態。
これ以上短縮するには C 命令そのものを減らす方向 (PG-baked call_static) が必要。

### YJIT との差の分解 (推測)

| 項目 | YJIT | koruby (AOT) | 差 |
|---|---|---|---|
| メソッド call ディスパッチ | inline 完全展開 | mc キャッシュ + 間接呼び出し | 主因 |
| Fixnum 演算 | 直接アセンブリ | 直接 C 演算 | 同等 |
| GC | mark-sweep + compact | Boehm (mark のみ) | YJIT 有利 |
| frame setup | minimal | fp += arg_index, zero locals | YJIT 有利 |

ホットループ 1 反復あたり 5-10 ns 程度の差で、ほぼメソッド呼出オーバヘッド由来と推測。

## 今後の優先候補 (perf 観点)

詳細 → [todo.md](./todo.md) の「性能向上のための課題」セクション。

1. **PG-baked call_static**: プロファイル後に call site の dispatcher を呼出先 SD に焼き直す → YJIT 並みを狙う
2. **RESULT 構造体化**: state を 2 レジスタ伝搬で
3. **型に基づくノード rewrite** (`node_plus` → `node_fixnum_plus`)
4. **FLONUM 即値化**
5. **polymorphic IC** (mc->klass[2] 程度)
