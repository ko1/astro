# runtime.md — aforth のランタイム解説

aforth が **どこで何をやっているか** を、ホットパス (word call /
データスタック / 制御フロー) を中心に説明する。詳細実装は `node.def`,
`main.c` 参照。

## 1. VALUE 表現

```c
typedef int64_t VALUE;
```

- 単純な signed 64-bit 整数。Forth-true は `-1` (全 1 ビット)、false は `0`。
- ヒープオブジェクトはなし。`VARIABLE` / `CREATE` で確保するセルは
  `c->vars[]` という 1M-cell の VALUE 配列に静的にレイアウトされる。
- アドレス値 (`&c->vars[i]`) はそのまま VALUE に詰まれて
  data stack に乗る (Forth-traditional な byte-address モデル)。

## 2. 実行コンテキスト (CTX)

```c
typedef struct {
    VALUE *dsp;   VALUE *dstack_base;   VALUE *dstack_end;
    VALUE *rsp;   VALUE *rstack_base;   VALUE *rstack_end;
    struct aforth_do_frame *dop;
                  struct aforth_do_frame *dostack_base;
                  struct aforth_do_frame *dostack_end;
    VALUE *vars;  uint32_t vars_used;
    int    leave_flag;
} CTX;
```

| 用途 | フィールド | 意味 |
|------|-----|-----|
| データスタック | `dstack_base` … `dstack_end` (64K cell) | `dsp` は **次の slot** を指す (top は `dsp[-1]`) |
| リターンスタック | `rstack_base` … `rstack_end` (16K cell) | Forth `>R / R> / R@` 用 |
| DO/LOOP フレーム | `dostack_base` … `dostack_end` (4K frame) | `(index, limit)` ペアの並列スタック。`I` / `J` がここを読む。リターンスタックと **混ざらない** ので `>R / R> / R@` がループ内でも自由に使える |
| `vars[]` (1M cell) | VARIABLE / CONSTANT / CREATE+ALLOT の領域 | `var_id` でインデックス |
| `leave_flag` | LEAVE が立てるフラグ | LOOP / AGAIN が観測して脱出 |

`dsp` は **GC scan の高水位** ではなく単なるスタックポインタ。
GC 自体ない (heap オブジェクトを持たない言語)。

## 3. word の実行

aforth は **AST 解釈** で動く Forth。各 word は AST 上の NODE で、
data stack へ作用する eval を `node.def` に書いている。

```c
NODE_DEF
node_dup(CTX *c, NODE *n)
{
    VALUE v = c->dsp[-1];
    c->dsp[0] = v;
    c->dsp++;
    return 0;
}
```

- 引数を受け取らない word は ASTroGen が `EVAL_node_dup(c, n)` を生成。
- `EVAL_ARG(c, child)` は子ノードの dispatcher を **直接呼ぶ** (静的に
  受け取った `node_dispatcher_func_t`)。SD 化されている子なら、その
  inline 版が SD 内で展開される。

### 3.1 `node_seq` — 連結

```c
NODE_DEF @always_inline
node_seq(CTX *c, NODE *n, NODE *first, NODE *rest)
{
    EVAL_ARG(c, first);
    return EVAL_ARG(c, rest);
}
```

`a b c d` という Forth source は parser が
`seq(a, seq(b, seq(c, d)))` に右畳みする。`@always_inline` 指定で SD
内では 1 直線の basic block として展開される。

### 3.2 IF / ELSE / THEN

`IF` は **stack top を pop** してその値で分岐する。`THEN` は構文上の
終端マークなので AST には残らない (`node_if(then, else)` の有無で
表現)。

```c
NODE_DEF @always_inline
node_if(CTX *c, NODE *n, NODE *then_branch, NODE *else_branch)
{
    VALUE cond = *--c->dsp;
    if (cond != 0) return EVAL_ARG(c, then_branch);
    if (else_branch) return EVAL_ARG(c, else_branch);
    return 0;
}
```

`else` が無い `IF body THEN` は `node_if_only(then)` という別ノードに
落ちる (NULL 比較を 1 つ削るだけのマイクロ最適化、効果は微妙だが
inline 後の C コードが綺麗)。

### 3.3 BEGIN / WHILE / UNTIL / AGAIN

```forth
BEGIN body UNTIL          \ until top != 0
BEGIN body AGAIN          \ infinite (LEAVE で抜ける)
BEGIN cond WHILE body REPEAT
```

3 種それぞれ別ノード:
- `node_begin_until(body)`
- `node_begin_again(body)` — `c->leave_flag` を見て break
- `node_begin_while(cond, body)`

### 3.4 DO / LOOP / +LOOP / I / J / LEAVE

`limit start DO body LOOP` は 2 値を pop して do-loop frame に push。
ループ index は `dop[-1].index`、外側は `dop[-2].index` (`J` が読む)。

```c
NODE_DEF @always_inline
node_do_loop(CTX *c, NODE *n, NODE *body)
{
    VALUE start = *--c->dsp;
    VALUE limit = *--c->dsp;
    c->dop->index = start;
    c->dop->limit = limit;
    c->dop++;
    while (c->dop[-1].index < c->dop[-1].limit && !c->leave_flag) {
        EVAL_ARG(c, body);
        c->dop[-1].index++;
    }
    if (c->leave_flag) c->leave_flag = 0;
    c->dop--;
    return 0;
}
```

`+LOOP` は body 末尾で push された step を pop して `index += step`。
正負ステップで終了条件が反転する標準 Forth 仕様。

### 3.5 word call

```c
NODE_DEF @noinline
node_call(CTX *c, NODE *n, const char *name, uint32_t word_id)
{
    NODE *body = aforth_word_table[word_id];
    return EVAL(c, body);
}
```

ポイント:
- **全 word call が runtime 間接 dispatch**。caller の SD には
  `EVAL_node_call(c, n)` の固定 stub だけ。
- `aforth_word_table[]` は parse 時に id ごと埋まる。RECURSE は自分
  自身の id を埋め込むだけで OK (table 経由なので body NODE * を前方
  参照しなくていい)。
- `@noinline` で caller の SD に展開されないため、相互再帰のサイクル
  検出が **不要**。
- 代償: 非再帰の simple word でも 1 回の load + indirect call が必ず
  入る。perf.md で議論しているとおり、ここは ASTroGen 拡張の余地。

## 4. AOT / 起動シーケンス

```
[1st run, code_store/all.so なし]
  INIT() → astro_cs_init("code_store", ".", 0)   nothing to load
  parse → ALLOC nodes → OPTIMIZE → astro_cs_load all miss
  EVAL                                             interp 経路

[--aot-compile 付き]
  parse 完了後:
    astro_cs_compile(toplevel, NULL)
    for each word_id w:
        astro_cs_compile(aforth_word_table[w], NULL)
    astro_cs_build(NULL)        # cc → all.so
    astro_cs_reload()           # dlopen
    astro_cs_load(toplevel) + 各 word に対して再 resolve
  EVAL                          SD 経由

[2nd run, all.so あり]
  INIT() → astro_cs_init dlopen all.so
  parse → ALLOC → OPTIMIZE → astro_cs_load   hit → dispatcher patched
  EVAL                                       SD 経由
```

`astro_cs_load` が NODE の `head.dispatcher` を SD のシンボルアドレス
に書き換えるので、`EVAL(c, n)` の最初の 1 dispatch から SD に飛ぶ。
最初の `OPTIMIZE` 呼び出しは parser から ALLOC 直後に走る。

## 5. parser の概略

`main.c` で:

1. `tokenize(buf)` — 空白区切りの Forth トークン化、`\` / `( ... )`
   のコメント除去、`."  ... "` を一塊のトークンに。
2. `parse_program()` — top-level ループ。
   - `:` ⇒ word_id を確保、symbol 登録、body を `parse_seq(END_SEMI)` で取り、
     `aforth_word_table[id] = body`。`code_repo_add` で AOT walk 用に
     登録。
   - `VARIABLE name` / `CREATE name` ⇒ vars 領域に slot 確保。
   - `<int|const> CONSTANT name` / `<int|const> ALLOT` は 2/3-token
     先読みで parser 内で解決。
   - 残りは `parse_one()` で leaf word / 制御構造を再帰下降。
3. `parse_seq(enders, *which)` — 制御構造の終端トークンに当たるまで
   `parse_one()` を呼んで、`fold_seq()` で右畳み。

`fold_seq` は flat な NODE 配列を `node_seq(first, rest)` の右畳みで
ツリー化する。1 要素なら包まずそのまま返す (= 構造ハッシュが安定)。

## 6. ASTro hooks

| 関数 | 役割 |
|------|------|
| `INIT()` | `astro_cs_init` 呼び出し。store dir = `code_store`、src dir = `.`、version = 0 |
| `EVAL(c, n)` | 単に `n->head.dispatcher(c, n)`。SD があれば SD、無ければ generated `DISPATCH_xxx` |
| `OPTIMIZE(n)` | ALLOC のたびに呼ばれる。`astro_cs_load(n, NULL)` で SD 解決を試みる |
| `code_repo_add(name, body, force)` | dedup 込みで `(name, body)` を append |
| `code_repo_find(h)` | hash で body を引く (現状 AOT walk 専用) |

`record_all` オプションは未配線 (常時 false 扱い)。
