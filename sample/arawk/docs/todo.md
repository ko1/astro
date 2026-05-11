# arawk TODO

最終目標は `sample/astrogre` (regex / are CLI) との AST traversal interpreter
統合実験。現状は regex 抜きの POSIX awk subset で実用度の高い部分はほぼ動く
が、I/O / 環境変数 / 一部 builtin がまだ抜けている。

## 完了済み

### Phase 0+1 — 基本 awk
- BEGIN / END / pattern-action
- `$N` / `NR` / `NF` (default FS=" ")
- 算術 / 比較 / 論理 / 文字列 concat
- if / while / break / continue / next / exit
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
- `delete a[k]` / `delete a`

### Phase 1.7 — printf & builtins
- `printf` 文 / `sprintf` builtin (`%d %i %u %o %x %X %c %s %f %e %E %g %G`, `*` width/prec)
- `substr(s, pos[, len])`, `index(s, t)`, `split(s, arr[, sep])`
- `tolower`, `toupper`, `int(x)`
- `sin cos sqrt exp log atan2`, `rand`, `srand([seed])`
- `$N = rhs` (field assignment, $0 rebuild via OFS)
- `$N++` / `$N--`
- `a[k]++` / `a[k]--` (post & pre)
- strtod の `inf/nan/infinity` 認識を回避 (`informed` → 0)

### Phase 1.8 — user-defined functions
- `function name(params) { body return v }` (`func` も alias)
- callee 用 `F[ARAWK_FRAME_MAX=64]` VLA frame
- params + extra locals 同じ slot 空間 (POSIX 慣行)
- 名前解決: 関数内 local → global fallback
- `arawk_node_gget / gset / aget_g / aset_g / postinc_g / for_in_g / delete_g` 群
- 再帰 OK / forward call OK
- 関数 body は OPTIMIZE() で AOT SD ロード対象
- 副産物 fix: return なし関数は UNINIT を返す (前は 0 を返してた)

### Phase 1.9 — pipe / output redirect (print 側)
- `print ... | "cmd"` (popen, cached per cmd)
- `print ... > "file"` (fopen overwrite)
- `print ... >> "file"` (fopen append)
- `awk_close_all_streams()` を main 末尾で実行 → sort 系が EOF 受け取って出力

---

## 残タスク (regex 抜き POSIX 完了に必要)

### Phase 1.10 — printf 系の redirect (低工数)

`print x | "cmd"` は動くが `printf "..." | "cmd"` は未対応。同じパターンで実装可能。

- [ ] `arawk_node_printf_to(fmt, base, cnt, dest, mode)` 追加 (print_to と同型)
- [ ] parser: `printf ... | "cmd"` / `> "file"` / `>> "file"`

### Phase 1.11 — I/O 関連 builtin (中工数)

- [ ] `close(file_or_cmd)` — 既存の output stream cache から取り除いて fclose/pclose
- [ ] `fflush()` / `fflush(file_or_cmd)` — 全部 or 個別
- [ ] `system("cmd")` — fork/exec/wait; 終了ステータスを返す

これがあると pipeline 中の order 制御 (`print > "f"; close("f"); ...`) や `system` 経由のスクリプト連携が可能。

### Phase 1.12 — getline (中〜大工数)

POSIX awk で結構使われる。6 形態あり:

| 構文 | 動作 | $0 | NR | NF | FNR | FILENAME |
|---|---|---|---|---|---|---|
| `getline` | 現入力進める | 更新 | 進 | 更新 | 進 | 更新 |
| `getline var` | 現入力 → var | 不変 | 進 | 不変 | 進 | 更新 |
| `getline < "file"` | file → $0 | 更新 | 不変 | 更新 | 不変 | 不変 |
| `getline var < "file"` | file → var | 不変 | 不変 | 不変 | 不変 | 不変 |
| `"cmd" \| getline` | cmd 出力 → $0 | 更新 | 不変 | 更新 | 不変 | 不変 |
| `"cmd" \| getline var` | cmd 出力 → var | 不変 | 不変 | 不変 | 不変 | 不変 |

- [ ] node: `arawk_node_getline_0`, `_var`, `_file_0`, `_file_var`, `_cmd_0`, `_cmd_var`
- [ ] runtime: 入力 stream cache (output cache と対称)。`awk_open_input(mode, dest)`
- [ ] parser: precedence は低い (concat より低、relop より高い扱いが gawk 流)
- [ ] EOF / エラー時の戻り値: 1 / 0 / -1

### Phase 1.13 — 環境配列 (低工数)

- [ ] `ENVIRON["VAR"]` — main 起動時に environ を読んで連想配列に詰める
- [ ] `ARGC` / `ARGV[0..ARGC-1]` — `arawk -f script file1 file2` の `file*` を ARGV に
  - awk 慣行: `ARGV[0] = "awk"`, `ARGV[1..] = files`
  - ARGV[i] が `var=value` 形式なら入力扱いせず代入
  - 実装には main.c の input loop と ARGV をリンクする必要あり

### Phase 1.14 — その他 POSIX 必須 builtin

- [ ] `system(cmd)` — 上で出した
- [ ] `printf` のロケール対応 (gawk 互換) — おそらく skip

### Phase 1.15 — 特殊変数の補完

- [ ] `RSTART` / `RLENGTH` — `match()` の副産物 (Phase 2 の regex 統合と一緒)
- [ ] `CONVFMT` (numeric→string conversion) を env から読む (現状 `"%.6g"` ハードコード)
- [ ] `OFMT` を `print` 系から読む
- [ ] `OFS` / `ORS` を `arawk_node_print` から読む (現状 `" "` / `"\n"` ハードコード)
- [ ] FS 変更時の $0 再 split
- [ ] NF への代入で field を切り詰める / 0 埋めする gawk 互換動作

### Phase 1.16 — エッジケース / 仕様詳細

- [ ] uninit の context-aware coercion (numeric vs string) の細部精査
- [ ] 配列を関数パラメータに渡したときの auto-vivification 共有 (現状: 既存配列なら参照渡し OK、UNINIT を渡すと callee 内の vivify は caller に伝わらない)
- [ ] `delete a` (全削除) の正式 POSIX 化 (POSIX 2017 で標準)
- [ ] 多次元配列の `delete a[i, j]`
- [ ] `\` 行継続 / `\n` 等のエスケープシーケンスの境界

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
- [ ] `node.def` を 2 つにマージする方針確定 (arawk 側は `arawk_node_*` prefix 済)
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

## perf 改善案 (docs/perf.md からの抜粋)

- [ ] **`print` の chunked write** (tt.01 が 0.17×) — 1 文 1 fwrite に集約
- [ ] **field の lazy strnum** (tt.03 系が 0.19-0.23×) — `$N` アクセス時のみ allocate
- [ ] **`substr` の copy-on-write** (tt.11 が 0.27×) — shared heap ptr で fresh allocate 回避
- [ ] **function callcache** (tt.14 が 0.45×) — astr の `astr_callcache` 相当を call site に inline
- [ ] **AOT 内 builtin inline** — `length` / `substr` / `int` 等の hot builtin を SD body に直接埋め込む (現状は PLT call)

---

## バックログ / nice-to-have

- [ ] benchmark report を継続的に取って perf.md に履歴を残す
- [ ] gawk の `testdir/*` (POSIX conformance suite, ~200 ケース) を submodule で取り込み
- [ ] fuzz テスト (random awk プログラム生成 → gawk と diff)
- [ ] indirect function call (`@f()`; gawk 拡張、優先度低)
- [ ] gawk-specific extensions (gensub, asort, etc.) — POSIX 完了後に検討

---

## 既知の制限事項 / 落とし穴 (memory にも記録)

- ASTroGen の `parse_def_head` は `name(params)` を 1 行で要求 (改行不可)
- ASTro framework は NULL NODE* を許さない → Null object pattern (`arawk_node_noop`) で対応
- NF は record read 直後に決まる必要あり (lazy split は NF=0 を返す) → eager split で対応
- strtod は C99 で `inf` / `infinity` / `nan` 認識 → awk 仕様と乖離。先頭が digit/sign-digit/`.digit` でなければ 0 を返すよう特別処理
- bash heredoc が `!` を escape → Write tool 使用

## テストとベンチ

- `make test` — smoke 74 × {plain, AOT} = 148 + tt.* 18 × {plain, AOT} = 36 ケース。所要 ~12s
- `make bench` — gawk / mawk / goawk と比較。geomean arawk-aot 0.59× vs gawk。詳細は `docs/perf.md`
