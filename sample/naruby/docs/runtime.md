# naruby ランタイム構造

言語仕様は [spec.md](spec.md)、実装済み機能は [done.md](done.md)、
未対応は [todo.md](todo.md) を参照。

naruby は ASTro フレームワーク (`../../lib/astrogen.rb` + `../../runtime/`)
の上に乗った最小サブセット Ruby インタプリタ。**Plain / AOT /
Profile-Guided / JIT** の 4 モードを 1 バイナリで切り替えるのが特徴。

## 1. パイプライン全景

```
   foo.na.rb
       │
       ▼
   Prism (libprism.so)
       │   pm_node_t* (CRuby と同じ Ruby AST)
       ▼
   transduce  (naruby_parse.c)
       │   PM_* → ALLOC_node_*  (未対応 PM_* は "unsupported" で exit)
       │   ローカル変数を slot 番号化、関数名を C string に確保
       ▼
   NODE * (heap, ASTroGen 形式)
       │
       ▼
   OPTIMIZE(ast)               ← node.c:88
       │   if (-i)            : 何もしない
       │   if (-j) jit submit : astro_jit_submit_query (UDS で L1 へ)
       │   else              : astro_cs_load(ast, NULL) で
       │                        code_store/all.so から SD_<hash> を dlsym
       ▼
   EVAL(c, ast)                ← node.h:64 (inline)
       │   (*ast->head.dispatcher)(c, ast)
       ▼
   再帰 dispatch (node_seq → node_call_<N>/node_pg_call_<N>/... → ...)
       │
       ▼
   VALUE (= int64_t)           ← printf("Result: %ld\n", ...)
       │
       ▼   (デフォルト or `-c` 時)
   build_code_store(ast)        ← main.c
       │   astro_cs_compile(ast, NULL)        ── code_store/c/SD_<hash>.c
       │   astro_cs_compile(各 code_repo body) ── 同上
       │   astro_cs_build(NULL)               ── make -C code_store all.so
       │   astro_cs_reload()                  ── dlopen 切り替え
```

## 2. NODE の構造

すべてのノードは `struct Node` で、`head` と `u`(union) からなる。

```c
struct Node {
    struct NodeHead head;
    union {
        struct node_num_struct       node_num;       // { int32_t num; }
        struct node_lget_struct      node_lget;      // { uint32_t index; }
        struct node_lset_struct      node_lset;      // { uint32_t index; NODE *rhs; }
        struct node_seq_struct       node_seq;       // { NODE *head; NODE *tail; }
        ...
    } u;
};
```

`union u` は `node.def` の `NODE_DEF` 宣言から ASTroGen が生成
(`node_head.h`)。各ノードのオペランド型がそのままフィールドになる。

`NodeHead` (node.h):

```c
struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;      // hash_value 有効か
        bool has_hash_opt;        // hash_opt 有効か (HOPT cache フラグ、PGSD で使用)
        bool is_specialized;      // SD_<hash> に置換済み
        bool is_specializing;     // SPECIALIZE 中 (再帰検出用)
        bool is_dumping;          // DUMP 中 (再帰検出用)
        bool no_inline;           // SPECIALIZE 出力で head.dispatcher を直読みする
    } flags;

    const struct NodeKind *kind;  // ノード種別のメタ (kind table は ASTroGen 生成)
    struct Node *parent;          // (オプション機能、ASTRO_NODEHEAD_PARENT)

    node_hash_t hash_value;       // 構造ハッシュ (HORG)
    node_hash_t hash_opt;         // プロファイル含むハッシュ (HOPT、PGSD chain 用)

    const char *dispatcher_name;  // "DISPATCH_node_xxx" / "SD_<hex>" / "PGSD_<hex>"
    node_dispatcher_func_t dispatcher; // VALUE (*)(CTX *c, NODE *n)

    enum jit_status { ... } jit_status;  // JIT 状態 (Querying / Compiling / ...)
    unsigned int dispatch_cnt;
    int line;                     // 元 .rb の行 (hopt_index の (Horg, file, line) → Hopt 引きに使用)
};
```

ハッシュは `node.def` 上のオペランドを再帰的に折り畳んで作る (ASTroGen
が `node_hash.c` を生成)。子 NODE は `hash_node()` を再帰呼び出し。
組み込み関数ポインタは `hash_builtin_func()` (naruby 独自、`bf->name` と
`bf->func_name` を結合) で扱う。

## 3. CTX とフレームポインタ

```c
typedef struct CTX_struct {
    VALUE *env;                          // 大域 VALUE スタック (malloc 確保)
    unsigned int func_set_cnt;
    struct function_entry *func_set;     // 関数テーブル (max 2000)
    state_serial_t serial;               // 関数定義版番。callcache 無効化に使う
} CTX;
```

ローカル変数フレームポインタ `fp` は **dispatcher の第 3 引数として
レジスタ渡し**しており、CTX には持たない。castro パターン。

```c
typedef RESULT (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *fp);
```

各 NODE_DEF は `(CTX *c, NODE *n, VALUE *fp, ...)` で受け取る。

- `node_lget(idx)` / `node_lset(idx, rhs)` は `fp[idx]` を直接読み書き
  (`c->fp` のメモリ往復ゼロ)。
- `node_scope(envsize, body)` は `EVAL(c, body, fp + envsize)` で
  body を呼び、本体は新しい fp を見る。
- `node_call*` は `fp + arg_index` を callee に渡す (caller が
  予め `lset` で並べた arg slots がそのまま callee の局所スロット
  になる)。

`function_entry` (`name → body / params_cnt / locals_cnt`) は線形リスト。
名前検索は線形探索で、`callcache` が hit すればそれを使う。

## 3.1 RESULT 型と非局所脱出

`return` を setjmp なしで実装するため、dispatcher の戻り値は **2 ワード
の RESULT envelope** (= `{ VALUE value; unsigned int state; }`)。
`{rax, rdx}` 2 レジスタで戻るので関数呼び出しの ABI コストはほぼ変わらない。

```c
#define RESULT_NORMAL 0u
#define RESULT_RETURN 1u

typedef struct { VALUE value; unsigned int state; } RESULT;

#define RESULT_OK(v)        ((RESULT){(v), RESULT_NORMAL})
#define RESULT_RETURN_(v)   ((RESULT){(v), RESULT_RETURN})

#define UNWRAP(r) ({ RESULT _r = (r); \
    if (UNLIKELY(_r.state != RESULT_NORMAL)) return _r; \
    _r.value; })
```

各 NODE_DEF は子 EVAL の戻り RESULT を `UNWRAP` で受ける。state が
NORMAL なら value を取り出して続行、そうでなければそのまま return
で上に投げ返す。`return` の意味論は:

```
node_return         → emit  RESULT { value, RETURN }
  ↓ propagate up via UNWRAP
node_seq / node_if / node_while → return as-is
  ↓
call_body (node_call) /
  node_call_<N>      /
  node_call2         /
  node_pg_call_<N>   /
  node_call_static       (function boundary)
  → if state == RETURN, set state = NORMAL (caught)
  → return RESULT { value, NORMAL } to caller of the function
```

最適化:

- インライン化された SD の連鎖では state が compile-time 0 で
  伝わるので、`if (UNLIKELY(state))` チェックは gcc が DCE する。
  非インライン境界 (関数呼び出し再帰、cross-SD call) でだけ
  状態チェックの分岐が残る — branch predictor がほぼ無料で扱う。
- naruby は今のところ RETURN 以外の状態 (BREAK / CONTINUE / GOTO)
  を使っていないので、関数境界の catch は単純な
  `if (state == RETURN) state = NORMAL` で済む。

## 4. EVAL_ARG とディスパッチャ

ASTroGen が生成する `node_eval.c` の頭:

```c
#define EVAL_ARG(c, n) (EVAL_ARG_CHECK(n), (*n##_dispatcher)(c, n))
```

`node_seq.head` を eval する場合、parser は予め `head_dispatcher` という
ローカル変数 (= `head->head.dispatcher`) を取って、`(*head_dispatcher)(c,
head)` を呼ぶ。これが「子ディスパッチャの load を 1 つ早く済ませる」
ための specialize-friendly な書き方。

specialize された SD_<hash> 関数では、子 dispatcher は **コンパイル時定数**
として埋め込まれる (`SD_<child_hash>` のシンボルを直接書く)。これにより:

- インタプリタ経路: indirect call (関数ポインタ越し)
- specialize 経路: direct call (シンボル決定済)

になる。これが ASTro の論文での評価対象。

## 5. 4 つの実行モード

### 5.1 Plain (`-i`)

最も単純。`OPTIMIZE` は何もせず、`head.dispatcher` は ASTroGen が出した
`DISPATCH_node_xxx` のまま。すべての子呼び出しは indirect call。

### 5.2 AOT (デフォルト or `-c`)

```
1st run:  parse → build_code_store → OPTIMIZE / cs_load → EVAL
                    ↓                       ↓
                 code_store/c/SD_<hash>.c   dlsym("SD_<hash>")
                 make -C code_store         head.dispatcher を SD_ に書き換え
                 dlopen → all.so            EVAL は specialized code で走る

2nd run:  parse → OPTIMIZE / cs_load → EVAL
            (build_code_store は dedup でほぼ no-op、`-b` で完全 skip)
```

**bake は EVAL より前に行う** (`main.c`)。これは「1st run でも SD を
使う」設計選択 — もし build_code_store を EVAL の後ろに置くと、cold
start では interp が走り終わってから bake が始まり、その run では SD が
全く使われない。これでは `aot-1st` セルが「interp 時間 + bake 時間」を
測ることになり、本来意図する「compile + SD-aware run」を測れない。
順序を入れ替えることで、cold start でも `OPTIMIZE` / `cs_load` が新しく
焼かれた `code_store/all.so` を見るので、EVAL は specialize された
dispatcher で走る。

`-c` (compile-only) は EVAL をスキップ。`-b` (bench mode) は
build_code_store をスキップ (load はする)。ベンチで AOT load 後の
純粋実行時間を測るときに使う。

### 5.3 Profile-Guided (`-p`)

parser が arity-N specialized なノードを吐く: argc ≤ 3 では
`node_pg_call_<N>`、argc > 3 では fallback として `node_call2`。
`node_call2` / `node_pg_call_<N>` はどちらも `callcache` に加えて
`sp_body` (specialized body) フィールドを持ち、本体 NODE への直接
リンクを保持する。AOT モード (= `node_call_<N>`) よりも 1 段ポインタを
少なくできるのが狙い (AOT の `cc->body` 経由は 2 段、PG の `sp_body`
経由は 1 段で、SD ベイク時には direct call まで畳み込まれる)。

#### node_call_<N> vs node_pg_call_<N> (AOT vs PG の比較公平化)

ASTro の AOT / PG ベンチ比較で「inlining の効果」を切り出すため、
非 PG モード (plain / AOT) でも argc ≤ 3 では arity-N specialized な
**`node_call_<N>` を使う** (parser の `alloc_call_specialized` 経由)。

両ノードファミリは shape を揃えてある:

| | node_call_<N> (plain/AOT) | node_pg_call_<N> (PG) |
|---|---|---|
| arg operands | explicit (`a0, a1, ...`) | explicit (`a0, a1, ...`) |
| 引数 eval | inline (`v0 = EVAL_ARG(a0)` etc) | inline (同上) |
| callee frame | `VALUE F[locals_cnt]` (fresh VLA) | 同上 |
| 引数の代入先 | `F[i] = vi` | 同上 |
| 本体 dispatch | **`EVAL(c, cc->body, F)` indirect** | **`(*sp_body_dispatcher)(c, sp_body, F)` direct** |

唯一の違いが「`cc->body` indirect vs `sp_body` baked-direct」で、
これがまさに PG モードが LTO で稼ぎたい "inlining 効果" の分。
ベンチの `n/aot-c` と `n/pg-c` を直接比較すれば、(i) arity-N
specialize / fresh frame を共通化した上で、(ii) baked-direct call
+ LTO 貫通 inline がどれだけ効くかを切り出して見られる。

argc > 3 や `-s` (static_lang) は従来の `node_call` / `node_call2` /
`node_call_static` 経路に落ちる (parser の generic path)。

#### sp_body の link タイミング

`naruby_parse.c` の `callsite_resolve` (PARSE 末尾で呼ばれる) が、
`def f` 完了時点で `f` への pending callsite を全部巡回し、
`node_call2.sp_body` を本体 NODE に書き戻す。これで前方参照・自己再帰
どちらも parse 終了時には sp_body が解決済みになる。

```ruby
def fib(n)
  if n < 2
    1
  else
    fib(n - 2) + fib(n - 1)   # 自己再帰: parse 中は sp_body=placeholder
  end                          # callsite_resolve が def 終了後に patch
end

fib(40)                        # 順方向: parse 時点で sp_body=fib_body
```

#### sp_body と HASH の関係

「parse 時に link」を実現するためには、sp_body の patch がノードの
HASH を変えてはいけない。それと同時に、再帰関数の sp_body=自己 が
HASH 計算でサイクルを起こしてもいけない。両方を解決するため、
**`sp_body` は HASH の計算式から完全に除外**してある (`naruby_gen.rb`
の `hash_call(val, kind:)` で `'0'` を返す)。

ASTro の Merkle 原則は「同一構造 ≡ 同一ハッシュ」だが、特定パラメータを
ハッシュに含めない (= 0 を返す) のはフレームワーク内の合法な操作。
今回 sp_body を除外する代わりに、call site の SD は body 側のシンボル
名を `hash_node(sp_body)` から直接合成している (= sp_body の identity
は SD body 内に直接埋め込まれる) ので、結果として「sp_body の差は
SD body の識別に効く / call site のキャッシュキーには効かない」という
住み分けになる。

> ASTro 原則上の落とし穴: call site の specialize 中に **body NODE の
> head を書き換えない**こと。body は別エントリとしてベイクされる側で、
> その中で head.dispatcher_name を設定する。call site の build_specializer
> が `sp_body->head.dispatcher_name = ...` を触ると、ベイク順依存で
> 変なことが起きる。naruby_gen.rb は body の hash から直接シンボル名を
> 計算するだけで body には触らない。

結果として:

- `HASH(node_call2)` は `name`, `params_cnt`, `arg_index` だけから
  決まる安定値 (callcache や sp_body は無視)。
- `HASH(fib_body)` は fib の中身ノードの HASH を再帰的にたたんだ値。
  fib 内の node_call2 の hash は sp_body に依らないので、自己参照
  は HASH に登場せず無限再帰しない。

#### SD は direct call をベイク

`naruby_gen.rb` の `build_specializer` で sp_body の emission を
上書きし、SD は **`SD_<HASH(sp_body)>` を C 上の定数として埋め込む**。
ベイクされた all.so 内では `-Wl,-Bsymbolic` で intra-.so の参照が
直接アドレス解決され、`addr32 call <SD_addr>` の direct call まで
落ちる。`sp_body->head.dispatcher` のロードは出ない。

#### 2 段ガード (cc 鮮度 + sp_body 一致)

baked-direct call (`SD_<HASH(parse_time_sp_body)>`) は **コンパイル時
定数** なので、実際の呼び先 body と parse 時の speculation が一致して
いるときだけ正しい。fast path はこれを 2 つの条件で守る:

1. **`c->serial == cc->serial`** — cc が古くないか (= def による
   global serial bump 以降に refresh 済か)
2. **`cc->body == sp_body`** — cc が指す現行 body が、SD にベイク
   された speculation 対象と一致しているか

両方真なら baked-direct call を採用。どちらかが偽なら slowpath に
落ちる。slowpath は cc を必要に応じ refresh し、`cc->body` を経由した
**indirect** dispatch で正しさを確保する (sp_body は parse 時値の
ままで触らない — そうしないと SD の baked 定数と整合しなくなる)。

```c
SD_<call2_hash>(c, n, fp) {
    cc = &n->u.node_call2.cc;            // @ref で inline 構造体
    if (LIKELY(c->serial == cc->serial && cc->body == sp_body)) {
        return SD_<HASH(sp_body)>(c, sp_body, fp + arg_index);  // direct
    }
    return node_pg_call_slowpath(c, n, fp, name, ..., cc);
}
```

slowpath:

```c
RESULT node_pg_call_slowpath(c, n, fp, name, ..., cc) {
    if (c->serial != cc->serial) {
        fe = sp_find_func_entry(c, name);
        cc->serial = c->serial;
        cc->body = fe->body;            // sp_body は触らない
    }
    return EVAL(c, cc->body, fp + arg_index);  // indirect dispatch
}
```

##### 2 段にした理由

「serial 1 つだけ」では足りないケース: parse 時の speculation
(`callsite_resolve` が `code_repo` の "最後の def" を sp_body に書き戻す)
と、実行時の **最初の cc 充填** で得られる body が一致しない場合がある。
代表例:

```ruby
def f(x) = x + 1     # body_v1
p(f(10))             # ← (A) 1 回目 call
def f(x) = x * 2     # body_v2
p(f(10))             # ← (B) 2 回目 call
```

- parse 末: `code_repo[f] = body_v2` (last wins) → 両 call site の
  `sp_body = body_v2`。SD は `SD_<HASH(body_v2)>` をベイク。
- 実行: `def v1` で `fe->body = body_v1`、serial bump。
- (A): cc miss → slowpath が `cc->body = body_v1` で refresh。
  serial 1 つチェックの旧設計だと、SD が次の同 call site 呼び出しで
  `SD_<HASH(body_v2)>(c, sp_body=body_v2, fp)` を baked-direct で呼ぶ。
  ところが `cc->body = body_v1` (current) → 実際の呼び先は body_v2 に
  なって意味的に wrong (もっとも (A) は再呼び出しがないので顕在化せず)。
- (A) を loop で複数回呼ぶ亜種では、2 周目以降に baked-direct が起動
  → body_v1 を呼ぶべきところで body_v2 の specialized code が走る。

2 段ガードでは (A) の 2 周目で `cc->body (=body_v1) != sp_body (=body_v2)`
で fast path が抜けて slowpath → `cc->body = body_v1` で正しく dispatch。

##### 再定義との整合

`def f` を 2 度目以降に走らせると `c->serial++`。次の call で:
- `c->serial != cc->serial` → fast path 抜け → slowpath。
- slowpath は cc を refresh (cc->body = 新 body)、`cc->body` 経由
  indirect で実行。
- 以降の同 call site 呼び出しは「serial match だが cc->body != sp_body」
  状態で連続 slowpath。ベンチでは再定義は起きないので fast path が
  常に通る。

旧設計 (1 段 + slowpath demote) と違い、**dispatcher の降格は不要**。
2 段チェック自体が baked-direct の正しさを動的に守るので、SD は
そのまま残してよい。

##### 定常時の dispatch コスト (benchmarks)

定常時 (再定義なし、parse 時 speculation が一致):

- load 1: `cc->serial` (cc は `@ref` で NODE inline、1 ロード)
- compare 1: `c->serial`
- load 2: `cc->body`
- compare 2: `sp_body` (operand 値、フレーム経由で利用可能)
- direct call → ベイクされた `SD_<HASH(body)>`
- 子の dispatcher load は **不要** (定数ベイク済)

旧 1 段ガードに比べ **+1 load + 1 cmp + 1 jcc** 増えるが、
recursive bench で実測 +20% 程度の影響。chain bench はほぼノイズ内。
正しさを担保するために避けられないコスト。

実測 (fib(40), [perf.md](perf.md) より): n/plain 4.34s → n/aot-c 0.73s
→ n/pg-c 0.71s → n/lto-c 0.71s。再帰系は HOPT cycle break で
PGSD ≒ SD なので LTO 効果なし、AOT vs PG の差も 2 段ガード分で縮む。

#### 5.3.1 HOPT / PGSD chain (Profile-Guided Specialization)

ASTro framework の HOPT/PGSD 機構を利用した投機的チェイン inline。
非再帰 call chain (`f → g → h → ...`) を LTO で 1 関数に折り畳む
ことを目的とする。実測 chain 系で **PG → PG+LTO で 36-65% 高速化**
(詳細 [perf.md](perf.md))。

##### 二段ハッシュ: HORG / HOPT

- **HORG** = 既存の `HASH(n)` (= `hash_node(n)`)。pg_call 系の
  `sp_body` は除外 (`naruby_gen.rb` の `hash_call(val, kind: :horg)`)。
  call site の identity = stable な構造ハッシュ。
- **HOPT** = `node.c` の `HOPT(n)` / `hash_node_opt(n)`。
  `naruby_gen.rb` の `hash_call(val, kind: :hopt)` で sp_body も
  含めて再帰計算。`is_hashing` フラグで cycle 検出時に 0 を返す
  (再帰関数では sp_body 部分が潰れる → HOPT ≈ HORG)。

各 NODE は `head.hash_value` (HORG cache) と `head.hash_opt`
(HOPT cache) の 2 つを持つ。`has_hash_value` / `has_hash_opt`
フラグで lazy compute。`node_allocate` で `memset` 必須
(`has_hash_opt` が malloc ガベージで true になると HOPT が
偽キャッシュ 0 を返す事故防止)。

##### SD と PGSD の二系統

同じ body NODE が `SD_<HORG>` と `PGSD_<HOPT>` の 2 つのシンボルで
all.so にエクスポートされる:

- **SD_<HORG>**: AOT モード (`astro_cs_compile(body, NULL)`)。
  HORG が同じなら同じ SD。sp_body 違いに鈍感。
- **PGSD_<HOPT>**: PGC モード (`astro_cs_compile(body, file)`)。
  sp_body の構造を反映 → 投機チェインごとに固有 SD。

PGSD chain は parent SD が `PGSD_<HOPT(child)>` を baked-direct で呼ぶ
構造になり、LTO で連鎖貫通 inline ができる。SD chain は
`SD_<HORG(child)>` で同じだが HORG 共有で同 SD に潰れるため inline
の余地が小さい。

##### `hopt_index.txt`

framework が `(Horg, file, line) → Hopt` の対応を `code_store/hopt_index.txt`
に追記永続化。`astro_cs_load(n, file)` は `file != NULL` のとき
hopt_index を引いて PGSD_<Hopt> を dlsym する。見つからなければ
SD_<Horg> へ fall-back。

##### naruby 側の wire-up

```c
/* node_def 実行時 (node.def): body の dispatcher を PGSD に */
NODE_DEF @noinline node_def(...) {
    fe->body = OPTIMIZE(body);
    (void)astro_cs_load(fe->body, name);   // file = function name
    ...
}

/* main.c: top-level も PGC bake + load */
extern const char *naruby_current_source_file;  /* parser 経由 */
OPTIMIZE(ast);
(void)astro_cs_load(ast, naruby_current_source_file);
EVAL(c, ast, c->env);
...
build_code_store(ast):
    astro_cs_compile(ast, NULL);                      // SD_<HORG(top)>
    astro_cs_compile(ast, naruby_current_source_file); // PGSD_<HOPT(top)>
    for body in code_repo:
        astro_cs_compile(body, NULL);                  // SD_<HORG(body)>
        astro_cs_compile(body, body_name);             // PGSD_<HOPT(body)>
```

両形 bake する理由: top SD (AOT) は `SD_<HORG(body)>` を baked-direct
で参照。SD form がないと dlopen の symbol resolution で fail する
(unresolved symbol in .data)。AOT 版を残すことで AOT chain が成立、
PGC 版を追加することで PGSD chain も成立。

##### 効果が出る条件

PGSD+LTO で大きく速くなるのは:
- **非再帰 call chain** (`f0 → f1 → ... → fN`)。各レベルの HOPT が
  下流の構造を符号化 → ユニークな PGSD → LTO inline 可能。
- **sp_body が parser-stable** (= def の前方リンク確定)。再帰
  function は cycle 0 で潰れる → HOPT ≈ HORG → SD と差なし。

実測 (詳細は perf.md):

| bench | n/pg-c | n/lto-c | 改善 |
|-------|------:|------:|-----:|
| call (10 段)         | 1.19 | 0.58 | **-51%** |
| chain20 (20 段)      | 1.27 | 0.71 | **-44%** |
| chain40 (40 段)      | 3.01 | 0.92 | **-69%** |
| chain_add (10 段 +1) | 0.11 | 0.07 | **-36%** |
| compose (3 段)       | 0.12 | 0.09 | **-25%** |
| deep_const (10 段 0-arg) | 1.26 | 0.63 | **-50%** |
| branch_dom           | 0.14 | 0.14 | ±0 (1 段なので chain 無効) |
| fib (再帰)           | 0.57 | 0.56 | ±0 (HOPT cycle break) |
| ackermann (再帰)     | 0.74 | 0.71 | ±0 (同上) |

### 5.4 JIT (`-j`)

`astro_jit.c` (740 行) が独立した worker (L1 サーバ) と Unix Domain
Socket で通信。

```
naruby (L0)               L1 (別プロセス)
   │                          │
   │ submit_query(NODE *)    │
   ├─────────────────────────►│   PRELOAD: 既に baked か？
   │ ◄─────────────────────── │   yes → so_path 返却 (L0 が dlopen)
   │                          │
   │ submit_compile(NODE *)  │   no  → SPECIALIZED_SRC を送信
   ├─────────────────────────►│         L1 が gcc で .so を作る
   │ ◄─────────────────────── │         compiled の通知 → L0 が dlopen
```

L0 側の dispatch counter (`head.dispatch_cnt`) が閾値を超えると
`OPTIMIZE` (= `astro_jit_submit_query`) を 1 回呼ぶ。L1 が baked .so
を返したら `head.dispatcher` を差し替える。astro_code_store と JIT は
独立した dlopen ハンドル (`astrojit/l1_so_store/all.so` vs
`code_store/all.so`) を持つ。

## 6. ASTroGen 連携 (`naruby_gen.rb`)

`node.def` を読んで C コードを 8 ファイル生成する。naruby では
`NaRubyNodeDef < ASTroGen::NodeDef` で次のオペランド型を上書き:

| 型 | hash_call | dumper | specializer |
|---|---|---|---|
| `struct builtin_func *` | `hash_builtin_func(...)` | `bf:NAME` | 識別子直書き |
| `builtin_func_ptr` | 0 | `<ptr>` | `(builtin_func_ptr)func_name` (have_src 時) |
| `state_serial_t` | 0 | `<serial>` | フィールドそのまま |
| `struct callcache *` | 0 | `<cc>` | フィールドそのまま |

ハッシュ計算で `state_serial_t` / `callcache *` を 0 で吸収するのが要点。
これにより「同じ構造の関数呼び出し」は同じ hash になり、SD_ がキャッシュ
ヒットする。

## 7. ファイル一覧

```
sample/naruby/
├── main.c                   # main, code_repo, define_builtin_functions, build_code_store
├── naruby_parse.c           # Prism AST → naruby NODE (transduce)
├── naruby_gen.rb            # ASTroGen の naruby カスタム
├── node.def                 # ノード定義 (naruby 言語仕様の中核)
├── node.h                   # NodeHead / EVAL マクロ / public API
├── node.c                   # node_allocate, OPTIMIZE, INIT + 共有 runtime/ の include 集約
├── context.h                # CTX, OPTION, VALUE, builtin_func, callcache
├── bf.h                     # 組み込み関数 (narb_p / narb_zero / narb_add)
├── astro_jit.h              # JIT API (start/stop/submit_query/submit_compile)
├── astro_jit.c              # JIT 実装 (UDS + dlopen)
├── Makefile                 # ビルド & ベンチ
├── prism/                   # libprism のサブモジュール
├── bench/
│   ├── bench.rb             # ベンチランナー
│   ├── *.na.rb              # ベンチ題材
│   └── *.c                  # 同等 C コード (gcc 比較用)
├── docs/                    # このディレクトリ
│   ├── spec.md              # 言語仕様
│   ├── done.md              # 実装済み機能
│   ├── todo.md              # 未対応 / 既知の制限
│   ├── runtime.md           # このファイル
│   └── perf.md              # ベンチ結果
├── README.md                # English overview
├── test.na.rb               # 既定の test 入力
└── (生成物)
    ├── node_eval.c          # ASTroGen 出力 (eval ロジック)
    ├── node_dispatch.c      # ASTroGen 出力 (DISPATCH_*)
    ├── node_dump.c          # ASTroGen 出力 (DUMP_*)
    ├── node_hash.c          # ASTroGen 出力 (HASH_*)
    ├── node_specialize.c    # ASTroGen 出力 (SPECIALIZE_*)
    ├── node_replace.c       # ASTroGen 出力
    ├── node_alloc.c         # ASTroGen 出力 (ALLOC_*)
    ├── node_head.h          # ASTroGen 出力 (kind table, struct Node)
    └── code_store/          # 実行時生成
        ├── Makefile
        ├── c/SD_<hash>.c
        ├── o/SD_<hash>.o
        └── all.so
```

`node_exec.c` と `naruby_code.c` は現在使われていない (詳細は [todo.md](todo.md))。

## 8. runtime/ との接続

`node.c` の中での #include 順は意味がある:

```c
#include "node.h"
#include "context.h"
#include "astro_jit.h"

// User-provided
extern size_t node_cnt;
static NODE *node_allocate(size_t size) { ... }   // ASTroGen が呼ぶ
static void dispatch_info(...) { ... }             // 旧式 trace (no-op)

// 共有インフラ
#include "astro_node.c"        // hash_*, HASH, DUMP, alloc_dispatcher_name
                               // → static、node_allocate を見つけられるよう先に定義

// naruby 固有
static node_hash_t hash_builtin_func(...) { ... }  // astro_node の hash_cstr/merge を利用

#include "astro_code_store.c"  // SPECIALIZE, astro_cs_*
                               // → astro_node.c の HORG/HOPT/hash_node を利用

// User-provided (続き)
void clear_hash(NODE *) { ... }
NODE *OPTIMIZE(NODE *) { ... }                    // ここで astro_cs_load を呼ぶ
char *SPECIALIZED_SRC(NODE *) { ... }              // JIT 用 (astro_jit.c から呼ばれる)

// ASTroGen 生成
#include "node_eval.c"
... etc

void INIT(void) { astro_cs_init("code_store", ".", 0); }
```

`astro_node.c` は `hash_uint64` / `hash_double` を `__attribute__((unused))`
で囲ってあるので、naruby が使っていない型のハッシュ関数があっても警告は
出ない。

`astro_code_store.c` の関数のうち naruby が使うのは:

| 関数 | 用途 |
|---|---|
| `astro_cs_init` | 起動時に `code_store/all.so` を dlopen |
| `astro_cs_load(n, NULL)` | OPTIMIZE で SD_<hash> に差し替え |
| `astro_cs_compile(n, NULL)` | 終了時に SD_<hash>.c を書き出す |
| `astro_cs_build(NULL)` | `make -C code_store all.so` |
| `astro_cs_reload` | dlopen を新しい all.so に切り替え |
| `astro_cs_disasm(n)` | (任意) objdump でディスアセンブル |

naruby は `astro_cs_load(n, file)` の 2-arg 形 (file != NULL) を使用し、
`code_store/hopt_index.txt` の `(HORG, file, line) → HOPT` 索引から
PGSD_<HOPT> を dlsym する。`HOPT(n)` は `node.c` で本体実装され、
`naruby_gen.rb` の `:hopt` gen_task が `node_hopt.c` を生成する。

呼び出し箇所:

| 場所 | 呼び出し | 用途 |
|---|---|---|
| `node.def` の `node_def` | `astro_cs_load(fe->body, name)` | 各関数 body を PGSD として load (file = function 名) |
| `main.c` 起動時 | `astro_cs_load(ast, naruby_current_source_file)` | top-level AST を PGSD として load (file = .na.rb のパス) |
| `main.c` の `build_code_store` | `astro_cs_compile(body, name)` × 全 code_repo | bake で AOT(SD)/PGC(PGSD) 両形を生成 |
