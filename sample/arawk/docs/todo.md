# arawk TODO

最終目標は `sample/astrogre` (regex / are CLI) との AST traversal interpreter
統合実験。現状 regex 抜きで POSIX awk subset がほぼ動く状態。

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
- `+=` `-=` `*=` `/=` `%=` `^=` (desugar に置換)
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
- `arawk_node_gget / gset / aget_g / aset_g / postinc_g / for_in_g / delete_g` 群 (global access)
- 再帰 OK / forward call OK
- 関数 body は OPTIMIZE() で AOT SD ロード対象

### Phase 1.9 — pipe / output redirect
- `print ... | "cmd"` (popen, cached per cmd)
- `print ... > "file"` (fopen overwrite)
- `print ... >> "file"` (fopen append)
- `awk_close_all_streams()` を main 末尾で実行 → sort 系が EOF 受け取って出力

## 残タスク

### Phase 2 — astrogre 統合 (本命)

#### Level 1: library として astrogre を呼ぶ
- [ ] `/regex/` literal トークン化 (slash-vs-division 曖昧性解消)
- [ ] `~` / `!~` 演算子
- [ ] `sub(re, repl[, target])` / `gsub(re, repl[, target])` builtin
- [ ] `match(s, re)` (RSTART / RLENGTH set)
- [ ] `split(s, arr, re)` 第3引数 regex
- [ ] dynamic regex (`$0 ~ pattern_var`)
- [ ] FS が regex のとき / RS が regex のとき
- [ ] sample/astrogre を Makefile で別ターゲットからリンク

#### Level 2: 2 AST traversal interpreter 並存
- [ ] `node.def` を 2 つにマージする方針確定 (現状 arawk 側は `arawk_node_*` prefix 済)
- [ ] astrogre 側も `agre_node_*` prefix にする (要 astrogre 改修)
- [ ] CTX 統合 (awk_record + agre rep_stack)

#### Level 3: 単一 interpreter で awk + regex を実行
- [ ] VALUE 統一 (awk LSB-tagged ↔ agre int64 MR_*)
- [ ] RESULT 状態空間統合 (NEXT/EXIT/RETURN ↔ MR_FAIL/STOP/CONTINUE)
- [ ] dispatcher テーブル統合
- [ ] SD bake が両 AST にまたがる挙動を検証

### その他

- [ ] getline (`getline var < "file"`, `"cmd" | getline var`, `getline` from current input)
- [ ] `$N` lvalue + `++` の組み合わせ精査
- [ ] OFS / ORS を env から読む (現状 `arawk_node_print` がハードコード " " / "\n")
- [ ] FS 変更時の再 split
- [ ] indirect function call (`@f()` gawk extension; 多分やらない)
- [ ] perf: `print` chunked write、`$N` キャッシュ、AOT 効果調査
- [ ] benchmark report を docs/perf.md にまとめる

## 直近の小バグ・気づき

- ASTroGen の `parse_def_head` は `name(params)` を 1 行で要求 (改行不可) → memory に記録済
- ASTro framework は NULL NODE* を許さない → Null object pattern (`arawk_node_noop`) で対応 → memory に記録済
- NF は record read 直後に決まる必要あり (lazy split は NF=0 を返す) → eager split で対応
- strtod は C99 で `inf` / `infinity` / `nan` 認識 → awk semantics と乖離。明示的に先頭が digit/sign-digit/`.digit` でなければ 0 を返すように調整
- bash heredoc が `!` を escape → Write tool 使用 (memory 記録済)

## テストとベンチ

- `make test` で 18 内部 smoke + 18 goawk tt.* テスト (regex 系 6 skip)
- `make bench` で gawk / mawk / goawk と比較
