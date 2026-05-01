# astrogre ランタイム

このドキュメントは astrogre が実際にどう regex を入力にマッチさせて
いるか — つまり AST node のランタイム意味論 — を解説する。特に繰り返し
と continuation flow の配線に重点を置く。

## 全体像

```
Ruby ソース ──prism──▶ pm_regular_expression_node_t.unescaped + flags
                       │
                       ▼
                 ┌────────────┐
                 │ regex      │  手書き再帰下降パーサ
                 │ parser     │  (parse.c) — /.../ の本体を読み IR を組み立て
                 └─────┬──────┘
                       │
                       ▼
                 ┌────────────┐
                 │ IR (ire_*) │  union ベースの小さい木: LIT, CONCAT, ALT,
                 │            │  REP, GROUP, CLASS, ...
                 └─────┬──────┘
                       │  lower(..., tail=succ)        right-to-left:
                       ▼                                各 node の
                 ┌────────────┐                         `next` は組み立て済みの
                 │ ASTro AST  │  node_re_*              残り
                 │ (ALLOC_*)  │  node.def から自動生成
                 └─────┬──────┘
                       │
                       ▼ EVAL(c, root)
                  match / no match + captures
```

「lowering」段階が AST を *continuation-passing 形式* の有向チェーン
にしている: 各 match node が `next` operand を持ち、`next` を dispatch
することで「パターンの残り」にマッチを試させる。`next` が失敗 (= 0
を返す) と呼び出し元に戻る — これが、明示的な thread list / stack
machine なしの backtracking 表現になる。

## 動く例 — AST が実際にどう見えるか

`./astrogre --dump '/<pat>/'` でそのパターンを lower した AST が
S 式で出る。代表例:

### 純リテラル — `/static/`

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
を dispatch する。

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
で、その範囲に対して AVX2 スキャンし、ヒットごとに内側チェーンを
dispatch する。

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
`{i, e, f, w, r, 0, 0, 0}` — alt 各枝の先頭 byte。
`node_grep_search_byteset` は 32 byte chunk で「これらのいずれかの
byte があるか」を AVX2 で判定し、候補位置でチェーンを dispatch。
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
そのままマッチする — つまり振る舞いが定数 operand ではなくランタイムの
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
だけ試して終わる。フレームワークもこの flag を構造ハッシュで見るので、
anchored / unanchored バリアントは別 SD として別個に bake される。

### Lower で残るもの・落ちるもの

Lower された AST は「ランタイムで何をするか」に忠実だが「ユーザが
何を書いたか」には忠実でない:

- 隣接リテラルは coalesce (`/he/` + `/llo/` で 1 個の `node_re_lit "hello"`)。
- `/i` リテラルは parse 時に lowercase に pre-fold、マッチャ側は入力を
  on-the-fly で fold (`node_re_lit_ci`)。
- `(?:...)` 非キャプチャグループは消える — body が囲い側のチェーンに
  inline される。
- アンカー (`\A`、`\b`、`^`、`$` 等) は単一 node になる、zero-width な
  ので `next` をそのまま dispatch するだけ。
- クラスは 256-bit bitmap (`uint64_t × 4` をインライン bake)、parser
  レベルの `[a-z]`、`\d`、`\w` 等は全部同じ node kind に潰れる。

## Continuation-passing 規約

各 match-node は同じ呼び出し形:

```c
NODE_DEF
node_re_xxx(CTX *c, NODE *n, ..., NODE *next)
{
    /* 自分の局所チェック */
    if (this node's local check fails) return 0;

    /* undo 用に状態を保存 */
    size_t saved_pos = c->pos;
    c->pos += how_much_we_consumed;

    /* パターンの残りに試させる */
    VALUE r = EVAL_ARG(c, next);

    if (!r) c->pos = saved_pos;     /* backtrack */
    return r;
}
```

この形は anchor (`c->pos` を変えない)、capture (slot を save、tail
失敗で restore)、lookaround (成功時も `c->pos` を restore) にも一般化
する。終端は `node_re_succ`、これが `ends[0] = c->pos` を書いて 1 を
返す — マッチがどこで終わったかの記録機構。

チェーンは `node_re_succ` で終端。チェーンの **始まり** は parser が
暗黙に挿入する `cap_start(0, ...)` — エントリポイントの特別扱い無しで
キャプチャグループ 0 が全体マッチ範囲を記録できるように。

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
  満たしているか? なら outer_next」にこの階層でフォールバック。
* **Lazy** — まず `outer_next` を試す (`min == 0` のとき); 失敗したら
  ようやく body をもう 1 回試す。

「再帰時に pop する」パターンが大事: `outer_next` を dispatch する
ときに自分のフレームを `c->rep_top` から一時的に外す — そうすると
パターンの残りから引き起こされる**ネストした** rep_cont が正しい
フレームを読める。backtrack 時に restore する。

これで `(a|ab)*c` を `abc` にマッチする例も正しく動く: iteration 1 で
`a` がマッチ、外側の `c` が pos 1 で失敗、コントロールが body の
continuation (rep_cont) を通って戻る。Body の `next` を rep_cont に
配線してあるので、戻り際に**内側の** alt がもう一方の枝 (`ab`) を
試す機会を得る — これが合成全体として `ab` then `c` をマッチさせる。
「body は呼び出しごとに 1 回成功を返す」short-cut だとこれを取り逃
がす。

## キャプチャ

キャプチャは `c->starts[]`、`c->ends[]`、`c->valid[]` に持つ。2 つの
node:

* `node_re_cap_start(idx, next)` — slot を save し `starts[idx] = c->pos`
  を書き、`next` を dispatch。tail 失敗時に slot を restore。
* `node_re_cap_end(idx, next)` — end 側も対称、`valid[idx] = true` を
  set して後方参照 / 出力読み出し側が「slot にデータあり」と分かる
  ように。

グループ 0 は parser が AST 全体を包むので、呼び出し側は特別なルートを
通らず `starts[0]` / `ends[0]` で全体マッチ範囲を取れる。`node_re_succ`
は `ends[0]` も書く (group 0 がラップされてるので冗長だが安価で、将来
「ラップしない」最適化が入っても破綻しない保険)。

## アンカー

`\A`、`\z`、`\Z`、`^`、`$`、`\b`、`\B` は zero-width: `c->pos` (および
周辺 byte) を見て `next` を dispatch するか 0 を返すかだけ。`\b` /
`\B` は 7-bit ASCII の単語文字述語 (`[A-Za-z0-9_]`) を使う — これが
Ruby のデフォルトで `/n` / `/u` モードの ASCII 文字での挙動。

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

マッチャは byte を比較; UTF-8 well-formedness は構築上保たれる。
`/i` は parse 時に lowercase に fold するが ASCII 文字 (`A`-`Z`) のみ
— `/É/i` は今 `é` にマッチしない (本格的な Unicode case fold は今後
の課題)。

### 文字クラス — ASCII のみ
クラスは 256-bit bitmap (`uint64_t × 4` インライン bake) で、生バイト
で動く。ASCII 範囲 (`[a-z]`、`[0-9]`、`\d`、`\w`、`\s`) は完全に動作。
`[...]` 内の非 ASCII *文字* は 1 つの bitmap エントリで表現できない
(`[ä]` の U+00E4 はバイト列 `0xC3 0xA4` をマッチする必要があり、
バイト `0xE4` だけではない); 今は parser が wrong な byte-wise bitmap
を作る、これは [`todo.md`](./todo.md) で文書化している。マルチバイト
クラスをサポートするには hybrid 表現 (ASCII bitmap + ソート済み
codepoint 範囲リスト) が必要。

### アンカーと `\b`
`\b` / `\B` は 7-bit ASCII 単語文字述語 (`[A-Za-z0-9_]`) を使う。
これが Ruby のデフォルト (`/n` / `/u` の ASCII 文字でのふるまい);
Unicode 単語境界 (`\p{L}`、`\p{N}` 由来) は未対応。他のアンカー
(`\A`、`\z`、`\Z`、`^`、`$`) はエンコーディング非依存 — byte 位置と
`\n` byte (0x0A) を見るだけで、これは全対応エンコーディングで同じ。

### エンコーディング × SIMD prefilter
prefilter ノードは byte レベルで動くので UTF-8 とちゃんと合成する:

- **memchr / memmem / byteset / range** — どれも入力を byte として
  スキャン。ASCII パターンならどのエンコーディングモードでも完全に
  正しい。最初のバイトが UTF-8 リーディングバイトのパターン (例:
  `/é+/` は `0xC3` で始まる) では prefilter が `0xC3` の候補位置を
  スキャンし、body chain が codepoint 全体を verify する — ランダムな
  `0xC3` バイトでの false positive は期待通り body で弾かれる。
- **class_scan (Truffle)** — 同じ。256-bit bitmap は byte ごとに
  build されるので、ASCII クラスでは真のクラスメンバーシップ判定、
  仮想的な UTF-8 クラスでは「許される codepoint の最初の byte」を
  prefilter にして body 側に再 verify を任せる形になる。

`/i` は今 prefilter ladder 全体を無効化する (cheap な ASCII fold ケース
の twin-memchr すら parser がまだ作っていない)。`/i` リテラル用の
twin-memchr が最も小さい修正。

### 未対応
- `\p{...}` / `\P{...}` Unicode property classes
- `/i` の Unicode case folding
- `[...]` 内のマルチバイト文字
- `\X` extended grapheme cluster
- EUC-JP (`/e`) / Windows-31J (`/s`) — 需要次第

## トップレベル search

For-each-start-position ループそれ自体が AST node: `node_grep_search`。
EVAL がループ、`body` operand が regex AST、`anchored_bos` operand は
`\A` 始まりパターン用に「1 位置だけ」に short-circuit する。
`astrogre_search` (match.c) は CTX をセットアップして `EVAL(c, root)`
を 1 度呼ぶだけ。

```c
NODE_DEF
node_grep_search(CTX *c, NODE *n, NODE *body, uint32_t anchored_bos)
{
    size_t start = c->pos;                  /* 呼び出し側がセット */
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
逆アセと 7.22× の `literal-tail` microbench 数値は
[`perf.md`](./perf.md) を参照。

## `-c` モードの AST 書き換え

`node_grep_search` がスタート位置探索ループを AST に取り込んだのと
同じ idiom を、CLI レイヤーでもう一段適用する。`-c PURE_LITERAL` の
per-line カウントループ (旧 `process_buffer_pure_literal` 内の
`while (memmem...) { count++; memchr(\n); ...}`) もまるごと AST
ノードに昇格させる。

通常パースだとリテラル `/static/` は

```
(node_grep_search_memmem
  (node_re_cap_start 0
    (node_re_lit "static" 6
      (node_re_cap_end 0
        (node_re_succ))))
  "static" 6 0)
```

を生成する。CLI 側は `-c` モードかつ pure-literal 形と判定したら
パターンの root を別ノードに**書き換える**:

```
(node_grep_count_lines_lit "static" 6)
```

body チェーン (cap_start/lit/cap_end/succ) は捨てる — `-c` モードの
契約が「verify 不要」を保証している。

新ノードの中身は scan + verify + line-skip + count++ を 1 関数に
集約: AVX2 dual-byte filter (先頭バイトと末尾バイトをそれぞれ
broadcast 即値として `vpcmpeqb`、AND マスクを `vptest` で全 0 判定
→ 99.5%+ のチャンクは hot path で `p += 64`)、候補位置で中間バイト
を `memcmp` verify (needle 長は AOT bake で即値、`cmpl + cmpb`
チェーンに展開)、マッチごとに `memchr` で次の `\n` まで飛んで
count++、最後に `c->count_result` に書く。

framework が needle を SD ソースに `static const char NEEDLE[] = ...`
として焼き込むので、`_mm256_set1_epi8(needle[0])` も `cmpl
$0x69746174` (= "tati") も即値で、PCRE2-JIT 風の SIMD-fused-verify
が AOT bake だけで自然と出てくる (PGO 不要)。

この書き換えのおかげで `-c /static/` は 64 ms → 23 ms (118 MB warm)。
内訳は scan 8 ms + mmap/munmap 13 ms + 起動その他 2 ms。`--verbose`
で各フェーズが見える ([`done.md`](./done.md#-c-pure_literal-を-ast-ノードに折り込み--node_grep_count_lines_lit) 参照)。

教訓: 「アルゴリズムをノードに包む」だけでなく **「実行モード由来の
不変式 (= verify が要らない / 行ごとにスキップしていい)」を AST 書き
換えで AST に伝える** と、framework が verify-less 特化版を bake
できる。CLI 周りの最適化を AST 上の操作に変換できると、後の AOT
パイプラインに自然に乗る。

## メモリ

* AST node はフレームワークの `node_allocate` (calloc) で heap allocate。
  パターンの寿命の間生き、`astrogre_pattern_free` で解放。
* 中間 IR (`ire_*`) は lower 直後に解放 — AST だけが残る。
* CTX と rep frame は stack 確保なので hot path での malloc なし。

## スレッドモデル

無し。CTX は呼び出しごと、rep_cont sentinel はグローバル共有だが、
mutable な状態 (rep stack、captures) は呼び出し側 CTX に置く。2 スレッ
ドが別 CTX で AST を共有しつつ並行マッチ可能。

## バックエンド抽象

grep CLI (main.c) は `backend.h` だけと話す。プラグインされている
バックエンドは 2 つ:

* `backend_astrogre.c` — in-house エンジン (この一連の解説の対象)。
* `backend_onigmo.c`   — Onigmo (`onig_new` + `onig_search` + region
                          オブジェクト)、`WITH_ONIGMO=1` で build。

ops 表は `compile / search / search_from / free`。`-F` (固定文字列)
は両者とも compile 呼び出しで実装 — 自前エンジンは
`astrogre_parse_fixed` 経由、Onigmo はメタ文字を escape してから
`onig_new` に渡す (Onigmo 自体には固定文字列モードがない)。パターン
オブジェクトはどちらも opaque、CLI 側は中身を見ない。

これは plumbing で最適化ではないが、`bench/grep_bench.sh` の比較
ハーネスを安く書けるようにしているのはこの抽象。

## ASTro の specialization が効くところ・効かないところ

このサンプルを書いていて分かったことのベンチ駆動メモ。「AOT bake は
何にでも効く」と思っていると裏切られる種類の答えなのでここに残す。

### 1 回の dispatch あたりの仕事が大きい時に bake は効く
1 dispatch の中身が意味のあるサイズ — メソッド呼出、型チェック、
フレーム push — のとき、bake の役割 (indirect call の削除 + 子
operand の定数畳み込み) が wall time の有意な部分を占める:

- koruby `fib`: interp → AOT で 3.6×。
- pascalast の典型的ベンチ: 表全体で 2–25×。
- 自前の `literal-tail` microbench (16 KiB single buffer、繰り返し
  search): **22.75 s → 3.15 s, 7.22×**。fused SD には indirect call が
  1 つも残らない。

### algorithmic optimization が dispatch を食ったら bake は効かなくなる
grep CLI では話が逆になる。各位置の中身は「1 〜 2 個の compare」で
あって、メソッド呼出ではない。bake が消す dispatch chain (1 位置
あたり 3〜4 個の indirect call、全部同じ hot BTB target、~1 ns 程度)
は元から安い。さらに literal-prefix prefilter ノード
(`node_grep_search_memchr` / `_memmem`) が landing したことで、
verify chain は候補位置でしか動かない (1 KB あたり数回)、bake が
削減できる総 dispatch 量は µs オーダー:

```
bench: 118 MB corpus, post-prefilter
                                interp     aot-cached
literal /static/                0.285 s    0.287 s    (essentially noise)
```

ugrep は同じ search を 2 ms で終える (mmap + memchr-spans-whole-file)。
astrogre が ugrep に対して残している 100× 差は specialization では
**埋まらない** — process startup + per-line getline + CTX init が
支配的で、行イテレーションを AST 側にも畳み込めば縮められる。

### 正解パターン: アルゴリズムをノードに包む
これがこのサンプルが明示している設計上の教訓。ASTro の specialiser は
algorithmic optimization を*発明*できない; 与えてくれるのは「無料の
合成機構」 — 一度 optimization を node として表現すれば、フレームワー
クが hash し、code-store で共有し、`body` operand を inline する。
エンジニアリングの形は:

> *algorithmic optimization を見つけたら、ノードに包む。parser に
> 適切な前提条件下で emit させる。bake が残りをやってくれる。*

5 つの prefilter ノードがランディング済み、全て同じ形に従い、AVX2
/ glibc-SIMD を使う:

| node                            | アルゴリズム                                | parser trigger                         |
|---------------------------------|---------------------------------------------|----------------------------------------|
| `node_grep_search_memmem`       | glibc memmem (two-way string match)         | ≥ 4-byte literal prefix                |
| `node_grep_search_memchr`       | glibc memchr (AVX2 PCMPEQB)                 | ≥ 1-byte literal prefix                |
| `node_grep_search_byteset`      | N × `vpcmpeqb` + OR (≤ 8 bytes)             | 小さい first-byte set (alt of literals) |
| `node_grep_search_range`        | `vpsubusb / vpminub / vpcmpeqb`             | 単一連続範囲の first class             |
| `node_grep_search_class_scan`   | Hyperscan-style Truffle (PSHUFB × 2 + AND)  | 任意 256-bit first class               |

設計上のポイントは **prefilter と bake が合成する** こと。
`node_grep_search_memchr(/static/)` の SD は memchr 呼び出し AND `"static"`
を候補位置で verify する inline 化されたチェーンの**両方**を含む —
両方が同じ SD 内、両方が gcc の optimiser から見える、両方が dlsym
で到達可能。各 prefilter を足すのに bake / hash / code-store 機構を
変える必要は無かった — フレームワークが自然と正しく振舞った。

ベンチ影響、118 MB コーパス、full-sweep count、ms/iter (★ = AOT が
grep / Onigmo の両方に勝利):

| パターン | astrogre +AOT | grep | onigmo | 発火した prefilter node |
|---|---:|---:|---:|---|
| `/(QQQ\|RRR)+\d+/` | **16** ★ | 85 | 726 | byteset over {Q,R} |
| `/(QQQX\|RRRX\|SSSX)+/` | **24** ★ | 26 | 700 | byteset over {Q,R,S} |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | **503** ★ | 533 | 717 | range `[a-z]` |
| `/[A-Z]{50,}/` | **678** ★ | 1570 | 1099 | range `[A-Z]` |
| `/\b(if\|else\|for\|while\|return)\b/` | 90 | **2.3** | 1060 | byteset over {i,e,f,w,r} |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 10824 | **2.7** | 9353 | Truffle on `\w` (common) |

**grep に 4/8 勝、Onigmo に 8/8 勝**。負けているパターンはどれも
multi-pattern literal extraction (Hyperscan Teddy / FDR) を要するもの
で、パターン中央の rare literal `(`、`,`、`)` を引き抜くアプローチが
必要。これが次の大きい追加項目 (`todo.md` 参照)。

ladder を拡張する候補 node (まだ手付かず、同じ形):

| node                          | アルゴリズム               | parser trigger                                    |
|-------------------------------|---------------------------|---------------------------------------------------|
| `node_grep_search_teddy`      | multi-pattern AVX2 scan   | パターンの任意位置に ≥ 1 個の固定 literal あり    |
| `node_grep_search_bmh`        | Boyer-Moore-Horspool      | `-F` モード短い固定パターン                       |
| `node_grep_lines`             | newline scan + per-line   | grep CLI driver                                   |
| `node_grep_search_ci2`        | twin memchr for /i        | case-insensitive literal-led pattern              |

### bake (固有) の効きそうな残り
grep 形ワークロードでは bake 単独の勝ちは小さく、エンコーディング /
flag specialization 周りに集中する:

- **UTF-8 dot leading-byte cascade**。`node_re_dot_utf8` の 4-way
  branch (`b < 0x80` / `0xC0` / `0xE0` / `0xF0`) は ASCII-only と
  分かっている入力では潰せる。だがそれは*ランタイム*の入力性質で
  あってパターンの parse-time 性質ではない — profile signal なしには
  bake が見えない。
- **Case-fold backref**。`node_re_backref` は `c->case_insensitive`
  branch を持つ。parse 時に ci / non-ci variant を分ければ bake 不要
  で消える — parser が flag を知っている。
- **クラス bitmap を定数として**。bake が `bm0..3` を即値に commit。
  小さいクラス (`[abc]`) なら gcc が switch table に展開するかも。
  `class-word` で ~1.11×、小さい。

### 要約
- algorithmic optimization は新しい node 種類として住まわせる; bake は
  そのあと AST の他の部分と無料で合成する。
- bake の貢献が綺麗に出るのは、内側の per-iter の仕事が trivial でない
  とき。grep 形ワークロードのように prefilter が重い仕事の大半を
  食ってしまう領域では、bake は noise レベル。
- 「ASTro が速い」には両方が要る: per-node のアルゴリズム工夫
  (memchr、PSHUFB、BMH, …) AND specialization (アルゴリズム外殻が
  内部で wrap する regex verify と合成できるように)。

## ドライバ: 上に乗っている grep

`main.c` が grep フロントエンド。それ自体は regex の仕事を一切
しない; 全部バックエンド ops を経由する。要点:

* ファイル / stdin から `getline`、`backend->search` を 1 行ずつ
  呼ぶ — パターンコンパイルは 1 パターンに 1 度、入力全体で再利用。
* `--color` / `-o`: `backend->search_from` をループで動かして
  1 行内の全マッチを enumerate。zero-width マッチはカーソルを 1 byte
  進めて空回りを避ける。
* `-w` (whole-word): regex レベルで `\b...\b` で包む (`-F` のときは
  literal を先に escape)。
* `-r`: `opendir` + 再帰下降; デフォルトでドットファイルをスキップ。
* `--via-prism`: 各 `-e PATTERN` を、その引数を Ruby ソースとして
  prism で parse して見つかる最初の `/.../` の本体で置き換える。
  Ruby コードのスニペットを直接渡したいときに便利。
* whole-file mmap 経路 (`process_buffer`): regular file かつパターンに
  SIMD/libc prefilter があるとき (memchr / memmem / byteset / range
  / class_scan) 自動で発火する高速化路。ファイルを一度 mmap、
  `backend->search_from` を whole buffer に対し回し、各マッチの含まれる
  行を memrchr/memchr で identify。`-v` invert モードはこの path を
  スキップ (非マッチ行も列挙する必要があるため)。`backend.h` の
  `has_fast_scan` op で gate される — Onigmo backend は op を NULL
  のまま ("常に yes" — Onigmo は内部 prefilter あり)。
