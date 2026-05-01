# astrogre ランタイム

このドキュメントは astrogre が実際にどう正規表現を入力にマッチさせて
いるか — つまり AST node のランタイム意味論と、それを上から包んでいる
ループ層の構造 — を解説する。

中心テーマは 2 つ:

1. **continuation-passing で組まれた小さなノード列** が regex の意味論
   (連結、選択、反復、capture、lookaround) を表現している。
2. その外側を包む **「scanner ノード + body chain」** という形で、入力
   全体を歩くループや、CLI モードに応じた per-match action (count、
   行 print、…) も AST 一級市民として乗っている。

両方とも framework の `body` operand と specialise 機構で AOT bake 時に
1 つの SD 関数に融合される。

## 全体像

```
Ruby ソース ──prism──▶ pm_regular_expression_node_t.unescaped + flags
                       │
                       ▼
                 ┌────────────┐
                 │ regex      │  手書き再帰下降パーサ (parse.c)
                 │ parser     │  /.../ の本体を読み IR を組み立てる
                 └─────┬──────┘
                       │
                       ▼
                 ┌────────────┐
                 │ IR (ire_*) │  union ベースの小さい木:
                 │            │  LIT, CONCAT, ALT, REP, GROUP, CLASS, ...
                 └─────┬──────┘
                       │ lower(..., tail=succ)         right-to-left:
                       ▼                                各 node の
                 ┌────────────┐                         `next` は組み立て済み
                 │ ASTro AST  │  node_re_*              の残り
                 │ (ALLOC_*)  │  node.def から自動生成
                 └─────┬──────┘
                       │  CLI mode に応じて scanner / action chain
                       │  を被せる (case-A factorization)
                       ▼
                 ┌────────────┐
                 │ 実行 AST    │  node_scan_*(... action_chain ...)
                 └─────┬──────┘
                       │
                       ▼ EVAL(c, root)
                  match / no match + captures + count + 出力
```

「lowering」段階で AST は *continuation-passing 形式* の有向チェーンに
なる: 各 match node が `next` operand を持ち、`next` を dispatch する
ことで「パターンの残り」にマッチを試させる。`next` が失敗 (= 0 を返す)
と呼び出し元に戻る — これが、明示的な thread list / stack machine
なしの backtracking 表現になる。

その上に、CLI モードに応じて **scanner ノード** (入力全体を歩く外側
ループ) と **action chain** (マッチごとに何をするかの末尾チェーン) が
被さる。これが後述の「case-A factorization」。

## 動く例 — AST が実際にどう見えるか

`./astrogre --dump '/<pat>/'` でそのパターンを lower した AST が
S 式で出る。代表例:

### 純リテラル search — `/static/`

```
(node_grep_search_memmem
  (node_re_cap_start 0
    (node_re_lit "static" 6
      (node_re_cap_end 0
        (node_re_succ))))
  "static" 6 0)
```

内側から読む: 成功時に `ends[0]` を書いて 1 を返す (`node_re_succ`)、
そこに至るまでに cap_end が `ends[0]` を明示的に書き、リテラルが
`c->str + c->pos` から 6 byte 比較、cap_start が `starts[0]` を書く。
最も外側の `node_grep_search_memmem` がトップレベルの driver — EVAL は
memmem 駆動のループで、候補位置に `c->pos` を設定して各位置でチェーン
を dispatch する。`re_succ` が return 1 を返したら scanner は即停止
(= search 系のセマンティクス)。

### 純リテラル `-c` — `astrogre -c static FILE`

CLI が `-c` モードを判定すると、上記の検索 AST とは別に **case-A
factorization** で組み直された「scanner + action chain」を使う:

```
(node_scan_lit_dual_byte
  (node_action_count
    (node_action_lineskip
      (node_action_continue)))
  "static" 6)
```

scanner は Hyperscan 風 dual-byte filter (先頭 / 末尾バイトを
`vpcmpeqb` して AND、`vptest` で 64-byte chunk 早期 exit) で候補を
見つけ、verify 通ったら body を dispatch。body は:

1. `action_count` が `c->count_result++` して `next` に渡す
2. `action_lineskip` が memchr で次の `\n` を探して `c->pos = nl + 1`
3. `action_continue` が **return 2** を返す (= scanner: 続行)

scanner は return 2 を見たら `c->pos` を読んで次のチャンクから再開する
(後述の return-code protocol)。

### 純リテラル default print — `astrogre static FILE`

scanner は同じ、body chain だけ違う:

```
(node_scan_lit_dual_byte
  (node_action_count
    (node_action_emit_match_line opts=... match_len=6
      (node_action_lineskip
        (node_action_continue))))
  "static" 6)
```

`emit_match_line` が memrchr/memchr で行頭・行末を確定し、`opts` の
bit に応じて filename / 行番号 / `--color` を整形して print する。
`opts` は `ASTROGRE_EMIT_FNAME | _LINENO | _COLOR` の OR。framework
の構造ハッシュは `opts` を operand として見るので、`-n` 付き / 無しは
別 SD として bake される。

### 単一クラス + `+` — `/[a-z]+/`

```
(node_grep_search_range
  (node_re_cap_start 0
    (node_re_rep
      (node_re_class 0 576460743713488896 0 0
        (node_re_rep_cont))
      (node_re_cap_end 0 (node_re_succ))
      1 -1 1))
  97 122 0)
```

16 進っぽい数字は 256-bit クラス bitmap の bm1 フィールド (`'a'`..`'z'`
のビットが立っている)。`node_re_rep` の operand は `body=class`、
`outer_next=cap_end → succ`、`min=1`、`max=-1`、`greedy=1`。Body の
`next` は `node_re_rep_cont` — `c->rep_top` のトップを読んで「もう
1 回繰り返すか抜けるか」を決める singleton sentinel。外側ラッパーが
`node_grep_search_range` なのはクラスが連続範囲 (`'a'`..`'z'`) だから
で、AVX2 で連続範囲スキャンし、ヒットごとに内側チェーンを dispatch する。

### リテラルの alt — `/\b(if|else|for|while|return)\b/`

```
(node_grep_search_byteset
  (node_re_cap_start 0
    (node_re_word_boundary
      (node_re_cap_start 1
        (node_re_alt
          (node_re_lit "if" 2 (... cap_end 1 → wb → cap_end 0 → succ))
          (node_re_alt
            (node_re_lit "else" 4 (...))
            (node_re_alt
              (node_re_lit "for" 3 (...))
              (node_re_alt
                (node_re_lit "while" 5 (...))
                (node_re_lit "return" 6 (...)))))))))
  491629471081 5 0)
```

`491629471081` の最初 8 byte (リトルエンディアン uint64) は
`{i, e, f, w, r, 0, 0, 0}` — alt 各枝の先頭バイト。
`node_grep_search_byteset` は 32 byte chunk で「これらのいずれかの
バイトがあるか」を AVX2 で判定し、候補位置でチェーンを dispatch する。
alt の各枝は末尾の `\b → cap_end → succ` を共有しているので、
lower された木は DAG (末尾 node が複数の親から指される)。

### 後方参照 — `/(\w+)\s+\1/`

```
(node_grep_search_class_scan
  (node_re_cap_start 0
    (node_re_cap_start 1
      (node_re_rep
        (node_re_class <wbm> (node_re_rep_cont))
        (node_re_cap_end 1
          (node_re_rep
            (node_re_class <sbm> (node_re_rep_cont))
            (node_re_backref 1
              (node_re_cap_end 0 (node_re_succ)))
            1 -1 1))
        1 -1 1)))
  <truffle nibble tables>  0)
```

外側ラッパーが `node_grep_search_class_scan` なのは、regex が最初に
マッチさせるのが `\w` (非連続クラス) だから。`node_re_backref 1` は
`c->starts[1]` / `c->ends[1]` を読んで先のキャプチャグループのバイトを
そのままマッチする — 振る舞いが定数 operand ではなくランタイムの
キャプチャ状態に依存する node。

### Anchored — `/\Afoo/`

```
(node_grep_search_memmem
  (node_re_cap_start 0
    (node_re_bos
      (node_re_lit "foo" 3
        (node_re_cap_end 0 (node_re_succ)))))
  "foo" 3 1)
```

外側ラッパー末尾の `1` が `anchored_bos` — search ループは `c->pos == 0`
だけ試して終わる。framework もこの flag を構造ハッシュで見るので、
anchored / unanchored バリアントは別 SD として別個に bake される。

### Lower で残るもの・落ちるもの

Lower された AST は「ランタイムで何をするか」に忠実だが「ユーザが
何を書いたか」には忠実でない:

- 隣接リテラルは coalesce (`/he/` + `/llo/` で 1 個の `node_re_lit "hello"`)
- `/i` リテラルは parse 時に lowercase に pre-fold、マッチャ側は入力を
  on-the-fly で fold (`node_re_lit_ci`)
- `(?:...)` 非キャプチャグループは消える — body が囲い側のチェーンに
  inline される
- アンカー (`\A`、`\b`、`^`、`$` 等) は単一 node になる、zero-width な
  ので `next` をそのまま dispatch するだけ
- クラスは 256-bit bitmap (`uint64_t × 4` をインライン bake)、parser
  レベルの `[a-z]`、`\d`、`\w` 等は全部同じ node kind に潰れる

## Continuation-passing 規約

各 match-node は同じ呼び出し形:

```c
NODE_DEF
node_re_xxx(CTX *c, NODE *n, ..., NODE *next)
{
    if (this node's local check fails) return 0;

    size_t saved_pos = c->pos;
    c->pos += how_much_we_consumed;

    VALUE r = EVAL_ARG(c, next);

    if (!r) c->pos = saved_pos;     /* backtrack */
    return r;
}
```

この形は anchor (`c->pos` を変えない)、capture (slot を save、tail
失敗で restore)、lookaround (成功時も `c->pos` を restore) にも一般化
する。終端は `node_re_succ`、これが `ends[0] = c->pos` を書いて 1 を
返す — マッチがどこで終わったかの記録機構。

チェーンの **始まり** は parser が暗黙に挿入する `cap_start(0, ...)` —
エントリポイントの特別扱い無しでキャプチャグループ 0 が全体マッチ範囲を
記録できるように。

### Return code の意味 (0 / 1 / 2)

scanner と body chain の境界では 3 値の return code が走る:

| 戻り値 | 意味 | 起点 | scanner の動作 |
|---:|---|---|---|
| 0 | body 失敗 (regex chain がここでマッチしなかった) | match-node の途中失敗 | 次の byte / chunk へ |
| 1 | body 成功 + stop | `node_re_succ` | 即 `return 1` |
| 2 | body 成功 + continue | `node_action_continue` | `c->pos` を読んで再開 |

regex chain 内部 (rep / alt / lookaround) は **0 / 1 のみ**で動く。
2 が出るのは action chain (scanner の body 末尾) が `action_continue`
で終わる場合だけ。だから既存の regex 用ノードの内部実装は何も変わらない。

backtracking は body 内部で完結する: rep_cont が body をもう 1 度試し
て失敗 (0) なら次の選択肢に降り、…と既存の CPS で進む。scanner には
何も漏れない。

## 繰り返しの仕組み

繰り返しは continuation-passing 単独では足りない唯一の構造: `a*b` は
body が 0 回以上マッチでき、外側の `b` が入力を奪い合う。小さい
ランタイムスタックを使う:

```c
struct rep_frame {
    NODE *body;           /* 何が繰り返されているか */
    NODE *outer_next;     /* rep の後ろにくるもの */
    int32_t min, max;     /* 残り回数; max == -1 で ∞ */
    uint32_t greedy;
    struct rep_frame *prev;
};
```

`node_re_rep` は新しいフレームを push し、起動時に確保された 1 つ
だけの sentinel — `node_re_rep_cont` — を dispatch する。これが
`c->rep_top` を読んで次の動作を決める。Body の `next` operand は
同じ sentinel に配線されるので、body の各成功 iteration が rep_cont
に戻ってくる。

```c
NODE_DEF
node_re_rep(CTX *c, NODE *n, NODE *body, NODE *outer_next, ...)
{
    struct rep_frame f = { body, outer_next, min, max, greedy, c->rep_top };
    c->rep_top = &f;
    VALUE r = (*c->rep_cont_sentinel->head.dispatcher)(c, c->rep_cont_sentinel);
    c->rep_top = f.prev;
    return r;
}
```

`node_re_rep_cont` は greedy / lazy 契約を実装:

* **Greedy** — まず body をもう 1 回試す (再帰的に body → rep_cont →
  body → ...)、どの深さでも `outer_next` が成功しないなら「min を
  満たしているか? なら outer_next」にこの階層でフォールバック
* **Lazy** — まず `outer_next` を試す (`min == 0` のとき); 失敗したら
  ようやく body をもう 1 回試す

「再帰時に pop する」パターンが大事: `outer_next` を dispatch する
ときに自分のフレームを `c->rep_top` から一時的に外す — そうすると
パターンの残りから引き起こされる**ネストした** rep_cont が正しい
フレームを読める。backtrack 時に restore する。

これで `(a|ab)*c` を `abc` にマッチする例も正しく動く: iteration 1 で
`a` がマッチ、外側の `c` が pos 1 で失敗、コントロールが body の
continuation (rep_cont) を通って戻る。Body の `next` を rep_cont に
配線してあるので、戻り際に**内側の** alt がもう一方の枝 (`ab`) を
試す機会を得る。

## キャプチャ

キャプチャは `c->starts[]`、`c->ends[]`、`c->valid[]` に持つ。2 つの
node:

* `node_re_cap_start(idx, next)` — slot を save し `starts[idx] = c->pos`
  を書き、`next` を dispatch。tail 失敗時に slot を restore
* `node_re_cap_end(idx, next)` — end 側も対称、`valid[idx] = true` を
  set して後方参照 / 出力読み出し側が「slot にデータあり」と分かる
  ように

グループ 0 は parser が AST 全体を包むので、呼び出し側は特別なルートを
通らず `starts[0]` / `ends[0]` で全体マッチ範囲を取れる。

## アンカー

`\A`、`\z`、`\Z`、`^`、`$`、`\b`、`\B` は zero-width: `c->pos` (および
周辺バイト) を見て `next` を dispatch するか 0 を返すかだけ。`\b` /
`\B` は 7-bit ASCII の単語文字述語 (`[A-Za-z0-9_]`) を使う — Ruby の
デフォルトで `/n` / `/u` モードの ASCII 文字での挙動。

## エンコーディング

`c->encoding` は prism のフラグビット (またはリテラル CLI 構文) から
セットされ、regex のエンコーディングモードを反映する:

| flag      | mode      | dot の進み量 | 典型用途 |
|-----------|-----------|--------------|--------|
| `/n`      | ASCII     | 1 byte       | バイナリ入力、性能重視 ASCII |
| `/u`      | UTF-8     | 1 codepoint  | モダン Ruby のデフォルト |
| (無指定)  | UTF-8     | 1 codepoint  | `/u` と同じ |

モードはマッチャの 3 箇所に効く:

### `.` — 4 つの node variant

dot ノードは 4 種類あり、parse 時に選ばれるのでマッチャ自体は
`c->encoding` で分岐しない:

| node                 | マッチするもの                  |
|----------------------|----------------------------------|
| `node_re_dot`        | `\n` 以外の任意の 1 byte         |
| `node_re_dot_m`      | 任意の 1 byte (`/m` フラグ)      |
| `node_re_dot_utf8`   | `\n` 以外の 1 UTF-8 codepoint    |
| `node_re_dot_utf8_m` | 1 UTF-8 codepoint                |

UTF-8 バリアントはリーディングバイトを見て (`0xxxxxxx` → 1 byte、
`110xxxxx` → 2、`1110xxxx` → 3、`11110xxx` → 4) 進み量を決め、
不正なリードバイトは拒否する。

### リテラル — bytes are bytes

パーサが encoding を意識する箇所は 1 つだけ: UTF-8 リーディングバイト
(≥ 0x80) を見たら継続バイト (0x80–0xBF) を同じ `IRE_LIT` トークンに
吸い込む — そうすることで quantifier が codepoint 全体に bind する:

```
/é+/  →  node_re_lit "é" 2 (rep ...)   ; /\xC3 (\xA9+)/ ではなく
```

マッチャはバイトを比較; UTF-8 well-formedness は構築上保たれる。
`/i` は parse 時に lowercase に fold するが ASCII 文字 (`A`-`Z`) のみ
— `/É/i` は今 `é` にマッチしない (本格的な Unicode case fold は今後
の課題)。

### 文字クラス — ASCII のみ

クラスは 256-bit bitmap (`uint64_t × 4` インライン bake) で、生バイト
で動く。ASCII 範囲 (`[a-z]`、`[0-9]`、`\d`、`\w`、`\s`) は完全に動作。
`[...]` 内の非 ASCII *文字* は 1 つの bitmap エントリで表現できない
(`[ä]` の U+00E4 はバイト列 `0xC3 0xA4` をマッチする必要があり、
バイト `0xE4` だけではない); 今は parser が wrong な byte-wise bitmap
を作る、これは `todo.md` で文書化している。

### エンコーディング × SIMD prefilter

prefilter ノードはバイトレベルで動くので UTF-8 とちゃんと合成する:

- **memchr / memmem / byteset / range** — どれも入力をバイトとして
  スキャン。ASCII パターンならどのエンコーディングモードでも完全に
  正しい。最初のバイトが UTF-8 リーディングバイトのパターン (例:
  `/é+/` は `0xC3` で始まる) では prefilter が `0xC3` の候補位置を
  スキャンし、body chain が codepoint 全体を verify する
- **class_scan (Truffle)** — 同じ。256-bit bitmap はバイトごとに
  build されるので、ASCII クラスでは真のクラスメンバーシップ判定

`/i` は今 prefilter ladder 全体を無効化する (cheap な ASCII fold
ケースの twin-memchr すら parser がまだ作っていない)。

### 未対応

- `\p{...}` / `\P{...}` Unicode property classes
- `/i` の Unicode case folding
- `[...]` 内のマルチバイト文字
- `\X` extended grapheme cluster
- EUC-JP (`/e`) / Windows-31J (`/s`) — 需要次第

## AST に折り畳んだループ — scanner と action chain

正規表現マッチを「**入力全体にわたって何度も適用する**」という外側
ループは 2 段ある:

1. **start-position scan** — match を試す位置 `c->pos` を 0、1、2、…
   と進める内側ループ
2. **per-match dispatch** — match が見つかるたびに「次に何をするか」
   を決める外側ループ (count、行 print、最初のマッチで止まる、…)

両方とも AST ノードに上げてある。framework の `body` operand と
specialise 機構が両方を 1 つの SD に inline-fuse できるからこそ、
indirect call ゼロの 1 関数として bake される。

### Layer 1: start-position scan を AST に — `node_grep_search`

For-each-start-position ループそれ自体が AST node。EVAL がループ、
`body` operand が regex AST、`anchored_bos` operand は `\A` 始まり
パターン用に「1 位置だけ」に short-circuit する。

```c
NODE_DEF
node_grep_search(CTX *c, NODE *n, NODE *body, uint32_t anchored_bos)
{
    size_t start = c->pos;
    size_t start_max = anchored_bos ? (start == 0 ? 1 : 0) : c->str_len + 1;
    for (size_t s = start; s < start_max; s++) {
        c->pos = s;
        for (int i = 0; i < ASTROGRE_MAX_GROUPS; i++) c->valid[i] = false;
        c->rep_top = NULL;
        if (EVAL_ARG(c, body)) return 1;
    }
    return 0;
}
```

ループを node.def に置くのが鍵。Specialiser が `body` に再帰し
`body_dispatcher` を直接関数ポインタとして inline すると、gcc が
ループと regex chain を 1 つの SD 関数に融合する — indirect call も
DISPATCH chain もなく、capture 状態リセットは `vmovdqu` 1 つに集約。

prefilter 付き variant (`node_grep_search_memmem` / `_memchr` /
`_byteset` / `_range` / `_class_scan`) も同じ形で、`body` に対する
扱いは同一; 違うのは「ループの中身が候補位置を見つける方法」だけ
(memmem / memchr / AVX2 SIMD)。

### Layer 2: per-line ループを AST に — `node_scan_lit_dual_byte` + action chain

CLI モード (`-c`、default print、…) は「マッチが見つかったら何をして、
どこから次を探すか」を決める。これも AST に上げる。

scanner ノードは layer 1 と同じ「`body` operand 経由の dispatch」形だが、
contract が違う:

```c
NODE_DEF
node_scan_lit_dual_byte(CTX *c, NODE *n, NODE *body,
                        const char *needle, uint32_t needle_len)
{
    /* ... 64-byte stride dual-byte filter ... */
    while (chunk) {
        if (testz(filter_mask)) { p += 64; continue; }   /* hot path */
        for each candidate q:
            if (verify_middle(q)) {
                c->pos = q - c->str;
                VALUE r = EVAL_ARG(c, body);
                if (r == 1) return 1;          /* search-style stop */
                if (r == 2) p = c->str + c->pos;  /* action chain advanced pos */
                /* r == 0: regex chain failed at q → walk next bit */
            }
    }
}
```

`body` には CLI モード別の **action chain** が入る:

| CLI mode | body chain |
|---|---|
| `-c LIT` | `count → lineskip → continue` |
| default `LIT FILE` | `count → emit_match_line(opts) → lineskip → continue` |
| 通常 search | `cap_start(0) → cap_end(0) → re_succ` (`re_succ` が return 1) |

action ノードはどれも小さく、`next` continuation を持つ。例:

```c
NODE_DEF
node_action_count(CTX *c, NODE *n, NODE *next)
{
    c->count_result++;
    return EVAL_ARG(c, next);
}

NODE_DEF
node_action_lineskip(CTX *c, NODE *n, NODE *next)
{
    const uint8_t *nl = memchr(c->str + c->pos, '\n', c->str_len - c->pos);
    c->pos = nl ? (size_t)(nl - c->str) + 1 : c->str_len + 1;
    return EVAL_ARG(c, next);
}

NODE_DEF
node_action_continue(CTX *c, NODE *n)
{
    return 2;
}
```

`emit_match_line(next, match_len, opts)` は memrchr/memchr で行範囲を
確定して `c->fname` / `c->lineno` / `c->out` を使って整形 print する。
`opts` (`ASTROGRE_EMIT_FNAME | _LINENO | _COLOR`) は baked immediate なので、
`-n` 付き / 無し / `-H` 付きが別 SD として bake される。

### Backtracking との関係

backtracking は body chain の **内部** で完結する。`/static_(\d+)/` の
default print なら body は

```
cap_start(0) → re_lit("static_") → cap_start(1) →
    rep([0-9]+) → cap_end(1) → cap_end(0) →
    count → emit_match_line(opts) → lineskip → continue
```

で、rep が `[0-9]+` を greedy に消費して後ろの `cap_end(1)` 等が成功
すれば action chain に進む。途中失敗 (例: rep が 0 回しか取れず
`cap_end` も失敗) なら body は 0 を返し、scanner は次の候補位置に
進む。action chain は match が成立して**初めて**動く。

scanner からは「body が 0 を返したか、1 を返したか、2 を返したか」しか
見えない。rep_top の管理も rep_cont sentinel もすべて regex chain 側
の責務。

### case-A factorization の利点

scanner と action を分けたことで:

- **scanner と action が直交**。新しい scanner (例: Hyperscan Teddy
  multi-pattern) を足したら全 mode が自動で恩恵を受ける。新しい
  action (例: `-Z` NUL 区切り print) を足したら全 scanner で使える
- **既存 regex AST と新 action AST が同じ chain protocol で繋がる** —
  途中で regex で verify し、その先で action 列を走らせる、という素直な
  合成が書ける
- **bake は何も特別扱いしない** — body chain 全体が SD に inline され
  scanner との境界は消える。CLI mode 別の SD が hash 経由で自動分離

`/static/` の default print は 66 ms → 28 ms (2.4×、118 MB warm)、
ripgrep 34 ms を抜く水準まで来た。`-c` も 23 ms で perf 同等を維持。
詳しいベンチは [`perf.md`](./perf.md)、設計判断のメモは
[`done.md`](./done.md#scanner--action-chain-で-c-と-default-print-を統一) 参照。

## メモリ

* AST node は framework の `node_allocate` (calloc) で heap allocate。
  パターンの寿命の間生き、`astrogre_pattern_free` で解放
* 中間 IR (`ire_*`) は lower 直後に解放 — AST だけが残る
* CTX と rep frame は stack 確保なので hot path での malloc なし
* 1 パターンに対して **複数の root** を持てる (regex search 用、
  count_lines 用、print_lines 用)。CLI モードに応じて lazy build され、
  framework の hash が共通サブツリーを SD レベルで dedup する

## スレッドモデル

無し。CTX は呼び出しごと、rep_cont sentinel はグローバル共有だが、
mutable な状態 (rep stack、captures、count_result、lineno、…) は呼び
出し側 CTX に置く。2 スレッドが別 CTX で AST を共有しつつ並行マッチ可能。

## バックエンド抽象

grep CLI (main.c) は `backend.h` だけと話す。プラグインされている
バックエンドは 2 つ:

* `backend_astrogre.c` — in-house エンジン (この一連の解説の対象)
* `backend_onigmo.c`   — Onigmo (`onig_new` + `onig_search` + region
                          オブジェクト)、`WITH_ONIGMO=1` で build

ops 表は `compile / search / search_from / free / aot_compile /
has_fast_scan`。`-F` (固定文字列) は両者とも compile 呼び出しで実装 —
自前エンジンは `astrogre_parse_fixed` 経由、Onigmo はメタ文字を
escape してから `onig_new` に渡す。パターンオブジェクトはどちらも
opaque、CLI 側は中身を見ない。

`-c` / default print の case-A 経路は astrogre backend 限定で
`astrogre_backend_pattern_get` 経由で `astrogre_pattern *` を取り出して
`astrogre_pattern_count_lines` / `astrogre_pattern_print_lines` を呼ぶ。
Onigmo backend ではそのまま `search_from` per-line ループに落ちる。

## ASTro の specialization が効くところ・効かないところ

このサンプルを書いていて分かったベンチ駆動メモ。

### 1 dispatch あたりの仕事が大きい時に bake は効く

dispatch の中身が意味のあるサイズ — メソッド呼出、型チェック、
フレーム push — のとき、bake の役割 (indirect call の削除 + 子
operand の定数畳み込み) が wall time の有意な部分を占める:

- koruby `fib`: interp → AOT で 3.6×
- 自前の `literal-tail` microbench (16 KiB single buffer、繰り返し
  search): 22.75 s → 3.15 s, **7.22×**。fused SD には indirect call が
  1 つも残らない

### 1 dispatch が trivial だと bake 単体は効きにくい

grep CLI では話が逆になる。各位置の中身は「1〜2 個の compare」であって、
メソッド呼出ではない。bake が消す dispatch chain (1 位置あたり 3〜4 個
の indirect call、全部同じ hot BTB target、~1 ns 程度) は元から安い。
さらに literal-prefix prefilter ノードが verify chain を候補位置でしか
動かさないので、bake が削減できる総 dispatch 量は µs オーダー。

つまり grep のような、1 位置の中身が trivial で全位置を歩く形の
ワークロードでは **bake 単独では伸びしろが小さい**。伸びしろは
algorithmic optimization の側にある。

### 正解パターン: アルゴリズムをノードに包む

このサンプルが明示している設計上の教訓。ASTro の specialiser は
algorithmic optimization を*発明*できない; 与えてくれるのは
「無料の合成機構」 — 一度 optimization を node として表現すれば、
framework が hash し、code-store で共有し、`body` operand を inline する。

> *algorithmic optimization を見つけたら、ノードに包む。parser /
> CLI に適切な前提条件下で emit させる。bake が残りをやってくれる。*

これに従って 5 つの prefilter ノードと 2 つの「ループそのものを node 化」
が landing 済み:

| node                            | アルゴリズム                                | parser / CLI trigger                   |
|---------------------------------|---------------------------------------------|----------------------------------------|
| `node_grep_search_memmem`       | glibc memmem (two-way string match)         | ≥ 4-byte literal prefix                |
| `node_grep_search_memchr`       | glibc memchr (AVX2 PCMPEQB)                 | ≥ 1-byte literal prefix                |
| `node_grep_search_byteset`      | N × `vpcmpeqb` + OR (≤ 8 bytes)             | 小さい first-byte set (alt of literals) |
| `node_grep_search_range`        | `vpsubusb / vpminub / vpcmpeqb`             | 単一連続範囲の first class             |
| `node_grep_search_class_scan`   | Hyperscan-style Truffle (PSHUFB ×2 + AND)   | 任意 256-bit first class               |
| `node_grep_search`              | for-each-start-position ループ              | 全パターン (上記 prefilter なしのとき) |
| `node_scan_lit_dual_byte`       | Hyperscan 風 dual-byte filter (64-byte stride) | `-c` / default print の pure-literal |

設計上のポイントは **prefilter / scanner と bake が合成する** こと。
`node_grep_search_memmem(/static/)` の SD は memmem 呼び出し AND
`"static"` を候補位置で verify する inline 化されたチェーンの**両方**
を含む — 両方が同じ SD 内、両方が gcc の optimiser から見える、両方が
dlsym で到達可能。各 prefilter / scanner を足すのに bake / hash /
code-store 機構を変える必要は無かった — framework が自然と正しく
振舞った。

case-A factorization (`node_scan_lit_dual_byte` + action chain) も
同じ形で乗ったのが分かりやすい例: scanner が verify 済みの位置で
action chain を dispatch、bake は両方を inline、CLI mode 別の SD は
hash で勝手に dedup される。

ladder を拡張する候補 node (まだ手付かず、同じ形):

| node                          | アルゴリズム               | parser / CLI trigger                              |
|-------------------------------|---------------------------|---------------------------------------------------|
| `node_grep_search_teddy`      | multi-pattern AVX2 scan   | パターンの任意位置に ≥ 1 個の固定 literal あり    |
| `node_action_emit_match_span` | only-matching 出力         | `-o`                                              |
| `node_action_emit_filename`   | filename print + early exit | `-l`                                              |
| `node_scan_lit_dual_byte_ci`  | case-fold dual-byte filter | `-c` / default print の `-i` 付き                 |

### 要約

- algorithmic optimization は新しい node 種類として住まわせる。bake は
  そのあと AST の他の部分と無料で合成する
- bake の貢献が綺麗に出るのは、内側の per-iter の仕事が trivial でない
  とき
- 「ASTro が速い」には両方が要る: per-node のアルゴリズム工夫
  (memchr、PSHUFB、dual-byte filter, …) AND specialization (アルゴリズム
  外殻が内部で wrap する regex verify と合成できるように)

## ドライバ: 上に乗っている grep

`main.c` が grep フロントエンド。それ自体は regex の仕事を一切
しない; 全部バックエンド ops と AST API を経由する。要点:

* **ファイル / stdin の per-line 経路** — `getline`、`backend->search`
  を 1 行ずつ。パターンコンパイルは 1 パターンに 1 度、入力全体で再利用
* **whole-file mmap 経路** (`process_buffer`) — regular file かつパターン
  に SIMD/libc prefilter があるとき自動発火。ファイルを一度 mmap、
  `backend->search_from` を whole buffer に対し回し、各マッチの含まれる
  行を memrchr/memchr で identify。`backend.h` の `has_fast_scan` op で
  gate される。`-v` invert モードはこの path をスキップ
* **Pure-literal `-c` 経路** (`process_buffer_pure_literal`、`-c` 専用) —
  pattern が pure literal で `-c` モードのとき、`astrogre_pattern_count_lines`
  経由で `node_scan_lit_dual_byte + count → lineskip → continue` に
  落とす。エンジン entry を 1 度だけ叩いて全マッチを scan
* **Pure-literal default print 経路** (同じ場所) — pattern が pure
  literal で default print モード (no `-c`、no `-l`/`-L`、no `-o`、no
  `-i`、no `-v`) のとき、`astrogre_pattern_print_lines` 経由で
  `node_scan_lit_dual_byte + count → emit_match_line → lineskip → continue`
  に落とす。CLI フラグ (`-n` / `-H` / `--color`) は `emit_opts` bitfield
  に packed されて framework の hash で別 SD として bake される
* **`--color` / `-o`** (旧経路): `backend->search_from` をループで動かして
  1 行内の全マッチを enumerate。zero-width マッチはカーソルを 1 byte
  進めて空回りを避ける。case-A action node 化は `todo.md` 参照
* **`-w` (whole-word)**: regex レベルで `\b...\b` で包む (`-F` のときは
  literal を先に escape)
* **`-r`**: `opendir` + 再帰下降; デフォルトでドットファイルをスキップ
* **`--via-prism`**: 各 `-e PATTERN` を、その引数を Ruby ソースとして
  prism で parse して見つかる最初の `/.../` の本体で置き換える
* **`--verbose`**: フェーズ別の wall-clock を stderr に出す。`INIT` /
  `pattern compile` / `mmap` / `scan` / `munmap` / `at exit` の各時刻が
  見えるので、何が支配的かを strace 無しで判断できる
