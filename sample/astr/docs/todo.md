# astr todo

優先度高めから順。

## 言語

- [ ] **lexical closure**: 現状トップレベル関数のみ。`function(...) function(...) ...`
  形やネスト関数を環境チェーンで扱えるようにする (pystro が参考)。
- [ ] **multi-element subscript**: `v[1:3]`、`v[v > 0]`、negative-index 除外
  形 `v[-1]`。今は scalar idx のみ。
- [ ] **`[[ ]]` / `$` アクセス**: list / data.frame に必要。
- [ ] **`apply` / `sapply` / `Map` / `Reduce`**: ベクタ/リスト用 higher-order。
- [ ] **integer/double 区別の徹底**: 現状 `1` を fixnum 扱いしているが
  R は `1` を double、`1L` を integer とする。fib bench の速度を保つには
  ASTroGen の specialize 段で型情報を伝播させるか、parser annotation で
  逃げる。
- [ ] **`tryCatch` / signal**: 例外ハンドリング。castro/pystro パターンの
  `setjmp` 利用が候補。
- [ ] **regex / `grepl` / `gsub`**: `sample/astrogre` 連携待ち。

## ランタイム

- [ ] **vector arithmetic AOT 対応**: 現状ベクタ算術は `astr_*_slow` 経由で
  毎回 `numvec` を heap allocate。SD specialize しても speed up しない。
  loop fusion か pre-allocated workspace で改善。
- [ ] **string interning**: `astr_make_string` を呼ぶたびに heap copy。
  リテラル文字列は parser 段で intern するか、CharSXP 風に
  hash table で重複排除。
- [ ] **list の `STR_VEC` 表現分離**: 今 `STR_VEC` は `lst.items` を間借り
  して文字列 VALUE を入れている。専用フィールドで型不変条件を強くする。
- [ ] **NA / NULL の伝搬**: 算術や比較で NA 入力を受けると 0 になる。
  R は NA を伝搬すべき。
- [ ] **vector recycling 警告**: 短い側が長い側の倍数でない場合は警告。

## AOT / ビルトイン

- [ ] **builtin func_ptr の AOT 安全化**: 今 `void *func_ptr` は SD 内で
  `(void *)NULL` ベイクされる。AST top-level → seq 経由で
  実体には届かないので segfault しないが、SD chain が builtin を
  inline できる経路を作るときに地雷になる。`name → func_ptr` の
  ランタイム解決 (1 回キャッシュ) に書き換える。
- [ ] **PG bake**: naruby パターンの sp_body 推測を導入すれば fib の
  recursive call も完全インライン化できる。

## テスト / ベンチ

- [ ] **R-base 比較**: `Rscript` がインストールされたら `make bench` が
  自動で並べる harness はもう入っているので、ベンチ規模を R が ~1s で
  終わるサイズに調整。
- [ ] **shootout 系の小ベンチ**: nbody, fannkuch-redux, mandelbrot 等を
  追加して double-heavy / vector-heavy 経路のカバレッジを上げる。
