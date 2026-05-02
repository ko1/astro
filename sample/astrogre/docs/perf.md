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

実利用に近い形。1 ファイル (118 MB の C ソースコーパス) を
`PATTERN file` 形式で開いて行ごとに結果を出す。astrogre interp
(`--plain`) / astrogre AOT-cached (warm `code_store/`) / GNU grep /
ripgrep の 4 を同条件で動かす。Onigmo backend は `WITH_ONIGMO=1`
で再ビルドしたときだけ。

| パターン                                | astrogre interp | astrogre +AOT/cached | grep | ripgrep |
|----------------------------------------|---:|---:|---:|---:|
| `/static/` literal                     | 71 | **40** | 153 | 86 |
| `/specialized_dispatcher/` rare        | **22** | 25 | 37 | **21** |
| `/^static/` anchored                   | 77 | 83 | **71** | 41 |
| `/VALUE/i` case-insens                 | 82 | **76** | 105 | 60 |
| `/static\|extern\|inline/` alt-3       | 389 | **128** | 204 | 64 |
| 12-way alt (AC prefilter)              | 418 | 419 | 281 | **147** |
| `/[0-9]{4,}/` class-rep                | 373 | 177 | **92** | 59 |
| `/[a-z_]+_[a-z]+\(/` ident-call        | 1911 | 1341 | 2421 | **220** |
| `-c /static/` count                    | 27 | 26 | 71 | **30** |

ms、3 回実行の最速。太字は行内の最速。

#### 読み取り

- **AOT が interp を上回るのは prefilter のあるパターン**で、
  `/static/` で 71→40 ms (1.78×)、alt-3 で 389→128 ms (3.04×)。
  両方とも候補位置で動く body chain (cap_start → lit/alt →
  cap_end → succ) が長く、AOT がそれを 1 個の SD 関数に折り畳む。
- **`-c /static/` で astrogre が rg / grep を抑えて勝つ** (26 ms
  vs rg 30 ms vs grep 71 ms)。`node_grep_count_lines_lit` 経由で
  scan + lineskip + count を 1 ループに融合した結果。
- **GNU grep は anchored / class-rep の 2 行で勝つ**。`^` 始まりは
  grep の line-oriented loop と相性が良く、`[0-9]{4,}` は数字
  リテラルがコードで疎なので grep の Boyer-Moore-Horspool が skip
  ahead しやすい。
- **ripgrep は ident-call / 12lit で圧勝**。前者は lazy DFA、
  後者は AC prefilter (我々と同方式) が SIMD で 1.5× 上手い。
  どちらも我々が次フェーズで詰める領域 (todo.md)。

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
**マッチャ単独の速度**を測る。astrogre は `--bench-file` モードで
118 MB バッファを 1 度メモリに置き、エンジンの search 関数を
ループ呼び出しして全マッチを数える。grep / ripgrep は比較のため
`-c` (count モード)。

選んだパターンは「astrogre の prefilter が効かず、chain が長い」
もの。AOT が indirect call を消した効果が一番見える形。

| パターン                                          | astrogre interp | astrogre +AOT | grep | ripgrep |
|--------------------------------------------------|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/`                                | 26 | **14** | 101 | 26 |
| `/(QQQX\|RRRX\|SSSX)+/`                           | 56 | **25** | 30 | 28 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`           | 1078 | 520 | 762 | **207** |
| `/[A-Z]{50,}/`                                    | 481 | 219 | 1737 | **209** |
| `/\b(if\|else\|for\|while\|return)\b/`            | 306 | **107** | 929 | 135 |
| `/[a-z][0-9][a-z][0-9][a-z]/`                    | 1144 | 497 | 1105 | **229** |
| `/(\d+\.\d+\.\d+\.\d+)/`                         | 458 | 223 | **97** | 231 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/`             | 7523 | 5364 | 6170 | **215** |

ms/iter、3 回実行の最速。

#### 読み取り

- **AOT は interp 比 1.4×–2.85× の高速化**を全行で出す。これが
  ASTro framework のコア成果。`(if|else|for|while|return)` の
  alt-of-LIT は 306→107 ms (2.85×) で、grep の 929 ms と
  ripgrep の 135 ms を抑えて行内最速。
- **`(QQQ|RRR)+\d+`、`(QQQX|RRRX|SSSX)+`** で astrogre+AOT が
  GNU grep 以上 + ripgrep と互角。`Q`/`R`/`S` の byteset prefilter
  が刺さって候補位置がほぼゼロ → 残ったコストが指数的に短縮。
- **`[A-Z]{50,}` で grep に 8× 勝ち** (219 ms vs 1737 ms)。grep の
  DFA は連続大文字 50 個以上 (= 100+ 状態) で破裂、astrogre は
  greedy_class fast-path で `[A-Z]+` を SIMD で進める。
- **ripgrep は `(\w+)\s*\(...\)` で 25× 速い** (215 ms vs 5364 ms)。
  rg は AC/Teddy で `(`, `,`, `)` を **任意位置のリテラル anchor**
  として抽出する。astrogre は先頭リテラルしか見ない (我々の AC
  実装は `(cat|dog|...)` 形式専用)。これが現在 rg に対する一番
  大きいギャップ。

#### grep が勝つ唯一の行: `(\d+\.\d+\.\d+\.\d+)` の 97 ms

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

### 静的 inline (`_INL`) リネーム + 弱シンボルラッパー

ASTro の SD は `static inline` として宣言される (gcc が内側の
関数ポインタ呼び出しを完全に inline 化できるように)。ただし
`static` だと dlsym で見えないので、ビルド後にソースを後処理
して `SD_<hash>` を `SD_<hash>_INL` にリネーム + 同名の弱
シンボルラッパー関数を追加する。これで:

- 内部からは inline 化されたまま
- 外部からは dlsym(`SD_<hash>`) で取れる

ノードチェーンの末端まで dlsym 経由で dispatcher を差し替えできる。

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
