# astrogre 性能ノート

このドキュメントは、astrogre が実際に他の正規表現エンジンと比べて
どれくらい速いか、どこで勝ってどこで負けるか、なぜそうなるかを
ベンチマーク結果に沿って書く。実装上の最適化のうち効いたもの・
効かなかったものも記録する。

astrogre は ASTro フレームワークの上で動く正規表現エンジン。
ASTro の中心機能は **AOT 特化 (specialization)** で、AST の各ノードを
ノード固有の C 関数として焼き出し、子ノードを inline で展開して
1 つの SD (= Specialized Dispatcher、特化された関数) に融合する。
search ループそのもの (= 入力中のすべての位置でマッチを試す for
ループ) も AST のノード `node_grep_search` として表現してあるため、
ループも regex マッチングロジックも一緒に焼かれて、indirect call が
ゼロの 1 関数になる。

実行モードは 4 つ:

- `--plain` (`--no-cs`): code store を使わない素のインタプリタ
- default: 起動時に過去にビルドした特化コードがあれば dlopen して使う
- `--aot-compile` (`-C`): その実行で AST を特化コンパイル → リンク → 使う
- `--pg-compile` (`-P`): profile 情報を使った特化 (まだ未配線、現状は `-C` と同じ)

以下、これらを比較する 2 種類のベンチマークを示す。

## ベンチマーク

### 計測上の注意 — `/dev/null` トリック

GNU grep は 2016 年の commit
[`af6af28`](https://cgit.git.savannah.gnu.org/cgit/grep.git/commit/?id=af6af288)
("/dev/null output speedup", Paul Eggert) で、stdout を `fstat`
して `/dev/null` だと判明した場合に **first-match-and-exit
mode (= `-q` 相当)** へ切り替える最適化が入っている。
コミットメッセージ曰く `seq 10000000000 | grep . >/dev/null` が
**380,000× 速くなる**。

つまり、ベンチで `> /dev/null` を使うと grep は「最初の 1
マッチを見つけたら即終了」していて、118 MB ファイルを最後まで
舐めていない。astrogre や ripgrep には同じ最適化がないので、両者
の比較が **桁違いに不公平**になる。

`bench/grep_bench.rb` / `bench/aot_bench.rb` / `bench/tree_bench.rb`
はすべて **regular file (`/tmp/*.out`) へリダイレクト**して計測
している。以下の表もそのモード。

### Bench A: grep CLI モード — `bench/grep_bench.rb`

実利用に近い形。1 ファイル (118 MB の C ソースコーパス) を `are PATTERN file`
形式で開いて行ごとに結果を出す。`are` は astrogre engine の grep CLI、
`-j 1` で並列 walker を切ってエンジン単体の比較にする。Onigmo backend
は `WITH_ONIGMO=1` で再ビルドしたときだけ列挙される。

| パターン                                | are interp | are AOT cached | are +onigmo | rg | grep |
|----------------------------------------|---:|---:|---:|---:|---:|
| `/static/`                             | **38** | 38 | 110 | 41 | 76 |
| `/specialized_dispatcher/`             | **19** | 22 | 34 | 20 | 34 |
| `/^static/` anchored                   | 75 | 78 | 102 | **38** | 66 |
| `/VALUE/i` case-insens                 | 80 | 71 | 146 | **55** | 97 |
| `/static\|extern\|inline/` alt-3       | 392 | 119 | 990 | **58** | 185 |
| 12-way alt (AC prefilter)              | 379 | 378 | 2582 | **133** | 257 |
| `/[0-9]{4,}/` class-rep                | 363 | 136 | 569 | **54** | 86 |
| `/[a-z_]+_[a-z]+\(/` ident-call        | 1824 | 632 | 3391 | **204** | 2211 |
| `-c /static/` count                    | **23** | 23 | 70 | 28 | 63 |

ms、`taskset -c 4` 固定 + 7 回実行の最速。太字は行内の最速。
honest 集計: are 3 勝、rg 6 勝、grep 0 勝。

#### 読み取り

- **AOT が interp を上回るのは body chain が長いパターン**: alt-3
  で 392→119 ms (3.3×)、class-rep で 363→136 ms (2.7×)、ident-call
  で 1824→632 ms (2.9×)。SD specializer が candidate 位置で走る
  body chain (cap_start → alt/lit → cap_end → succ) を 1 個の SD
  関数に折り畳む。
- **`-c /static/` で are が rg / grep を抑えて勝つ** (23 ms vs rg
  28 ms vs grep 63 ms)。`node_grep_count_lines_lit` 経由で scan +
  lineskip + count を 1 ループに融合。
- **`/static/` 単独 print も are が最速** (38 ms vs rg 41 vs grep
  76)。dual-byte SIMD scan + emit-line action chain が AOT で 1
  basic block に焼かれる。
- **ripgrep が anchored / `/i` / class-rep / alt-3 / ident-call で
  勝つ**。lazy DFA + literal-prefix prefilter の組合せが構造的に
  強く、AOT で chain inline しても追いつけない箇所がある。
- **alt-12 (AC prefilter)**: rg の AC + Teddy SIMD multi-pattern
  は per-byte で 2-3× 速い。todo.md に `Teddy 風 SIMD AC` を残置。

#### `/static/` の内訳 — `--verbose` から

```
[verbose] after INIT()            0.12 ms      ← dlopen + 動的リンカ
[verbose] after pattern compile   0.13 ms      ← 正規表現のパース
[verbose] after mmap             10.04 ms      ← 118 MB の mmap (~30 k PTE 構築)
[verbose] after scan             18.09 ms      ← SIMD スキャン本体
[verbose] after munmap           23.53 ms      ← PTE の破棄
```

実スキャンは 8 ms (= 18.09 − 10.04)、約 15 GB/s。残り 13 ms は
OS のページテーブル構築/破棄で、118 MB を mmap する以上不可避な
物理コスト。grep が 153 ms と遅いのは output のフォーマット
(`fname:lineno:line` の sprintf + write) が dominant な行を
160k 個生成するため。`-c` モードの 71 ms が grep の "scan 専念"
時間で、その 81 ms 差が format/write の per-match コスト。

`/static/` のスキャン側を高速にしているのは、astrogre が
**入力を行ごとに走査するループそのものを AST ノードとして実装した**
こと。`node_scan_lit_dual_byte` が Hyperscan 風 dual-byte filter
(needle の先頭バイトと末尾バイトを別々に SIMD 比較してから AND
を取り、64 バイトずつ進める) で候補位置を見つけ、その後ろにつなぐ
小さな action ノード列 (count → emit_match_line → lineskip →
continue) でカウント・行表示・改行スキップを行う。CLI のモード
ごとに後ろの action 列だけが入れ替わる構造。AOT 焼き時は scanner
+ action 列が **1 個の SD 関数** に統合される。

### Bench B: エンジン単独ベンチ — `bench/aot_bench.rb`

CLI のオーバーヘッド (起動、mmap、行ごとの I/O) を取り除いて
**マッチャ単独の速度**を測る。astrogre は `selftest_runner bench-file`
モードで 118 MB バッファを 1 度メモリに置き、エンジンの search 関数を
ループ呼び出しして全マッチを数える。grep / ripgrep は比較のため `-c`
(count モード)。

選んだパターンは「astrogre の prefilter が効かず、chain が長い」
もの。AOT が indirect call を消した効果が一番見える形。

| パターン                                          | interp | +AOT | +onigmo | grep | rg |
|--------------------------------------------------|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/`                                | 23 | **13** | 510 | 94 | 23 |
| `/(QQQX\|RRRX\|SSSX)+/`                           | 48 | 25 | 539 | 26 | **24** |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`             | 1023 | 646 | 558 | 701 | **181** |
| `/[A-Z]{50,}/`                                    | 479 | **156** | 945 | 1587 | 180 |
| `/\b(if\|else\|for\|while\|return)\b/`            | 284 | **120** | 914 | 825 | 123 |
| `/[a-z][0-9][a-z][0-9][a-z]/`                     | 1021 | 445 | 552 | 1005 | **180** |
| `/(\d+\.\d+\.\d+\.\d+)/`                          | 430 | 105 | 565 | **82** | 185 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/`              | 6958 | **2086** | 12291 | 5708 | 217 |

ms/iter、`taskset -c 4` 固定 + 3 回実行の最速。太字は行内の最速。

#### 読み取り

- **AOT は interp 比 1.6×–3.3× の高速化**を全行で出す。これが
  ASTro framework のコア成果。`(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)` の
  capture-heavy chain は 6958→2086 ms (3.3×)、`(\d+\.\d+\.\d+\.\d+)`
  は 430→105 ms (4.1×)、`\b(if\|else\|...)\b` の alt-of-LIT は
  284→120 ms (2.4×)。
- **vs Onigmo: 8/8 勝ち** (3-15× ペース)。Onigmo は bytecode VM、
  alt branch / quantifier iter ごとに dispatch する; AOT は indirect
  call 無しの flat SD に折り畳む。
- **vs GNU grep: 6/8 勝ち**。
  - `[A-Z]{50,}`: AOT 156 vs grep 1587 (10×)。grep の DFA は連続
    大文字 50 個以上で破裂、greedy_class fast-path が SIMD で進める。
  - `\b(if\|else\|...)\b`: AOT 120 vs grep 825 (7×)。
  - `(\w+)\s*\(...\)`: AOT 2086 vs grep 5708 (2.7×)。
  - 1 敗 (`(\d+\.\d+\.\d+\.\d+)`): grep の Boyer-Moore-Horspool が
    `\d` 疎なコード入力で skip ahead しやすい。
- **vs ripgrep: 3/8 勝ち + 1 引き分け**。
  - 勝つ行: `(QQQ\|RRR)+\d+`、`[A-Z]{50,}`、`\b(if\|else\|...)\b`。
    どれも byteset prefilter or greedy_class fast-path が刺さる場面。
  - 負ける行: `(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)` で 10× 負け
    (2086 vs 217)。rg は AC/Teddy で `(`, `,`, `)` を **任意位置の
    リテラル anchor** として抽出する。astrogre は先頭リテラル
    しか見ない (我々の AC 実装は `(cat|dog|...)` 形式専用)。
    これが現在 rg に対する一番大きいギャップ。

#### grep が勝つ唯一の行: `(\d+\.\d+\.\d+\.\d+)` の 82 ms

数字バイト (`0-9`) はコードに疎なので grep の Boyer-Moore-Horspool
が skip ahead しやすく、scan 量が物理的に少ない。astrogre の
`greedy_class` は SIMD だが全 byte を舐めるので速度差が出る。
これは「希少バイトだけ拾えば後段が短い」ケースで、構造的勝負。

## AOT (specialization) が効くところ・効かないところ

AOT がよく効くのは「per-position の dispatch (= 関数ポインタ
経由の関数呼び出し) が多い」場面に限る。indirect call を消す
ことが本業の最適化なので、消す材料が無いと効かない。

具体的には:

- **ノード数が少ない短いチェーン** (= 1 位置で 2-3 ノード) では
  bake は 1.1-1.2× 程度。`/[A-Z]{50,}/` の 1.18× がこれ。
- **ノード数が多いチェーン** (= 1 位置で 5-9 ノード) では bake は
  2-3×。`/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` の 2.06× や
  `/\b(if|else|...)\b/` の 3.07× がこれ。
- **prefilter が効くケース** (= 候補位置がスパース) では、bake が
  消すべき dispatch の総量自体が小さい (大半のバイトは scan の中で
  飛ばされる) ので、AOT の貢献は小さい。代わりに prefilter ノード
  そのものが本業 (アルゴリズム勝ち)。

過去の最高記録は `literal-tail` という人工 microbench での 7.22×
(SD が 30 命令前後の 1 関数に畳まれた)。現実の正規表現では 2-3×
が実用上の天井で、それ以上は scan 自体のメモリ帯域に制限される。

## 実装上の最適化 — 効いたもの

### Search ループを AST ノードに

`node_grep_search` は「入力の各位置で `body` を試す」for ループを
そのまま AST ノードとして実装している。`body` operand は regex の
本体 (= cap_start → re_lit → cap_end → succ のような chain)。
特化処理が `body` operand に再帰し、子の dispatcher を直接の
関数ポインタとして埋め込むと、gcc は for ループと regex chain を
1 つの関数に融合する。`literal-tail` 7.22× の出所。

### 行ごとの走査ループも AST ノードに (case-A factorization)

`-c` モードと通常表示モードの「ファイル全体を行ごとに走査して
マッチを数える / 出力する」ループを `node_scan_lit_dual_byte` という
別のスキャナノードにして、その `body` に小さな action ノード列を
継ぐ:

| CLI mode | body chain |
|---|---|
| `-c LIT` | `count → lineskip → continue` |
| 通常表示 `LIT FILE` | `count → emit_match_line(opts) → lineskip → continue` |

action ノードはどれも 1 つの仕事 (count: カウンタを増やす、
lineskip: 次の改行までスキップ、emit_match_line: 行を整形して出力、
continue: スキャナに「続行」を返す) を持ち、`next` operand で次の
ノードを呼ぶ continuation-passing 形式。スキャナと action の
組み合わせは AOT 焼きで 1 SD に融合する。

`/static/` の `-c` で 64 ms → 23 ms (2.8×)、通常表示で 66 ms →
28 ms (2.4×) という主要な高速化はこの仕組みから来ている。

### prefilter ladder

正規表現が「最初に何を消費するか」のシェイプに応じて、
パーサが 5 種類の SIMD/libc ベースのスキャナノードを使い分ける:

| ノード | アルゴリズム | パース時の選択条件 |
|---|---|---|
| `node_grep_search_memmem` | glibc memmem (two-way string match) | 4 バイト以上のリテラル先頭 |
| `node_grep_search_memchr` | glibc memchr (AVX2 PCMPEQB) | 1 バイト以上のリテラル先頭 |
| `node_grep_search_byteset` | N 個の `vpcmpeqb` を OR (N ≤ 8) | alt の各枝の先頭バイト集合 |
| `node_grep_search_range` | `vpsubusb` / `vpminub` / `vpcmpeqb` | 単一連続範囲のクラス先頭 (`[a-z]` 等) |
| `node_grep_search_class_scan` | Hyperscan 風 Truffle (PSHUFB × 2 + AND) | 任意 256-bit クラス先頭 (`\w` 等) |

ノードはそれぞれ「候補位置を見つける」専用の SIMD コードを持ち、
候補位置で内側の regex chain を試す。AOT 焼きで scan 部分と
verify 部分が同一 SD に inline 化されるので、バイトレベルで
固有定数 (memchr のターゲットバイト、range の上限・下限 等) が
即値になる。

### EVAL_ARG (chain inline) と EVAL (runtime indirect) の分離

dispatch site は **どこから dispatcher 値が来るか** で 2 種類に
分けられる:

- **EVAL_ARG(c, X)**: `X` が NODE_DEF の **直接 param** (= 親 node の
  static body operand 由来)。ASTroGen の `DISPATCH_xxx` は `X` の
  dispatcher を別 param として EVAL_xxx に渡すので、SD specializer
  はその dispatcher 値を **直接呼び出し** に展開できる。chain inline
  → indirect call ゼロ → 1 個の SD 関数に折り畳まれる。
- **EVAL(c, X)**: `X` が CTX field / stack frame / runtime selection
  から来る (例: `c->rep_cont_sentinel`、`f->body`、`c->sub_chains[idx]`、
  `c->sub_top->return_to`)。SD 生成時には dispatcher 値が分からない
  ので、runtime に `X->head.dispatcher` を読む真の indirect call。

node.def の dispatch site を grep 可能なマクロにしたうえで、
chain 部分は EVAL_ARG、runtime indirect は EVAL に分けて書く。
これで「どの site が SD 内 inline、どの site が dlsym 経由」が
ソースレベルで明示される。

### entry node の正しい登録 — `astro_cs_compile` を必要箇所だけ呼ぶ

EVAL(c, X) で叩かれる NODE は **public extern symbol** として SD
に出ないと dlsym で見つからず、interp dispatcher のままになる。
chain bouncing (interp ↔ SD ping-pong) を防ぐには、これらを
全部 `astro_cs_compile(node, NULL)` で entry 登録する必要がある。

**重要なのは「全 NODE 登録ではない」点**。chain inline される
EVAL_ARG site の NODE は parent SD の中で `static inline _INL`
として展開される ─ 個別に extern にする必要は無い。framework の
`astro_cs_compile` は entry を public、subtree を `static inline`
として 1 SD ファイルに出すので、chain dispatch の NODE はそこに
吸収される。

EVAL される (= entry になる) NODE はソース上で機械的に列挙可能:

| dispatch site | entry NODE |
|---|---|
| `node_re_rep` の `c->rep_cont_sentinel` 呼び | `rep_cont_sentinel` (process global singleton) |
| `rep_cont_inner` の `f->body`, `f->outer_next` | 各 `node_re_rep` の body / outer_next operand |
| `node_re_subroutine_call` の `c->sub_chains[idx]` | 各 `p->sub_chains[i]` (= 各 `\g<i>` の lower 結果) |
| `node_re_sub_return` の `c->sub_top->return_to` | 各 `node_re_subroutine_call` の outer_next operand |
| pattern root | `p->root`、`p->count_lines_root`、`p->print_lines_root` |

`lower_ctx_t::entries` に IR-lower 時に push しておき、
`astrogre_pattern_aot_compile` で iterate して `astro_cs_compile`
に渡す。この方式により以前あった「全 SD を後処理で `_INL`+wrapper
化する」ファイル書き換え hack (~140 行) は撤去できた。

#### なぜ `node_re_rep_cont` だけ singleton なのか

各 rep ごとに per-rep cont を allocate して body / outer_next を
struct field にする案 (= EVAL を完全に EVAL_ARG に置換する案) は
**subroutine 再帰と本質的に conflict** する。rep_cont が固定された
NODE になると `sub_call.outer_next` が静的に「自分のレキシカル
コンテキストの cont」を指してしまい、再帰で多段 unwind するときに
正しくない cont を dispatch する。OLD の singleton 設計は **`c->rep_top`
で動的に「今どの rep の中か」判定** することで、`(\((?:[^()]|\g<paren>)*\))`
のような任意深さの再帰を 1 個の generic NODE で処理できる。

per-rep 案は depth=3 以上の `(((triple)))` で `c->rep_top = NULL`
で crash することを実装で確認した (revert 済)。

### サイドアレイによる allocate 後再解決

`node_allocate` がすべての NODE をサイドアレイ
`astrogre_all_nodes` に登録、AOT ビルド完了後にこの配列を歩いて
各ノードに対して `astro_cs_load` を再実行する。これがないと、
今走っているプロセス内では古い dispatcher のままで、次回以降の
起動でしか新しい SD が効かない。

### パーサ時の隣接リテラル統合

`/Hello/` を 5 個の `lit` ノードではなく 1 個の `lit("Hello", 5)`
にする。dispatch 数が減るし、SD ファイル数も減る。

### パーサ時の case fold 事前展開

`/Hello/i` は `node_re_lit_ci("hello", 5, ...)` (小文字で保持) に
lower される。マッチ時はパターン側を毎回 fold せず、入力側の各
バイトだけを「大文字なら 32 足す」で fold して比較する非対称
ポリシー。

### `\A` 始まりの短絡

`/\Afoo/` のような pattern 先頭アンカーの場合、`anchored_bos`
operand を `node_grep_search` に渡しておくと、search ループは
「位置 0 だけ試して終わる」ことを静的に知れる。AOT 焼きでも
`anchored_bos = 1` が即値として焼かれるので分岐ごと消える。

### 単一 `rep_cont` センチネル

繰り返しノード (`*`、`+`、`{n,m}`) の body の `next` は、起動時に
1 個だけ確保される sentinel ノード `node_re_rep_cont` を全 rep
ノードで共有する。これで AST が DAG (= 後ろ向き辺なし) のまま
残り、構造ハッシュの計算と code-store でのサブツリー共有が成立する。
個別の rep ノードごとに別々の continuation を持たせていた初期案では
AST に cycle ができて構造ハッシュが破綻していた。

### `restrict` と inline 256-bit クラスビットマップ

CTX とノードのポインタ引数に `restrict` を付けて gcc に alias 自由を
保証。文字クラスは 256-bit ビットマップを `uint64_t × 4` として
ノード struct に直接埋める (ヒープ参照させない)。標準的な C 高速化。

## 実装上の最適化 — 効かなかった、棄却したもの

### rep ノードごとの専用 continuation

各 `rep` ノードに専用の continuation ノードを割り当てる初期案。
AST に後ろ向き辺ができて構造ハッシュが循環、code-store の
サブツリー共有が成立しなくなった。単一 sentinel に変更して解決。

### 「body は呼び出しごとに 1 回成功を返す」という repetition モデル

`(a|ab)*c` を `abc` にマッチさせる際、body の中の alt は
「最初の枝 `a` でマッチ、次に外側 `c` を試す → 失敗」のあとで、
内側の alt の **2 番目の枝** (`ab`) も試す機会が必要になる。
body を「呼び出しごとに最後まで決め打って 1 回返す」と書いてしまうと
このバックトラックが取れない。現状の rep_cont は「呼び出しの **続き**
として動く」形にしてあって、外側の失敗を内側の alt が受け取れる。

### UTF-8 マルチバイト文字クラス

クラスを「256-bit ASCII ビットマップ + (lo, hi) コードポイント
範囲のソート済リスト」のハイブリッドで持つ案。バイナリサーチが
dispatch を支配して没。マルチバイトクラス対応自体はまだ未実装。

### クラスの bytes を `const char *` で外置きする

ノード struct を 24 バイト削れるが、マッチャがビットマップを
ロードするために間接参照が増える。実測で `class-word` ベンチが
12 % 遅化したので没、inline 維持。

### possessive 量化子 (`*+`、`++`、`?+`)

パースのみ受け付けて greedy にデグレード。意味論的には commit
バリアが必要で、今の rep_cont プロトコルに穴を開けて入れる必要が
ある — 需要待ち。

### profile-guided コンパイル (PG)

`--pg-compile` フラグは abruby との CLI 互換のため受け付けるが、
正規表現用の profile 信号 (alt の各枝のヒット率、よく使われる
反復回数、参照されないキャプチャ等) を集めるパスが未実装。
現状は `--aot-compile` と同じ経路を通る。

## 実装した最適化の付録: 焼かれた SD のアセンブリ

`/static/` を `node_grep_search_memmem` で wrap した SD の
逆アセンブリ (約 30 命令、indirect call なし、DISPATCH chain なし、
SD 外の関数呼び出しもなし):

```
SD_<hash>_INL:
    endbr64
    mov    8(%rdi),%rcx          ; c->str_len
    mov    0x10(%rdi),%rax       ; c->pos
    lea    1(%rcx),%rdx
    cmp    %rdx,%rax
    jae    66f                   ; pos >= len + 1 → 終了
    ...
    vpxor  %xmm0,%xmm0,%xmm0     ; capture-state リセット用 ymm0 = 0
.loop:
    lea    -6(%rax),%rdx
    mov    %rdx,0x10(%rdi)        ; c->pos = s
    vmovdqu %ymm0,(%rsi)         ; valid[] 32 個を SIMD で一括クリア
    ...
    cmp    %rax,%rcx
    jb     fail
    add    (%rdi),%rdx           ; str + pos
    cmpl   $0x74617473,(%rdx)    ; "stat" を即値で比較
    je     check_ic
    inc    %rax
    cmp    %rax,%r8
    jne    .loop
    ret_zero
check_ic:
    cmpw   $0x6369,4(%rdx)        ; "ic" を即値で比較
    jne    fail
    ; ends[0] と valid[0] をセット、return 1
```

`"static"` の各バイトが SD ソースに baked literal として埋め込まれた
結果、gcc が `cmpl $0x74617473` ("stat" の little-endian) と
`cmpw $0x6369` ("ic") の即値命令にまで畳んでいる。

## 着手前のもの

- **マルチパターン Hyperscan-Teddy 風の literal anchor 抽出**。
  パターンの**任意位置**にあるリテラル (例:
  `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` の `(`, `,`, `)`) を抜き
  出して anchor にする。ugrep に対する最大のギャップ。
- **AST 内の行イテレーション** (`-c` 以外も)。`-c` と通常表示
  モードでは入っているが、`-l` (ファイル名のみ) や `-o`
  (マッチ部分のみ) はまだ main.c の C ループで動いている。
- **first-byte ビットマップ**。Boyer-Moore-Horspool より単純で、
  「許される first byte」の 256-bit ビットマップを SIMD で
  チェックして skip。
- **本物の PG 信号**。alt 各枝のヒット率を計測して hot な枝を
  最初に試す並べ替え、参照されないキャプチャの save/restore 省略、
  反復回数の集中したパターン用の固定 N 展開など。
- **JIT** (= ランタイム特化)。今は AOT のみ (起動時に dlopen で
  load)、JIT 経路 (実行中に dlopen し直す) は ASTro 標準として
  あるが配線していない。

詳細は [`todo.md`](./todo.md)。
