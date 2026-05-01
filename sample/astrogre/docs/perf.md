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
| `/static/` literal | **28** | **28** | 94 | 2 | 34 |
| `/specialized_dispatcher/` rare | **21** | **21** | 36 | 34 | **21** |
| `/^static/` anchored | 68 | 66 | 95 | **2** | 35 |
| `/VALUE/i` case-i | 587 | 240 | 129 | **2** | 48 |
| `/static\|extern\|inline/` alt-3 | 278 | 81 | 903 | **2** | 49 |
| `/[0-9]{4,}/` class-rep | 458 | 389 | 540 | **2** | 52 |
| `/[a-z_]+_[a-z]+\(/` ident-call | 3183 | 2387 | 2951 | **2** | 178 |
| `-c /static/` count | **23** | **23** | 70 | 2 | 28 |

whole-file mmap 経路 (パターンに SIMD/libc prefilter があるとき発火)
が literal-led astrogre を従来の per-line getline ループより 3-10×
落とす。**literal / rare-literal / `-c` の 3 行で astrogre が
ripgrep を抜く**:

- `/static/` default print 28 ms vs ripgrep 34 ms
- `/specialized_dispatcher/` rare 22 ms vs ripgrep 20 ms (互角)
- `-c /static/` count 24 ms vs ripgrep 27 ms

GNU grep は依然として 1 桁先 (memory-bandwidth-bound memchr)。
`/i`、`class-rep`、`ident-call` の行は per-line streaming に
フォールバック (エンジンが leading `\w` / `[0-9]` / `/i` 形に対する
fast scan を持たないため) — per-line overhead が消えた状態の AOT の
挙動は Bench B を参照。

`/static/` literal と `-c /static/` の 28 ms / 24 ms は同じ仕掛け:
**case-A factorization** で per-line ループを AST に折り畳み、
`node_scan_lit_dual_byte` (Hyperscan 風 dual-byte filter + 64-byte
stride) の body chain として `count → emit → lineskip → continue`
(default print) または `count → lineskip → continue` (`-c`) を
渡す。CLI mode 別に末尾の chain が変わるだけで、scanner と AOT 焼き
は共通。`--verbose` で見える内訳:

```
[verbose] after INIT()            0.12 ms      ← ld.so + dlopen
[verbose] after pattern compile   0.13 ms
[verbose] after mmap             10.04 ms      ← PTE 30k 個セット
[verbose] after scan             18.09 ms      ← SIMD scan (15 GB/s)
[verbose] after munmap           23.53 ms      ← PTE 30k 個破棄
```

scan は 15 GB/s で memory-bandwidth-bound 寄り、残るギャップ ~13 ms
は 118 MB の mmap+munmap PTE 操作。詳しくは
[`done.md`](./done.md#-c-pure_literal-を-ast-ノードに折り込み--node_grep_count_lines_lit) 参照。

### Bench B — engine-level whole-file scan (ms/iter, best-of-3)

`bench/aot_bench.sh`。astrogre は `--bench-file` (バッファをまるごと
メモリに置き、全マッチをカウント); grep / ripgrep / onigmo は `-c`。
パターンは AOT が効きやすい形 (chain が長く、bake の dispatch 削除が
意味を持つ) を選択。★ = astrogre + AOT が grep / Onigmo の両方に勝利。
太字 = 行内ベスト。

| パターン | astrogre interp | astrogre +AOT | astrogre +onigmo | grep | ripgrep |
|---|---:|---:|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | 18 | **12** ★ | 481 | 74 | 23 |
| `/(QQQX\|RRRX\|SSSX)+/` | 40 | **20** ★ | 499 | 25 | 25 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 893 | **433** ★ | 515 | 486 | 176 |
| `/[A-Z]{50,}/` | 737 | **624** ★ | 863 | 1431 | 176 |
| `/\b(if\|else\|for\|while\|return)\b/` | 234 | 76 | 926 | **2** | 118 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 900 | 406 | 516 | **4** | 177 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 537 | 390 | 538 | **4** | 54 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 11944 | 9265 | 12183 | **3** | 189 |

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
| `/(QQQ\|RRR)+\d+/` | 18 | 12 | 1.47× | 481 | 74 | 23 |
| `/(QQQX\|RRRX\|SSSX)+/` | 40 | 20 | 2.04× | 499 | 25 | 25 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 893 | 433 | 2.06× | 515 | 486 | 176 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 900 | 406 | 2.21× | 516 | **4** | 177 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 537 | 390 | 1.38× | 538 | **4** | 54 |
| `/[A-Z]{50,}/` | 737 | 624 | 1.18× | 863 | 1431 | 176 |
| `/\b(if\|else\|for\|while\|return)\b/` | 234 | 76 | 3.07× | 926 | **2** | 118 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 11944 | 9265 | 1.29× | 12183 | **3** | 189 |

(prefilter ladder と同じ条件で、`bench/aot_bench.sh` から再採取。
`-c` カウントを `--bench-file` で再現するので、
[`done.md`](./done.md#他エンジン比較ベンチ) の Bench B 表とほぼ同じ
数字になる — このセクションは AOT/interp 比に焦点を当てた読み方。)

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

`/[A-Z]{50,}/` は全入力をスイープするのに 1.18× にとどまる — chain が
rep_cont ↔ class の 2 ノードしかないので、dispatch がそもそも安い。
逆に chain が長い `/\b(if|else|...)\b/` や `/[a-z][0-9][a-z][0-9][a-z]/`
ではそれぞれ 3.07× / 2.21×。AOT の効きは「per-position の dispatch 数」
にきれいに比例する。

### vs Onigmo

astrogre + AOT は **8/8 全パターンで Onigmo を上回る**:

| パターン | astrogre+AOT | +onigmo | 速度比 |
|---|---:|---:|---:|
| `/(QQQ\|RRR)+\d+/` | 12 ms | 481 ms | **40× 速い** |
| `/(QQQX\|RRRX\|SSSX)+/` | 20 ms | 499 ms | **25× 速い** |
| `/\b(if\|else\|for\|while\|return)\b/` | 76 ms | 926 ms | **12× 速い** |
| `/[A-Z]{50,}/` | 624 ms | 863 ms | 1.38× |
| `/(\d+\.\d+\.\d+\.\d+)/` | 390 ms | 538 ms | 1.38× |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 406 ms | 516 ms | 1.27× |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 433 ms | 515 ms | 1.19× |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 9265 ms | 12183 ms | 1.31× |

特に大きく開くのは alt + literal の prefilter が刺さるケース
(`(QQQ\|RRR)+\d+` 40×、`(QQQX\|RRRX\|SSSX)+` 25×、`\b(if|else|...)\b`
12×) — astrogre は byteset / Truffle / boundary-aware alt scan を
ノード化して bake で即値化、Onigmo は bytecode VM が各分岐で dispatch
するので 1 桁差以上が開く。残りも全部 1.19-1.38× で安定して上回る
(class チェーンの inline 化と capture 状態リセットの SIMD 一括クリア
が効いている)。

### vs grep / ripgrep

ugrep / ripgrep は literal-prefix prefilter / lazy DFA が発火する
パターンで 1 桁先 (`/[a-z][0-9][a-z][0-9][a-z]/` ← grep 4 ms。
leading `[a-z]` が memchr-class スキャンに崩されている)。

しかし prefilter が適用できないパターンでは、ASTro+AOT は **4/8 で
grep を上回る**:

- `/(QQQ\|RRR)+\d+/`: astrogre+AOT 12 ms 対 grep 74 ms (**6.2×**) —
  alt + 共通先頭バイト集合が byteset に落ちる。
- `/[A-Z]{50,}/`: astrogre+AOT 624 ms 対 grep 1431 ms (**2.3×**) —
  grep の DFA は class-rep-50 を簡約できず各位置で線形に試す;
  ASTro は `node_grep_search_class_scan` (Truffle) で先頭スキャン
  + 短絡。
- `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/`: astrogre+AOT 433 ms 対
  grep 486 ms (**1.12×**) — 同じく prefilter 不発、ASTro の inline
  チェーンが単に速い。
- `/(QQQX\|RRRX\|SSSX)+/`: astrogre+AOT 20 ms 対 grep 25 ms
  (**1.25×**) — byteset のおかげ。

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

### Per-line ループも AST に折り込む — case-A scanner + action chain
search ループ折り畳みの自然な拡張: per-line iteration も AST に持って
くる。`node_scan_lit_dual_byte` (= scanner、Hyperscan 風 dual-byte
filter + 64-byte stride) の `body` operand として、CLI mode 別の
小さな action ノード列を渡す:

| CLI mode | body chain |
|---|---|
| `-c LIT` | `count → lineskip → continue` |
| default `LIT FILE` | `count → emit_match_line(opts) → lineskip → continue` |

scanner は候補位置で body を dispatch、body は continuation-passing で
chain を回す。終端は `re_succ` (return 1 = stop, search-style) または
新しい `action_continue` (return 2 = "scanner: c->pos 読んで続行")。
backtracking は body 内部 (rep / alt) で完結し、scanner には影響しない。

性能 (118 MB warm corpus、`/static/`):

| 段階 | wall (ms) |
|---|---:|
| 出発点: `-c` は memmem+memchr in main.c、default は per-line | 64 (`-c`) / 66 (default) |
| stage 1: monolithic `node_grep_count_lines_lit` for `-c` | 23 / 66 |
| stage 2: scanner + action chain (`-c` + default 共通) | **24** / **28** |
| ripgrep ref | 27 / 34 |
| GNU grep ref | 2 / 2 |

stage 2 で **default print の `/static/` が 66 → 28 ms (2.4×)** で
ripgrep 34 ms を抜いた。`-c` の数字は同等を保つ (factorize しても
hot path は変わらず、action chain の dispatch は match 候補時のみ)。
詳細は [`done.md`](./done.md#scanner--action-chain-で-c-と-default-print-を統一) と
[`runtime.md`](./runtime.md#-c-モードの-ast-書き換え) 参照。

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
