# astrogre — 実装済み機能

v1 で入っているもの、カテゴリ別。[`todo.md`](./todo.md) の対。

## エンジン — 言語仕様

### Atom
- リテラル byte 列。隣接リテラルは parse 時に coalesce。
- `.` (dot)。4 variant: ASCII、ASCII-`/m`、UTF-8、UTF-8-`/m`。
- 文字クラス `[...]`、否定 `[^...]`。
- クラス内 ASCII 範囲 (`[a-z]`)。
- クラス内 escape ショートカット: `\d \D \w \W \s \S`。
- 数値 escape: `\xHH`、`\0`、`\n \t \r \f \v \a \e`。
- アンカー: `\A` `\z` `\Z` `^` `$` `\b` `\B`。
- リテラル escape: `\\ \/ \. \^ \$ \( \) \[ \] \{ \} \| \* \+ \? \-`。

### 量化子
- `*` `+` `?` の greedy / lazy (`*?` `+?` `??`)。
- `{n}` / `{n,}` / `{n,m}` の greedy / lazy。
- Possessive (`*+` `++` `?+`) は parse のみ — greedy に degrade。

### グループ
- キャプチャ `(...)`。
- 非キャプチャ `(?:...)`。
- 名前付きキャプチャ `(?<name>...)` — 左右順 index で。
- インラインフラグ `(?ixm-ixm:...)` / `(?ixm)`。
- 先読み `(?=...)` / 否定先読み `(?!...)`。

### 後方参照
- `\1`–`\9`。

### フラグ / エンコーディング
- `/i` (ASCII case-fold)、`/m` (dot は newline にマッチ)、`/x` (extended)、
  `/n` (ASCII byte mode)、`/u` (UTF-8、デフォルト)。

### フロントエンド
- prism 統合 (`astrogre_parse_via_prism`): 任意の Ruby ソース AST を
  歩いて、最初の `PM_REGULAR_EXPRESSION_NODE` を拾う。
- `astrogre_parse_literal`: テスト / CLI 用 `/pat/flags` 構文。
- `astrogre_parse_fixed`: `-F` モード、regex parser バイパス。

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

- **grep CLI**: `./astrogre PATTERN [FILE...]` — 標準オプション
  `-i -n -c -v -w -F -l -L -H -h -o -r -e --color=auto`。
  `-r` でディレクトリ再帰、ドットファイルはスキップ。複数パターンは
  `-e PATTERN` 反復。`-l` / `-L` でファイル名のみ出力、`-o` でマッチ
  span のみ出力。
- **`--color`**: マッチを赤、行番号を緑、ファイル名を紫(GNU grep 互換)。
  `--color=auto` は `isatty` を見る。
- **`--via-prism`**: パターン引数を Ruby ソースとして prism で parse、
  最初の `/.../` の本体を検索パターンとして使う。
- **`--backend=astrogre|onigmo`**: 実行時バックエンド切り替え。Onigmo
  は `make WITH_ONIGMO=1` で local build (autoconf / libtool 不要の
  自前 `build_local.mk`)。
- **`--self-test`** (44 ケース)、**`--bench`** (in-engine microbench) を
  flag として保持。
- **`bench/grep_bench.sh`** / **`bench/aot_bench.sh`**: 他ツール比較
  ハーネス。grep / ripgrep / astrogre / astrogre+onigmo を同じコーパス
  + パターンで実行、tool ごとの best-of-N を表示。

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
シェイプを判定、`process_buffer_pure_literal` が以下のループを
回す:

```c
while (p < end) {
    q = memmem(p, end-p, needle, nlen);
    if (!q) break;
    matches_this_file++;
    e = memchr(q, '\n', end-q);
    if (!e) break;
    p = e + 1;  // 次の行へ
}
```

エンジン呼び出し (CTX init + dispatch chain) を完全に省略するので、
`-c /lit/` で 50 → 47 ms (理論限界の bare memmem ループ 26 ms に
対し +20 ms はプロセス起動 + mmap セットアップ + その他 CLI 処理)。

## grep CLI の whole-file mmap 経路

`process_buffer` (main.c) は、パターンが SIMD/libc prefilter を持つ
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
| `/(QQQ\|RRR)+\d+/` | 19 | **12** ★ | 488 | 74 | 23 |
| `/(QQQX\|RRRX\|SSSX)+/` | 40 | **23** ★ | 535 | 27 | 25 |
| `/[a-z]\d[A-Z]\d[a-z]\d[A-Z]\d[a-z]/` | 926 | **444** ★ | 548 | 507 | 185 |
| `/[A-Z]{50,}/` | 741 | **640** ★ | 919 | 1525 | 185 |
| `/\b(if\|else\|for\|while\|return)\b/` | 252 | 90 | 894 | **2.5** | 118 |
| `/[a-z][0-9][a-z][0-9][a-z]/` | 1008 | 429 | 535 | **4** | 186 |
| `/(\d+\.\d+\.\d+\.\d+)/` | 566 | 397 | 554 | **4** | 48 |
| `/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/` | 13061 | 10096 | 14532 | **5** | 351 |

★ = astrogre + AOT が grep / Onigmo の両方に勝利。太字 = 行内ベスト。

**この set で grep に 4/8 勝、Onigmo に 8/8 勝** (ugrep 7.5 + PCRE2-JIT
比較)。負けているパターンは全て multi-pattern literal extraction
(Hyperscan Teddy / FDR) を要するもので、これが次の大きな追加項目。

ベンチ詳細は [`perf.md`](./perf.md)、ASTro 設計上の教訓は
[`runtime.md`](./runtime.md) を参照 — bake が各 prefilter node と
均一に合成される様子と、各アルゴリズムが定数オペランドつき特化
C 関数 1 個になる過程が解説されている。
