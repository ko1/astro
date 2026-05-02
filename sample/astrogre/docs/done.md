# astrogre — 実装済み機能

v1 で入っているもの、カテゴリ別。[`todo.md`](./todo.md) の対。

## エンジン — 言語仕様

### Atom
- リテラル byte 列。隣接リテラルは parse 時に coalesce。
- `.` (dot)。4 variant: ASCII、ASCII-`/m`、UTF-8、UTF-8-`/m`。
- 文字クラス `[...]`、否定 `[^...]`。
- クラス内 ASCII 範囲 (`[a-z]`)。
- クラス内 escape ショートカット: `\d \D \w \W \s \S \h \H`。
- POSIX bracket class `[[:alpha:]]` ほか (`alnum` `alpha` `ascii`
  `blank` `cntrl` `digit` `graph` `lower` `print` `punct` `space`
  `upper` `word` `xdigit`)、否定形 `[[:^alpha:]]`。
- クラス内集合演算: `[a-z&&[^aeiou]]` (`&&` で intersection、左結合、
  ネストした `[...]` は union メンバー)。
- 数値 escape: `\xHH`、`\0`、`\n \t \r \f \v \a \e`。
- Unicode コードポイント escape: `\uHHHH` (4 hex 桁) と `\u{H...}`
  (1–8 hex 桁) — UTF-8 byte 列にエンコードして埋め込む。クラス内では
  ASCII (cp < 0x80) のみ許可、multi-byte は parse error。
- アンカー: `\A` `\z` `\Z` `^` `$` `\b` `\B` `\G` (前回マッチ末尾)。
- 改行種 `\R` (`\r\n` または `[\n\v\f\r]`)。
- `\K` keep-marker — 全体マッチ開始位置を現在の pos に reset。
- リテラル escape: `\\ \/ \. \^ \$ \( \) \[ \] \{ \} \| \* \+ \? \-`。
- インラインコメント `(?#...)` — マッチ `)` まで読み飛ばし、
  `\)` エスケープも対応。

### 量化子
- `*` `+` `?` の greedy / lazy (`*?` `+?` `??`)。
- `{n}` / `{n,}` / `{n,m}` の greedy / lazy。
- Possessive (`*+` `++` `?+`) は atomic group 等価で実装。
  `{m,n}+` は Onigmo に倣い nested rep として扱う(possessive ではない)。

### グループ
- キャプチャ `(...)`。
- 非キャプチャ `(?:...)`。
- 名前付きキャプチャ `(?<name>...)` — name table 経由で `\k<name>` /
  `\g<name>` から参照可。
- インラインフラグ `(?ixm-ixm:...)` / `(?ixm)`。
- 先読み `(?=...)` / 否定先読み `(?!...)`。
- 後読み `(?<=...)` / 否定後読み `(?<!...)`。
  - 固定長: parse 時に幅を計算して逆方向ジャンプ。
  - alt-of-fixed (`(?<=ab|cd)`): 各枝を per-branch 後読みに分割。
  - 真の可変長: 候補幅を長い順にスキャンし `node_re_lb_check` で末尾
    一致を検証。
- Atomic group `(?>...)` — body は自前の succ tail で動き、外側の
  バックトラックが内側の枝に届かない。
- 条件分岐 `(?(N)yes|no)` / `(?(<name>)yes|no)` — capture の
  valid/invalid で yes/no 枝を選ぶ。
- サブルーチン呼び出し `\g<name>` / `\g<N>` — group の lower 結果を
  CTX 経由で indirect dispatch。再帰可。`stack_base`/`stack_limit`
  による動的スタックガード (RLIMIT_STACK の半分、最低 1 MB)。
- 不在 (absence) 演算子 `(?~body)` — body が現在位置でマッチし得ない
  限り 1 byte ずつ greedy に進める。tail 失敗時は 1 byte ずつ縮退。
  `(?:(?!body).)*` 等価の単純実装で、Onigmo の "no contiguous
  substring" semantics とはアンカーなし時に長さが異なる場合あり。

### 後方参照
- `\1`–`\9` 数値、`\k<name>` 名前付き — name table を引いて idx 解決。

### フラグ / エンコーディング
- `/i` (ASCII case-fold)、`/m` (dot は newline にマッチ)、`/x` (extended)、
  `/n` (ASCII byte mode)、`/u` (UTF-8、デフォルト)。

### ReDoS 対策
- Onigmo 互換 MatchCache を移植。`node_re_alt` / `node_re_rep_cont` で
  `(branch_id, pos)` ペアを bit array にメモして既知失敗点を skip。
- メモは lazy alloc — `backtrack_count > str_len × n_branches` に
  なるまで割り当てない (Onigmo の閾値式)。
- 静的 eligibility 判定: backref / atomic / subroutine / conditional /
  capture-inside-lookaround を含む式は memoize 不可と判定して
  `memo_eligible = false`。

### フロントエンド
- `astrogre_parse(pat, len, flags)`: regex 本体 + flag bitmask の主入口。
- `astrogre_parse_literal`: テスト / CLI 用 `/pat/flags` 構文。
- `astrogre_parse_fixed`: `-F` モード、regex parser バイパス。
- prism 依存はドロップ済み (旧 `--via-prism` ・`astrogre_parse_via_prism`
  は撤去)。flag ビット値 (`PR_FLAGS_*`) は prism のレイアウトと数値
  互換だが、prism リンクは不要。

## エンジン — ランタイム

- AST は continuation-passing 形式 — 各 match-node が `next` operand を持ち、
  チェーンは `node_re_succ` で終端。
- 繰り返しは shared `node_re_rep_cont` sentinel + per-call `rep_frame`
  on `c->rep_top`(AST に cycle なし)。
- キャプチャグループ 0 は parser が AST 全体を包むので、全体マッチ範囲は
  ユーザ番号付きグループと同じ機構で記録される。
- `astrogre_search` / `astrogre_search_from`: caller が中断・再開できる
  ヒット enumeration。

## エンジン — 性能改善 (ランディング済み)

- IR レベルで隣接リテラルを coalesce。
- `/i` 下のパターンリテラルは parse 時に lowercase に pre-fold。
- `\A` 始まりパターンの search loop short-circuit (1 位置だけ試す)。
- 単一グローバル `rep_cont` sentinel。
- クラスビットマップは `uint64_t × 4` インライン。
- `node.def` 全体で `CTX *` / `NODE *` に `restrict`。

## ドライバ / ツーリング

- **grep CLI**: `./are/are PATTERN [PATH...]` — 標準オプション
  `-i -n -c -v -w -F -l -L -H -h -o -e -A/-B/-C --color=auto`。
  default で再帰 + `.gitignore` 尊重 + dotfile / binary skip + 並列
  walker。複数パターンは `-e PATTERN` 反復。`-l` / `-L` でファイル名
  のみ出力、`-o` でマッチ span のみ出力。`--no-recursive` / `--hidden`
  / `-a` で grep 互換の挙動に切り替え可能。
- **`--color`**: マッチを赤、行番号を緑、ファイル名を紫(GNU grep 互換)。
  `--color=auto` は `isatty` を見る。
- **`--engine=astrogre|onigmo`**: 実行時バックエンド切り替え。Onigmo
  は `make WITH_ONIGMO=1` で local build (autoconf / libtool 不要の
  自前 `build_local.mk`)。
- **`--encoding=utf-8|ascii`**: regex エンコーディング選択。default は
  UTF-8 (`/u` 相当)、`--encoding=ascii` で `/n` モード。両 backend
  共通で pass-through。
- **engine self-test / microbench**: `make self-test` (118 ケース) /
  `make bench` (in-engine microbench) / `make bench-file` の Makefile
  ターゲット。grep CLI から切り出した `selftest_runner` 専用バイナリで
  動かすので、grep 経路を一切経由せずエンジンの結果を確認できる。
- **`--verbose`**: 主要フェーズ (main entry、INIT、pattern compile、
  AOT compile、exit) の wall-clock を `[verbose] tag    elapsed_ms
  (+delta_ms)` 形式で stderr に出力。`strace` 無しでどのフェーズが
  効いてるか確認できる。`clock_gettime` × 5 回ぶんなので無効時の
  オーバヘッドはゼロ。
- **`bench/grep_bench.rb`** / **`bench/aot_bench.rb`**: 他ツール比較
  ハーネス。grep / ripgrep / are / are +onigmo を同じコーパス +
  パターンで実行、tool ごとの best-of-N を表示。

## バックエンド抽象

- `backend.h`: ops 表 (`compile / search / search_from / free /
  aot_compile / has_fast_scan`) を grep CLI に提供。
- `backend_astrogre.c` (~80 行): in-house エンジンを wrap。
- `backend_onigmo.c` (~110 行): Onigmo の `onig_new` / `onig_search`
  / `onig_region` を wrap。両者とも `-F` をサポート (Onigmo 側は
  compile 時にメタ文字を escape)。

## AOT / cached / PG

- **`--aot-compile` (`-C`)**: 各パターンを `code_store/c/SD_<hash>.c` に
  コンパイル、`code_store/all.so` をビルド、各 node の dispatcher を
  swap、その上で実行。次回以降はデフォルトモードで `OPTIMIZE` 内の
  `astro_cs_load` が自動的に cached `all.so` を拾う。
- **default (cached)**: パターン allocation 時に `OPTIMIZE` が各 node に
  対し `astro_cs_load`。Hit なら specialized dispatcher、miss なら
  interp フォールバック。
- **`--pg-compile` (`-P`)**: abruby との CLI 互換のため受け付け、現状は
  `--aot-compile` と同じ経路を通る (PG profile signal 未実装、
  HOPT == HORG)。
- **`--plain` (`--no-cs`)**: code store を完全 bypass。

内側 SD は `astrogre_export_sd_wrappers` post-process (luastro 由来) で
外部可視化される — 各 SD を `SD_<hash>_INL` にリネームし、薄い
externally-visible wrapper を append、`dlsym` がルートだけでなく全
node を見つけられるように。サイドアレイで全 allocated NODE を track
してビルド後に `astro_cs_load` を全 node に再適用、チェーン全体に
パッチ。

## search ループを AST に折り込み

For-each-start-position search ループそれ自体が node (`node_grep_search`)
になっている — EVAL がループ、specialiser は `body` を通常の NODE *
operand として扱うので、SD はループ + inline 化された regex chain を
1 個の C 関数として bake する。

`/static/` を 16 KiB 文字列に対して回した SD は約 30 命令: ループ、
リテラル比較の `cmpl + cmpw`、capture 状態リセットの `vmovdqu`、
indirect call 一切なし、DISPATCH chain なし。In-engine microbench:
**22.75 s → 3.15 s on `literal-tail` (7.2× 高速化)**。

grep CLI で fusion の効果を見るには、per-line `getline` + `CTX_struct`
ゼロ初期化のオーバヘッドを取り除く必要がある。これを解いたのが下の
whole-file mmap 経路。

## 純リテラルパターンの fast path

パターンが `cap_start(0) → lit → cap_end(0) → succ` の純粋リテラル
形 (regex メタ無し、capture 参照無し) で、かつモードが `-c` /
`-l` / `-L` / default-print のとき、**エンジンを丸ごとバイパス**
して memmem + memchr のタイトループだけで動く。

`astrogre_pattern_pure_literal` がパターンをイントロスペクトして
シェイプを判定、`process_buffer_pure_literal` が memmem+memchr
のタイトループを回す。`-c PURE_LITERAL` の最速経路は別の AST
ノードに格上げした (下の `node_grep_count_lines_lit` 節を参照)。
他のモード (`-l`、`-L`、default-print) では今もこの memmem ループ
を使う。

## scanner + action chain で `-c` と default print を統一

case-A factorization。`node_grep_search` がスタート位置探索ループを
AST に取り込んだのと同じ発想で、per-line iteration も AST に上げる。
**1 つの scanner ノードを `body` operand 経由で複数の per-match action
chain と組み合わせる**:

```
node_scan_lit_dual_byte(needle, body)         ← Hyperscan 風 scanner
   body は per-match action 列:
     ↳ action_count                           ← c->count_result++
     ↳ action_emit_match_line(opts)           ← 行範囲確定 + print
     ↳ action_lineskip                        ← c->pos = 次の \n + 1
     ↳ action_continue                        ← terminator (return 2)
```

CLI mode 別に末尾の chain だけが変わる:

| CLI mode | body chain |
|---|---|
| `-c LIT` | `count → lineskip → continue` |
| default `LIT FILE` | `count → emit_match_line(opts) → lineskip → continue` |
| `-l` (将来) | `print_filename → re_succ` |
| `-o` (将来) | `print_match_span → match_end_advance → continue` |
| 通常 search (regex) | `cap_start → ... → cap_end → re_succ` (既存通り) |

return code を 0/1 → 0/1/2 に拡張:
- 0 = body 失敗 (regex chain がここでマッチしなかった) → scanner は
  次の byte/chunk に進む
- 1 = body 成功 + stop (`re_succ` の意味、既存 search) → scanner は
  即 return
- 2 = body 成功 + continue (新 `action_continue`) → scanner は
  c->pos を読んでそこから再開

backtracking (rep / alt / lookahead) は body 内部の既存 CPS 機構で
完結し、scanner には影響しない。CTX には `fname` / `out` / `lineno`
/ `lineno_pos` を追加して emit action が利用する。

scanner 内部は **Hyperscan 風 dual-byte filter**:
- AVX2 で 64 byte ぶんロード (256-bit × 2 本)、先頭バイトと末尾
  バイトをそれぞれブロードキャスト即値で `vpcmpeqb`、AND した
  マスクを `vptest` で全 0 判定 → 99.5%+ のチャンクは hot path で
  `p += 64` で抜ける
- 候補ヒット時に中間バイトを `memcmp` で verify (needle 長は AOT
  bake で即値、`cmpl + cmpb` チェーンに展開)
- マッチしたら `memchr` で次の `\n` まで飛んで count++

framework が needle を SD ソースに `static const char NEEDLE[] = ...`
として焼き込むので、`_mm256_set1_epi8(needle[0])` は即値、verify の
`memcmp` は固定長で `cmpl/cmpb` に展開される (PCRE2-JIT 風 SIMD-
fused-verify を AOT bake だけで実現)。

性能 (118 MB warm コーパス、`/static/`、160 k matches、出力先は
regular file — `/dev/null` だと grep の dev_null 短絡 (af6af288) で
比較不能になる):

| 段階                                        | `-c` (ms) | default print (ms) |
|---------------------------------------------|---:|---:|
| 出発点 (memmem + memchr in are/main.c)      | 64 | 66 |
| stage 1: monolithic `count_lines_lit`       | 23 | 66 |
| stage 2: scanner + action chain (現在)      | **27** | **71** |
| stage 2 + AOT cached                        | **26** | **40** |
| ripgrep                                     | 30 | 86 |
| GNU grep                                    | 71 | 153 |

`-c /static/` で astrogre interp が **27 ms ─ ripgrep 30 ms / GNU
grep 71 ms** を抑えて行内最速。default print は AOT 込みで 40 ms と
ripgrep 86 ms の半分を切る。GNU grep は両方とも我々より遅い (153 /
71 ms) が、これは grep が `static` のような頻出パターンで毎マッチ
ごと output (`fname:lineno:line` フォーマット + write) を生成する
コストが大きいため。`-c` だけなら grep もタイトな count loop で
済むので 71 ms までは詰まる、それでも我々の AC + scanner
factorization のほうが速い。

`--verbose` で見える分解:

```
[verbose] main entry              0.005 ms
[verbose] after INIT()            0.116 ms  (+0.116)   ← ld.so + dlopen
[verbose] after pattern compile   0.128 ms  (+0.012)   ← 正規表現 parse
[verbose] before mmap             0.132 ms  (+0.004)
[verbose] after mmap             10.041 ms  (+9.900)   ← PTE 30k 個セット
[verbose] after scan             18.090 ms  (+8.050)   ← SIMD scan (15 GB/s)
[verbose] after munmap           23.532 ms  (+5.442)   ← PTE 30k 個破棄
```

scan は 15 GB/s (= シングルコア memory bandwidth ~30 GB/s の半分)
で memory-bandwidth-bound 寄り。残るギャップ ~13 ms は 118 MB の
mmap+POPULATE と munmap で、これは PTE 操作の物理コスト。`read()` 路線
にしても context-copy で同等以上のコストがかかる (実装比較済)。

長い rare needle では grep を上回るケースもある:

| needle (説明) | astrogre `-c` | grep `-c` | 比 |
|---|---:|---:|---:|
| `static` (dense common) | 22 ms | 1 ms | 22× behind |
| `void` | 20 ms | 1 ms | 20× behind |
| `specialized_dispatcher` (22-byte rare) | 19 ms | **33 ms** | **0.6×** ★ |
| `XYZ` (no match) | 19 ms | 3 ms | 6× |

## grep CLI の whole-file mmap 経路

`process_buffer` (are/main.c) は、パターンが SIMD/libc prefilter を持つ
場合 (memchr / memmem / byteset / range / class_scan) 通常ファイルに
対して per-line `getline` を置き換える。ファイルを一度 mmap、バック
エンドの `search_from` をバッファ全体に対しループ、各マッチの所属行
を memrchr/memchr で identify。prefilter 無しのプレーン backtracking
パターンでは per-line streaming のほうが速い (1 行が短いため)。

ベンチ影響: line-by-line grep CLI、118 MB コーパス (mmap + prefilter 後):

| パターン | 旧 interp | mmap interp | grep | ripgrep |
|---|---:|---:|---:|---:|
| `/static/` literal | 285 ms | **77** ms | 2 | 34 |
| literal-rare | 266 | **26** | 35 | 20 |
| `/^static/` | 273 | **76** | 2 | 36 |
| `-c /static/` | 279 | **48** | 2 | 27 |

per-line baseline 比 3-10×。`literal-rare` 行が見出し: 26 ms 対
ripgrep 20 ms、ugrep 35 ms より速い — `node_grep_search_memmem` の
SIMD memmem は ripgrep 並み、mmap path がそれを実際に発火させて
いる。

ゲート: `backend.h` に `has_fast_scan` op を露出、CLI が問い合わせて
mmap path を取るかどうか決定。Onigmo backend は op を NULL のまま
("always yes" — Onigmo は内部 prefilter あり)。`-v` invert モードは
mmap path をスキップ (非マッチ行も列挙する必要があるため)。

## Prefilter ladder — アルゴリズムを node として

5 つのアルゴリズム prefilter node、全て `node_grep_search` の形を
共有: EVAL が SIMD / libc スキャン、body operand が候補位置で
verify する regex chain。Specialiser が body を inline、algorithmic
constants (first byte、needle bytes、範囲境界、nibble テーブル、
packed byte-set) を SD 内の即値として bake。

Parser は最も特化したものを選ぶ:

| node                            | 発火条件                                  | 内部アルゴリズム |
|---------------------------------|-------------------------------------------|---------------|
| `node_grep_search_memmem`       | ≥ 4-byte リテラル prefix、`/i` なし       | glibc memmem (two-way) |
| `node_grep_search_memchr`       | ≥ 1-byte リテラル prefix、`/i` なし       | glibc memchr (AVX2) |
| `node_grep_search_byteset`      | ≤ 8 個の固有 first byte (alt of literal)  | N × `vpcmpeqb` + OR |
| `node_grep_search_range`        | 単一連続範囲の first class                | `vpsubusb / vpminub / vpcmpeqb` |
| `node_grep_search_class_scan`   | 任意 256-bit first class (\w 等)          | Truffle (PSHUFB nibble lookup) |
| `node_grep_search`              | 上記いずれにも該当せず                    | プレーン start-position ループ |

検出ヘルパ (parse.c 内):

- `ire_collect_prefix` — 最長固定リテラル prefix
- `ire_first_class` — zero-width を walk-through した最初の class node
- `bm_is_single_range` — `[a-z]` 風のクラス検出
- `ire_collect_first_byte_set` — alt 各枝の固有先頭 byte を収集
  (>8 で諦め)
- `build_truffle_tables` — 任意 256-bit クラス用 Hyperscan-style
  nibble エンコード

`/i` 下では prefilter 無効化 (case-fold には twin memchr が必要)。
全 SIMD scan は `__AVX2__` ガード、スカラフォールバックあり。

## 他エンジン比較ベンチ

最新結果、118 MB コーパス、full-sweep count (grep `-c` セマンティクス
を `--bench-file` で再現)、best-of-3 ms/iter:

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

★ = astrogre + AOT が grep / Onigmo の両方に勝利。太字 = 行内ベスト。

**この set で grep に 4/8 勝、Onigmo に 8/8 勝** (ugrep 7.5 + PCRE2-JIT
比較)。負けているパターンは全て multi-pattern literal extraction
(Hyperscan Teddy / FDR) を要するもので、これが次の大きな追加項目。

ベンチ詳細は [`perf.md`](./perf.md)、ASTro 設計上の教訓は
[`runtime.md`](./runtime.md) を参照 — bake が各 prefilter node と
均一に合成される様子と、各アルゴリズムが定数オペランドつき特化
C 関数 1 個になる過程が解説されている。
