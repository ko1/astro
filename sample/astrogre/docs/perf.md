# astrogre 性能ノート

これは v1 サンプル。ASTro の specialization / code-store 機構が
abruby 風モード (`--aot-compile` / cached / `--pg-compile` /
`--plain`) で配線されており、search ループ自体も AST node
(`node_grep_search`) なので specialization は for-each-start-position
ループと inline 化された regex chain を 1 つの SD 関数に fuse する。
このドキュメントは、現在の数字に至るまでに**効いたもの・効かなかった
もの**を記録する。

## 他エンジン scoreboard

ベンチは 2 種類並走:
- **Bench A**: grep CLI ベンチ (line-by-line、grep の実利用に近い形)
- **Bench B**: engine-level whole-file ベンチ (コーパス一括ロード、
  `astrogre_search` をループ呼び出し — エンジン純コストを isolate)

### Bench A — grep CLI (line-by-line, 118 MB コーパス, ms, best-of-5)

`bench/grep_bench.sh`。全 4 エンジンをそれぞれの grep CLI / `-c` モード
で起動。

| パターン | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/static/` literal | 77 | 78 | 98 | **2** | 34 |
| `/specialized_dispatcher/` rare | 26 | 26 | 38 | 35 | **20** |
| `/^static/` anchored | 76 | 76 | 98 | **2** | 36 |
| `/VALUE/i` case-i | 704 | 682 | 129 | **2** | 50 |
| `/static\|extern\|inline/` alt-3 | 308 | 313 | 959 | **2** | 50 |
| `/[0-9]{4,}/` class-rep | 475 | 471 | 565 | **2** | 55 |
| `/[a-z_]+_[a-z]+\(/` ident-call | 3277 | 3283 | 3177 | **2** | 185 |
| `-c /static/` count | 48 | 50 | 72 | **2** | 27 |

whole-file mmap 経路 (パターンに SIMD/libc prefilter があるとき発火)
が literal-led astrogre を従来の per-line getline ループより 3-10×
落とす。**rare-literal パターンでは astrogre が ripgrep 並み**
(`literal-rare` 26 ms 対 ripgrep 20 ms; ugrep の 35 ms より速い)。
common literal (`/static/`) では ugrep が memory-bandwidth-bound
memchr で 1 桁先。`/i`、`class-rep`、`ident-call` の行は per-line
streaming にフォールバック (エンジンが leading `\w` / `[0-9]` /
`/i` 形に対する fast scan を持たないため) — per-line overhead が
消えた状態の AOT の挙動は Bench B を参照。

### Bench B — engine-level whole-file scan (ms/iter, best-of-3)

`bench/aot_bench.sh`。astrogre は `--bench-file` (バッファをまるごと
メモリに置き、全マッチをカウント); grep / ripgrep / onigmo は `-c`。
パターンは AOT が効きやすい形 (chain が長く、bake の dispatch 削除が
意味を持つ) を選択。★ = astrogre + AOT が grep / Onigmo の両方に勝利。
太字 = 行内ベスト。

| パターン | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | 19 | **12** ★ | 488 | 74 | 23 |
| `/(QQQX\|RRRX\|SSSX)+/` | 40 | **23** ★ | 535 | 27 | 25 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 926 | **444** ★ | 548 | 507 | 185 |
| `/[A-Z]{50,}/` | 741 | **640** ★ | 919 | 1525 | 185 |
| `/\b(if\|else\|for\|while\|return)\b/` | 252 | 90 | 894 | **2.5** | 118 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 1008 | 429 | 535 | **4** | 186 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 566 | 397 | 554 | **4** | 48 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 13061 | 10096 | 14532 | **5** | 351 |

**この set で grep に 4/8 勝、Onigmo に 8/8 勝**。勝因は prefilter
ladder — memchr / memmem / byteset / range / Truffle がそれぞれ「パ
ターンが最初に消費するもの」の異なる形を扱い、specialiser が inline
化された regex chain と合成する。

負けているパターンは共通の性質を持つ: ugrep がパターン内の複数の
literal を anchor として抽出し、Hyperscan-style multi-pattern スキャン
(Teddy / Aho-Corasick) を回す。astrogre はそういう「任意位置」の
literal anchor 抽出を未実装 — `(\w+)\s*\(\s*(\w+)\)` には強制
literal `(`、`,`、`)` があるが、prefix 解析が先頭しか見ない。
「任意位置 literal anchor」抽出が次の最大 lever (todo.md 参照)。

## AOT specialization が効くところ

オリジナルの microbench にあった literal-led パターンは memchr /
memmem prefilter (次節) に食われた — bake が触る前に prefilter が
処理するので、もう面白くない。AOT の効きは**反対の形**で見える:
chain が長い、固定 first byte がない (prefilter 不発)、各位置の
作業量がそこそこあって dispatch overhead の比率が大きい。

`bench/aot_bench.sh` は in-engine `--bench-file` 経路 (118 MB バッファ
を一度ロード、各 iter で全マッチ count) を使い、grep / ripgrep /
Onigmo を `-c` で比較。全て ms、best-of-3:

| パターン | interp | aot | aot/I | +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | 2341 | 1053 | 2.22× | 696 | 79 | 24 |
| `/(QQQX\|RRRX\|SSSX)+/` | 3052 | 998 | 3.06× | 720 | 28 | 27 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 854 | 377 | 2.26× | 777 | 597 | 215 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 909 | 404 | 2.25× | 808 | **5** | 225 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 1557 | 1101 | 1.42× | 755 | **5** | 50 |
| `/[A-Z]{50,}/` | 1285 | 1182 | 1.09× | 1111 | 1535 | 187 |
| `/\b(if\|else\|for\|while\|return)\b/` | 1655 | 404 | 4.10× | 1078 | **2** | 119 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 20413 | 15583 | 1.31× | 10141 | **3** | 221 |

(これは prefilter 投入前の純粋 AOT bench; 上の Bench B は post-
prefilter 数値。)

AOT 2-3× の勝因は AOT specialization の thesis 通り:

- **Chain が長くないと効かない**。alt (`(QQQ|RRR)`) → rep → class /
  literal → … と各マッチ試行で 5+ ノードを通れば、bake が消す
  indirect call も 5+ 個。
- **Prefilter が**発火**してはいけない**。これらのパターンは class
  / alt 始まりなので memchr / memmem が pre-skip できない、chain は
  全位置で走る。
- **Search が入力全体を歩く**必要がある。これらのパターンは 118 MB
  全体をスイープする (失敗パターンは exit せず尽く、成功パターンも
  全マッチをカウント)。

`/[A-Z]{50,}/` は全入力をスイープするのに 1.09× にとどまる — chain が
rep_cont ↔ class の 2 ノードしかないので、dispatch がそもそも安い。

### vs Onigmo

astrogre + AOT が Onigmo に勝つ 3 例が興味深い:

- `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`: 377 ms 対 777 ms (**Onigmo より 2.06× 速い**)
  — 1 試行に class チェック 9 個、ASTro は全部 1 関数に inline、Onigmo の
  bytecode VM は各々 dispatch。
- `/[a-z][0-9][a-z][0-9][a-z]/`: 404 ms 対 808 ms (**2.00× 速い**) — 同じ形、
  class チェック 5 個。
- `/\b(if|else|for|while|return)\b/`: 404 ms 対 1078 ms (**2.67× 速い**) —
  `\b` + alt-5; ASTro は alt 各枝を条件分岐に展開、Onigmo の VM は 1 つ
  ずつ走る。

Onigmo が勝つのは構造的近道を持っているパターン:

- `/(QQQ|RRR)+\d+/`、`(QQQX|RRRX|SSSX)+`: 共通 prefix 付き
  alternation の最適化があるはず。
- `/(\d+\.\d+\.\d+\.\d+)/`: greedy-dot + 末尾アンカー heuristic 系。

### vs grep / ripgrep

ugrep / ripgrep は literal-prefix prefilter / lazy DFA が発火する
パターンで 1 桁先 (`/[a-z][0-9][a-z][0-9][a-z]/` ← grep 5 ms。
leading `[a-z]` が memchr-class スキャンに崩されている)。

しかし prefilter が適用できないパターンでは、ASTro+AOT は **grep
より速い**:

- `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`: astrogre+AOT 377 ms 対
  grep 597 ms — grep にはこの形に効く prefilter がなく、各位置で
  PCRE-class マッチにフォールバック; ASTro の inline チェーンの
  ほうが単に速い。
- `/[A-Z]{50,}/`: astrogre+AOT 1182 ms 対 grep 1535 ms — 同じ事情。

ripgrep は全体的に常に速い (lazy DFA + literal-prefix prefilter)
が、エンジニアリング投資量が桁違い。

### 上限

冒頭 microbench の `literal-tail /match/` 7.22× は今でも bake が
出せる**上限**として通用 (prefilter なし *かつ* memcmp が per-iter
を支配しない条件)。だが現実的な regex パターンでは AOT 単独で 2-3×
が実用上の天井 — それでも chain-heavy パターンでは Onigmo を
追い越せる、という結果。

`/static/` の fused SD の逆アセ (search ループ + inline 化された
regex match が約 30 命令、indirect call なし、DISPATCH chain なし、
SD 外への関数呼び出しなし):

```
SD_<hash>_INL:
    endbr64
    mov    8(%rdi),%rcx          ; c->str_len
    mov    0x10(%rdi),%rax       ; c->pos
    lea    1(%rcx),%rdx
    cmp    %rdx,%rax
    jae    66f                   ; pos >= len + 1 → exit
    ...
    vpxor  %xmm0,%xmm0,%xmm0     ; capture-state リセット用 ymm0 ゼロ
.loop:
    lea    -6(%rax),%rdx
    mov    %rdx,0x10(%rdi)        ; c->pos = s
    vmovdqu %ymm0,(%rsi)         ; valid[] 32 個を SIMD で一括クリア
    ...
    cmp    %rax,%rcx
    jb     fail
    add    (%rdi),%rdx           ; str + pos
    cmpl   $0x74617473,(%rdx)    ; "stat"
    je     check_ic
    inc    %rax
    cmp    %rax,%r8
    jne    .loop
    ret_zero
check_ic:
    cmpw   $0x6369,4(%rdx)        ; "ic"
    jne    fail
    set ends[0], valid[0]; ret 1
```

## 効いたもの

### Search ループを AST に折り込む
`node_grep_search` がパーサが各コンパイル済みパターンの最上位に置く
ラッパー。EVAL が for-each-start-position ループで、specialiser が
`body` operand に再帰し `body_dispatcher` を直接関数ポインタとして
inline すると、gcc がループと regex chain を indirect call ゼロの
1 関数に融合する。`literal-tail` 7.22× の出所。

### `_INL` リネーム + extern wrapper post-process
luastro の `luastro_export_sd_wrappers` をそのまま借用。これがないと
`astro_cs_load` はルート SD しか見つけられず、内側 node の SD は
`static inline` の裏に隠れたまま、ランタイム dispatch が host 側の
`DISPATCH_*` を経由して各 node ごとにバウンスする。

### Side-array 経由の build 後再解決
`node_allocate` が全 NODE を `astrogre_all_nodes` に track、
`astrogre_pattern_aot_compile` が build 後に array を歩いて各 node で
`astro_cs_load`。これがないと「次回起動時」しか SD が効かない (今走っ
ている run には間に合わない)。

### Parse 時の隣接リテラル coalesce
`\AHello, World/` を 13 個の lit node から 1 個に — dispatch も SD
ファイルも減る。

### Parse 時の case fold pre-fold
`/Hello/i` は `node_re_lit_ci("hello", 5, ...)` になる — pattern 側は
1 度だけ lowercase に、入力側は byte ごとに非対称に fold して比較。

### \A 始まりの short-circuit
`anchored_bos` を `node_grep_search` の operand として持たせると、
SD の position ループが「1 回だけ試して終わる」ことを静的に知れる。

### 単一 rep_cont sentinel
`node_re_rep_cont` allocation は全 rep node 共通でちょうど 1 つ。
これで AST が DAG (cycle なし) のまま — Merkle hash が成立し、
code-store の sharing も適用できる。

### バックエンド抽象
`backend.h` で grep CLI とマッチャを decouple。Onigmo backend の追加は
~150 行のラッパー + 60 行の `build_local.mk` (autoconf / libtool 抜きで
Onigmo をビルド)。

### `restrict` on `c` and `n`、256-bit クラスビットマップを `uint64_t × 4` インライン
標準的な ASTro hygiene。

## 効かなかった / 棄却した

### 1 rep node ごとの "static" continuation node
初期案では `rep_cont` を **rep node ごと** に allocate していた。AST
に cycle が生じて Merkle hash が破綻。

### 「body は呼び出しごとに 1 回成功を返す」repetition モデル
`(a|ab)*c` の `abc` で fail — 内側の alt は iteration 越しに retry
できる必要がある。

### UTF-8 マルチバイト char-class
class を 256-bit ASCII bitmap *に加えて* (lo, hi) コードポイント範囲の
ソート済リストとして持つ案。binary search が dispatch を支配して没。

### Class の bytes を `const char *` で持つ
ノード struct を 24 byte 削れるが、マッチャは bitmap ポインタを load
する必要が出る — `class-word` で inline が ~12 % 勝った。

### Possessive 量化子
parse のみ、greedy に degrade。

### PG (profile-guided) bake
`--pg-compile` は abruby との CLI 互換のため flag として受け付けるが、
profile signal がないので現状は `--aot-compile` と同等動作。regex 用の
本物の signal (hot-alternative reordering、hot 反復回数、参照されない
キャプチャの省略等) は今後の課題。

## 着手前のもの

* **AST 内の行イテレーション**。grep CLI の C 側 mmap 経路で大半カバー
  済みだが、行境界スキャンと per-line search dispatch + print/count
  side-effect を 1 つの SD として bake する `node_grep_lines(body, str,
  len, callback)` ノードを足せば、まだ残る per-call overhead をさらに
  下げられる。
* **マルチパターン Hyperscan-Teddy literal anchor 抽出**。これが ugrep
  に対する最大の miss。fixed-byte prefix なしのパターンでも、内部に
  必須リテラルがあれば anchor として使える。
* **First-byte bitmap**。完全な BMH より更に簡単 — compile 時に「許される
  first byte」の 256-bit bitmap を作り、ベクトル化スキャンで skip。
* **本物の PG signal**。各 `node_re_alt` での枝ヒット数を計測、hot な
  枝を最初に試す alternation を bake。Capture elision: profile run で
  参照されないグループの save/restore をドロップ。
* **JIT**。一度 compile すれば次起動でなくこの run の中で SD を生成
  したい場合の標準 ASTro JIT 経路。AST が DAG なので適用可能。

詳細は [`todo.md`](./todo.md)。
