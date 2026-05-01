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

### 行イテレーションを AST node に
CLI の whole-file mmap 経路 (`process_buffer` in main.c) でほぼ
カバー済 — prefilter eligible なパターンでは ripgrep 並みの速度。
行イテレーションロジック自体を AST に取り込んで `node_grep_lines(body)`
にすれば、specialiser が行境界探索ループと inline 化された body を
1 つの SD として bake できるが、ベンチでメモリ帯域を支配的にする
には mmap 経路で十分。優先度低。

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
