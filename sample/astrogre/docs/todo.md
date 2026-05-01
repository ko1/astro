# astrogre — TODO

未実装機能 / 未完了の性能改善のバックログ。[`done.md`](./done.md) の対。
おおむねインパクト / 実装コスト順。

## 性能 — 次の一手

### マルチパターン Hyperscan-Teddy literal anchor scan
ベンチで ugrep に対する一番大きいギャップ。`/(\w+)\s*\(\s*(\w+)\s*,\s*(\w+)\)/`
のようなパターンは内部に必須リテラル (`(`、`,`、`)`) を持つ — ugrep は
これらを抽出してマルチパターン Teddy / FDR スキャンを走らせ、ヒット
した位置で本格 verify する。astrogre は今、パターンの「先頭」しか
見ていない。

計画:
- IR を歩いて任意位置の「必ず現れる」リテラル byte 列を、マッチ開始
  からの最小距離付きで集める。
- 最も rare(または最長)なものを anchor として選ぶ。
- 新ノード `node_grep_search_teddy(body, anchors, max_back)` — AVX2
  マルチパターンスキャン。各 anchor ヒット位置から
  `[hit - max_back, hit]` の窓で body を試す。

これで `(\w+)\s*\(...\)` と `(\d+\.\d+\.\d+\.\d+)` の grep 差を
詰められる。実装コスト: ~500-1000 行 (parser 抽出 + AVX2 マルチ
パターンスキャナ + バックアップロジック)。

### scanner + per-match action factorization (案 A) ── 段階 1+2 完了
`-c PURE_LITERAL` と default print の per-line ループを `node_scan_lit_dual_byte`
+ action chain に factorize 済 ([done.md](./done.md) の case-A セクション)。
`/static/` default print が 66 → 28 ms (2.4×) で ripgrep 34 ms を抜いた。
`-c` は 23 → 24 ms で perf 同等を維持。

残作業:

- **`-l` / `-L`**: action_emit_filename + 適切な terminator
  (`re_succ` で stop / 最後にまとめて出力)
- **`-o`**: action_emit_match + action_match_end_advance (重複なし
  複数マッチ列挙)
- **`-i` (case-fold)**: scanner 側に case-fold variant を入れる、
  もしくは IR 側で needle のケース全パターンを byteset 化
- **`-v` (invert)**: 「マッチ**しない**行」を出すモード。scanner では
  expressible しない (line-aware ではないため)。要別経路
- **anchored (`^pat`)**: 現在 fast path にフォールバックしない
  (anchored_bos = true で pure_literal が false 返す)。
  anchored 専用の per-line scanner variant を入れる
- **既存 `node_grep_search_*` を `node_scan_*` に揃える**: protocol
  統一で scanner / body の組み合わせ自由度を上げる

設計決定 (実装済):

- **scanner**: `node_scan_lit_dual_byte / _memmem / _memchr / _byteset /
  _class_range / _class_truffle / _plain` — 既存 `node_grep_search_*`
  を body protocol で一般化したもの。引数は body + アルゴリズム定数。
- **action ノード** (新規、いずれも `next` 持ち continuation):
  `action_count` / `action_emit_match_line(opts)` / `action_emit_match`
  / `action_emit_filename` / `action_lineskip` /
  `action_match_end_advance` / `action_first_only` / `action_continue`
  (terminator).
- **return code 拡張** (2 値 → 3 値):
  - 0 = body 失敗 (regex chain がここでマッチしなかった)
  - 1 = body 成功 + stop (= 既存 search セマンティクス、`re_succ`)
  - 2 = body 成功 + continue (= action chain 完了、`action_continue`)
- **scanner の主ループ**:
  ```c
  VALUE r = EVAL_ARG(c, body);
  if (r == 1) return 1;
  if (r == 2) p = c->pos;       /* action chain advanced pos */
  else        p = q + 1;        /* body failed at this candidate */
  ```
- **CLI が AST 末尾を差し替えで mode を表現**:
  - search: `... → re_succ`
  - `-c`: `... → action_count → action_lineskip → action_continue`
  - default print: `... → action_emit_match_line(opts) → action_lineskip → action_continue`
  - `-l`: `... → action_emit_filename → re_succ`
  - `-o`: `... → action_emit_match → action_match_end_advance → action_continue`

backtracking は body 内部 (rep / alt / lookahead) で完結し、scanner には
影響しない。CTX に `fname` / `lineno` / `lineno_pos` / `out` を追加して
action ノードが利用する。

実装は段階移行:
1. 3 値 protocol + `action_count` / `action_lineskip` / `action_continue`
   追加 → count_lines_lit を `scan_lit_dual_byte(count → lineskip →
   continue)` に書き換え (perf 同等を確認)
2. `action_emit_match_line(opts)` 追加 → default print を新形に切替
   (期待 ~15-20 ms、現 64-66 ms から大幅減)
3. 残り `node_grep_search_*` を `node_scan_*` に揃え、`-l` / `-o` を
   action 列で表現

`/static/` (default print) の予測 ~15-20 ms で、現 64 ms から ripgrep
33 ms を抜けるかどうかというライン。期待値は高い。

### 本物の PG signal
`--pg-compile` は配線済みだが現状は `--aot-compile` の alias
(`HOPT == HORG` のため)。本物の signal:
- **Hot-alternative reordering**: 各 `node_re_alt` の枝ヒット数を計測し、
  hot な枝を最初に試すよう alternation を再構成。
- **Capture elision**: profile run で参照されないキャプチャグループは
  save/restore を省略。
- **反復回数特化**: 観測された反復回数が集中する場合の固定 N 展開
  variant を bake。

### `/i` ケースフォールド用 twin-memchr scan
`/foo/i` — 'f' / 'F' どちらも match 開始候補。選択肢:
- 入力 chunk ごとに 2 回 memchr → min 位置をピック。
- もしくは 2-byte byteset エントリ: {'f', 'F'} を pre-compute して
  既存の `node_grep_search_byteset` を流用。
2番目はほぼ trivial — `ire_collect_first_byte_set` を `/i` リテラル
で両ケース push するよう拡張するだけ。

### `node_grep_search_bmh` for `-F` モード
glibc memmem は two-way アルゴリズム。Boyer-Moore-Horspool に bake-time
bad-character テーブル付きの方が短い針では速いことが多い。

### 小さい char-class の比較展開
`[abc]` のような場合、`b == 'a' || b == 'b' || b == 'c'` の方が
gcc が switch table / SIMD compare に畳むので bitmap test より
速いケースあり。Specialiser candidate。

### キャプチャ状態を CTX 外、スタック上に
`c->starts[]` / `c->ends[]` / `c->valid[]` 各 32 entry × 8 byte で
~750 byte/CTX。実用パターンはほぼ ≤ 4 group。

### read 経路でラージファイルの mmap 13 ms を回避
GNU grep のように `read()` を 32–96 KB chunk で回せば 118 MB ファイル
の mmap+munmap 13 ms がそっくり消える。一度試して 75 ms に逆悪化した
時点で諦めたが、原因 (chunk size か per-chunk ノード dispatch
overhead) は未追求。きちんと詰めれば `-c /static/` を 23 → 12 ms 程度
まで短縮できる見込み (grep 2 ms との差はほぼ dynamic linker と libc
init になる)。

### file-backed THP / hugetlb
`madvise(MADV_HUGEPAGE)` を入れて 30 k 個の 4 KB PTE を ~60 個の 2 MB
PTE に減らせれば mmap+munmap が劇的に速くなる。試した時点では
test kernel が file-backed THP 無効 (`shmem_enabled = [never]`) で
逆効果だった。CONFIG_READ_ONLY_THP_FOR_FS が有効な環境で再評価。

### Code-store mtime 無効化
`node.def` を編集すると cached `SD_*.c` は古くなるが、`astro_cs_init`
の version 引数は今 `0`。バイナリの mtime を渡せば再コンパイルで強制
リビルドされる。

### JIT
Teddy / line-iteration nodes が landing したあと、標準の ASTro JIT
経路にプラグイン: AST が DAG なので code-store sharing が応用できる。

## 言語仕様の不足

### 後読み (lookbehind)
- `(?<=...)` / `(?<!...)`。Parser は明示的にエラーを返す。
- 固定長: parse 時に body の長さを計算して逆方向にジャンプ。
- 可変長: 逆方向の continuation-passing が必要。

### Atomic group / possessive 量化子
- `(?>...)` および `*+`、`++`、`?+`。
- Possessive は parse のみ — greedy に degrade。
- rep_frame プロトコルに「commit barrier」が必要。

### `\k<name>` の name-table 参照
名前付きキャプチャは認識されているが index でしか保存されていない。
`\k<name>` は今 group 1 を無条件にマッチしてしまう。

### 条件付きグループ / 再帰
- `(?(cond)yes|no)`。
- `\g<name>` / `\g<n>` (再帰サブルーチン)。
- `(?#...)` コメント。

### Unicode
- `\p{...}` / `\P{...}` プロパティクラス。
- `/i` の Unicode case fold (今は ASCII のみ)。
- `\X` extended grapheme cluster。

### クラス内マルチバイト文字
`[äé]` は今、ASCII bitmap に高位バイトを byte-by-byte で書き込んで
いるだけで、コードポイント単位のマッチにはならない。ハイブリッド
クラス (ASCII bitmap + ソート済みコードポイント範囲リスト) が必要。

### エンコーディング
EUC-JP (`/e`)、Windows-31J (`/s`)。需要次第で。

### アンカー / 境界
- `\G` (前回マッチ末尾アンカー)。
- `\R` (改行種)。

### `Regexp.new(str)` / `Regexp.compile(str)`
`--via-prism` 経路は prism がリテラル `/.../` として認識する正規表現
しか拾わない。ランタイム文字列 + フラグ引数を取る別経路があれば、
astrogre をホストプログラムから regex *ライブラリ* として使える。

## API / ドライバ

### grep CLI の GNU grep 比不足
- `-A` / `-B` / `-C` 文脈行。
- `-Z` / `-z` NUL 区切り出力 / 入力。
- `--include` / `--exclude` glob (再帰時)。
- `-q` quiet (exit code のみ)。
- バイナリファイル検出。
- `--mmap` ラージファイル用 (今は `getline` ベース; 通常ファイルは
  すでに mmap を使っているが、CLI flag による明示は無し)。

### エンジン API
- 全グループを行・列番号付きで露出する `MatchData` 相当の構造体。
- `gsub` 相当: 入力を歩いて、繰り返しマッチ、置換を splice。
- `scan` 相当: 非重複マッチを callback で列挙。

### 診断
- prism のソース span 由来の line / column 付き parse エラー報告。
- Trace モード (dispatch + 位置を毎回出力)。
- PCRE / Onigmo / re2-tests コーパスを通して挙動ギャップを系統的に
  洗い出す。

## テスト

- マルチバイト char-class 実装後の UTF-8 カバレッジ拡張。
- マッチ位置の drift をキャッチするための grep CLI 出力 vs GNU grep
  クロスチェック (パターン × コーパスを多数)。
- 病的バックトラッキング (`/(a+)+b/`) — 今は指数爆発する; 既知の
  制約。
