# todo.md — aforth 未実装 / 既知ギャップ

[done.md](./done.md) が現状リスト。本書は **未実装 / 不完全** をまとめる。

## 言語機能

### 制御フロー
- `EXIT` — 関数の早期 return。state-based 抜けを node_call まで伝える機構が必要。`leave_flag` を関数スコープに拡張するか、setjmp で実装。
- `?DO` — limit == start なら body をスキップする変種。
- `UNLOOP` — DO/LOOP の枠を破棄して return — 現状 `LEAVE` で代替。

### 値表現 / 型
- 文字列値（`S" string"`、`COUNT`、`TYPE` 等）— 現状 `." ... "` で印字専用のみ。
- Float (`F+ F* F.`) — VALUE が int64 固定なので別スタック実装が要る。
- `R-stack の 2>R / 2R@` — tak で書き換えに使った Tx/Ty/Tz scratch を整理できる。

### 定義系
- `DOES>` — CREATE の振る舞い拡張。
- Immediate words / コンパイル時拡張 (`POSTPONE`, `EVALUATE` 等)。
- `LOCALS|`/`{ ... }` — 名前付きローカル。tak のような 3-引数再帰がだいぶ書きやすくなる。
- 単一 source の REPL — 現状 1 file load → 実行のみ。

### 記憶域
- `C@ C!` — byte-granular access。bitmap-style sieve に効く。
- `,` (comma) — 連続 ALLOT + `!` の砂糖。
- `2VARIABLE` / `2CONSTANT` — 2-cell 版。

### Inspection
- `.S` — 全スタックの非破壊 dump。
- `WORDS` — 定義済み word 一覧。
- スタックアンダーフロー時の診断 (現状 silent overflow → SEGV)。安価な方法はガードページ + DSP 範囲外 fault。

## 性能

[perf.md](./perf.md) も参照。短く:

- **node_call が `@noinline` + table-load**: 各 word 呼び出しが `aforth_word_table[id]` の load + 間接 dispatch。SD で baking できれば call 1 回あたり ~5 cycle 削れる見込み。`@ref body NODE *` の adapter を ASTroGen に追加 (ascheme の `struct *@ref` に倣う) → 各 call site が baked dispatcher を直呼び、self-recursion は cycle break で自動 no-inline。
- **PGC モード (HOPT split)**: koruby と同じ `--pg-compile` で profile 採取 → hot path 専用 SD。pg-1st モードまで含むかは要検討。
- **inline cache for `@ ! +!`**: 現状 var_id → vars[i] でただの array access、cache はないが cold path も不要。VARIABLE 自体が稀なので priority 低。
- **DO/LOOP の always_inline**: ループ body は `@always_inline` 指定済みだが、ネストが深いと register pressure で逆効果になる可能性。castro の経験 (4-7× regression) があるので `@always_inline` を貼った node は計測しながら剥がすかもしれない。

## ツール

- gforth との実機並走 — 現状 `benchmark/run.rb` に runner だけ枠を用意 (gforth 未 install で無視される)。CRuby の bench を ruby 系比較で書くノリで `benchmark/run-vs-gforth.rb` 派生があると有用。
- `--dump-ast` の出力が NODE_DUMP のままなので Forth source っぽくない。逆 lower (`SPECIALIZE`/`DUMP` の Forth 風 textual back-end) があると debug が楽。

## ドキュメント

- `runtime.md` の `EVAL` 流れの図解 (現状 word 紹介のみ)。
- benchmark/run.rb のオプション仕様 (`-r`, `-n`, `--no-warmup`, `-l`) を README に書く。
