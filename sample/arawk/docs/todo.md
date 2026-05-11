# arawk TODO

最終目標は `sample/astrogre` (regex / are CLI) との AST traversal interpreter
統合実験。**Phase 1 (regex 抜き POSIX awk subset) 完了**。次は Phase 2 で
astrogre を統合する。

## 完了済み

### Phase 0+1 — 基本 awk
- BEGIN / END / pattern-action / `{ ... }`
- `$N` / `NR` / `NF` (default FS=" ")、`NR`/`NF` は普通の global 扱い (代入も OK)
- 算術 / 比較 / 論理 / 文字列 concat
- if / while / do-while / break / continue / next / nextfile / exit
- グローバル変数 (parse-time slot)
- AOT bake (`-c`) + Code Store reload

### Phase 1.5 — 制御フロー & 算術拡張
- `for (init; cond; step) body`
- `++` / `--` (pre/post on local & global scalar + `$N`)
- `+=` `-=` `*=` `/=` `%=` `^=` (desugar)
- `length` / `length()` / `length(expr)`

### Phase 1.6 — 配列 & 三項 & in
- 連想配列 `a[k]` (lvalue / rvalue, 多次元 `a[i,j]` → SUBSEP join)
- `?:` 三項
- `k in arr` 演算子
- `for (k in arr) body`
- `delete a[k]` / `delete a` / 多次元 `delete a[i,j]`

### Phase 1.7 — printf & builtins
- `printf` 文 / `sprintf` builtin (`%d %i %u %o %x %X %c %s %f %e %E %g %G`, `*` width/prec)
- `substr(s, pos[, len])`, `index(s, t)`, `split(s, arr[, sep])`
- `tolower`, `toupper`, `int(x)`
- `sin cos sqrt exp log atan2`, `rand`, `srand([seed])`
- `$N = rhs` (field assignment、$0 rebuild via OFS、欠 field は "" 埋め)
- `$N++` / `$N--`
- `a[k]++` / `a[k]--` (post & pre)
- strtod の `inf/nan/infinity` 誤認識を回避

### Phase 1.8 — user-defined functions
- `function name(params) { body return v }` (`func` alias 受け付け)
- callee 用 `F[ARAWK_FRAME_MAX=64]` VLA frame
- params + extra locals 同じ slot 空間 (POSIX 慣行)
- 名前解決: 関数内 local → global fallback
- 再帰 OK / forward call OK
- 関数 body は OPTIMIZE() で AOT SD ロード対象
- return なし関数は UNINIT を返す

### Phase 1.9 — pipe / output redirect (print 側)
- `print ... | "cmd"` (popen, cached per cmd)
- `print ... > "file"` / `>>` (fopen overwrite / append)
- `awk_close_all_streams()` を main 末尾で実行

### Phase 1.10 — printf 系の redirect
- `printf "..." | "cmd"`
- `printf "..." > "file"` / `>> "file"`

### Phase 1.11 — I/O 関連 builtin
- `close(file_or_cmd)` (output / input 両方の stream cache に対応)
- `fflush()` / `fflush(name)` / `fflush("stdout")` / `fflush("stderr")`
- `system("cmd")` — stdout flush してから system(3)、戻り値は wait status

### Phase 1.12 — getline (6 形態 全部)
- `getline` — 現入力 → $0、NR/FNR/NF 更新
- `getline NAME` — 現入力 → NAME、NR/FNR 更新
- `getline < expr` — file → $0、NR は不変
- `getline NAME < expr` — file → NAME、副作用なし
- `expr | getline` — cmd 出力 → $0
- `expr | getline NAME` — cmd 出力 → NAME
- 戻り値 1 (read) / 0 (EOF) / -1 (error)
- input stream cache (popen/fopen) を runtime に追加

### Phase 1.13 — ENVIRON / ARGC / ARGV
- `ENVIRON["VAR"]` — main 起動時に `environ` を連想配列に詰める
- `ARGV[0]` = "arawk"、`ARGV[1..]` = 入力ファイル列、`ARGC` = 個数 + 1

### Phase 1.15 — 特殊変数の補完
- **OFS / ORS** を `print` / `print_to` 各ノードが env から読む (変数代入即反映)
- **CONVFMT** を `awk_to_cstr` の float 変換で読む (CTX_CURRENT 経由)
- **OFMT** slot は確保 (現状 CONVFMT と統合扱い)
- **RSTART / RLENGTH** slot は確保 (Phase 2 で `match()` 実装時に使用)
- **FS** 代入で現レコードの fields_split を invalidate → 次の $N で再 split
- **NF =** 代入で field を `""` 埋め / 切り詰め、$0 を OFS で再構築

### Phase 1.16 — エッジケース対応分
- 多次元 `delete a[i,j]` (key を SUBSEP join するだけで naturally 動く)
- NF 代入で `$0` が OFS で再構築される

---

## Phase 2 — astrogre 統合 (本命)

### Level 1: library として astrogre を呼ぶ
- [ ] `/regex/` literal トークン化 (slash-vs-division 曖昧性解消)
- [ ] `~` / `!~` 演算子
- [ ] `sub(re, repl[, target])` / `gsub(re, repl[, target])` builtin
- [ ] `match(s, re)` (RSTART / RLENGTH set)
- [ ] `split(s, arr, re)` 第 3 引数 regex
- [ ] dynamic regex (`$0 ~ pattern_var`)
- [ ] FS / RS が regex のとき
- [ ] sample/astrogre を Makefile で別ターゲットからリンク

### Level 2: 2 AST traversal interpreter 並存
- [ ] astrogre 側も `agre_node_*` prefix にする (要 astrogre 改修)
- [ ] CTX 統合 (awk_record + agre rep_stack)
- [ ] code_store のディレクトリ分離 or 統合

### Level 3: 単一 interpreter で awk + regex を実行 (本命)
- [ ] VALUE 統一 (awk LSB-tagged ↔ agre int64 MR_*)
  - awk の AWK_UNINIT/STRNUM は LSB=0 の heap ptr、 agre MR_* は小整数 0-2 — 値領域は衝突しないので統合可能
- [ ] RESULT 状態空間統合 (NEXT/EXIT/RETURN ↔ MR_FAIL/STOP/CONTINUE)
- [ ] dispatcher テーブル統合
- [ ] SD bake が両 AST にまたがる挙動を検証

---

## perf 改善案 (docs/perf.md 連動)

- [ ] **`print` の chunked write** (tt.01 が 0.17×) — 1 文 1 fwrite に集約
- [ ] **field の lazy strnum** (tt.03 系が 0.19-0.23×) — `$N` アクセス時のみ allocate
- [ ] **`substr` の copy-on-write** (tt.11 が 0.27×) — shared heap ptr で fresh allocate 回避
- [ ] **function callcache** (tt.14 が 0.45×) — astr の `astr_callcache` 相当を call site に inline
- [ ] **AOT 内 builtin inline** — `length` / `substr` / `int` 等の hot builtin を SD body に直接埋め込む

---

## バックログ / nice-to-have

- [ ] gawk extensions: `gensub` / `asort` / `asorti` / `mktime` / `strftime` / `systime`
- [ ] indirect function call (`@f()`; gawk 拡張)
- [ ] `(i, j) in a` syntax (gawk extension; 多次元キーの subscript-in)
- [ ] ARGV 駆動の入力ループ (現状 ARGV は read-only で、入力は OPTION.input_files から開く)
- [ ] benchmark report の継続的計測 → perf.md に履歴
- [ ] gawk の `testdir/*` (POSIX conformance suite, ~200 ケース) を submodule で取り込み
- [ ] fuzz テスト (random awk プログラム生成 → gawk と diff)

---

## 既知の制限事項 / 落とし穴 (memory にも記録)

- ASTroGen の `parse_def_head` は `name(params)` を 1 行で要求 (改行不可)
- ASTro framework は NULL NODE* を許さない → Null object pattern (`arawk_node_noop`) で対応
- NF は record read 直後に決まる必要あり → eager split で対応
- strtod は C99 で `inf` / `infinity` / `nan` 認識 → awk 仕様と乖離。先頭が digit/sign-digit/`.digit` でなければ 0 を返すよう特別処理
- bash heredoc が `!` を escape → Write tool 使用
- 関数の array 引数 + auto-vivify は callee 内に閉じる (gawk と微妙に異なる; pre-vivified なら参照渡し OK)

## テストとベンチ

- `make test`:
  - smoke 98 × {plain, AOT} = 196 ケース
  - tt.* 18 × {plain, AOT} = 36 ケース (regex 系 6 skip)
- `make bench`: gawk / mawk / goawk と比較。geomean arawk-aot 0.59× vs gawk。詳細は `docs/perf.md`
