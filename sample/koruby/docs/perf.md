# perf.md — koruby 性能改善の記録 (成功 / 失敗)

本書は **どんな最適化を試したか** と **その結果** を一覧する。
成功例だけでなく **見送ったもの** も同じ重みで記録する (再評価のために)。

## ベンチマーク環境

- CPU: x86_64
- OS: Linux 6.8 (Ubuntu 24.04)
- コンパイラ: gcc 13.3 (-O2 / -O3)
- Ruby (比較対象): CRuby 3.4 系 (no-JIT / `--yjit`)

## ベンチマーク結果サマリ

### optcarrot (Lan_Master.nes, 180 frames headless)

| 構成 | wall time | vs no-JIT |
|---|---:|---:|
| ruby (no JIT) | 5.59 s | 1.00× |
| **koruby (interp, -O2)** | **5.32 s** | **1.05×** ✅ |
| ruby --yjit | 1.81 s | 3.09× |

**koruby は CRuby (no-JIT) を上回った。** Hash 実装を proper chained hash に直したのと
ivar の inline cache を入れたのが効き、24 fps 程度 → 33 fps へ到達。

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
