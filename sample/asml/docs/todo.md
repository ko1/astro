# 残課題

Phase 1〜2 の主要機能 (パース / HM 推論 / 型駆動特殊化 / 部分適用 /
末尾呼出 / 例外 / record / generic ノード削除 / is_leaf / tail-call rewrite /
AOT cflags) は実装済み。

## 言語仕様 — 未実装

### モジュールシステム
- `signature` / `structure` / `functor`
- `local in end`、`open`
- 修飾名 `Module.name` は字句で 1 トークンに glue するだけ (本物の名前空間は無い)

### 型システム — 残り
- **型注釈の構文** — `(e : T)`, `fun f (x : int) = ...`, `val x : int = ...`
  はパース時に reject される。本物の SML だと推論支援になる
- **Recursive types の occurs check** — `'a -> 'a` が `'a = 'b list ->
  'b list -> ...` 型に膨らむケースで occurs を入れているが、エラーメッセ
  ージが分かりにくい
- **エラー位置の richer report** — 現状は `tk.line` ベースで line 番号のみ。
  どの operand 由来かのコンテキストが無い
- **多相 `=` の eq-type 制約** — SML は equality 型クラスがあり、関数型
  などは `=` で比較できない。現状は実行時に compare で fall through (関数値
  比較は false 扱い)。型レベルで弾く方が望ましい
- **`real` の overload された `+ - *`** — SML 本家は `+` が int と real の両方で
  動くが、asml では int 専用。`+. -. *.` も無い (`/` が real 専用、それ以外は op
  をプリミティブ経由で使う必要)。本格運用するなら overload か default-int 解決
  を入れたい
- **record の row polymorphism** — 現状 `#field e` は `e` の型が確定して
  いないと「ambiguous record selector」で reject される。SML の通常の
  type inference は flexible record (row var) で対応している

### 構文・小機能
- **`exception E of T` 宣言** — 現状は組み込み + datatype で代替
- **文字リテラル `#"a"`** — char 型サポート
- **`let val (a, b) = ...`** — ローカル val でタプル分解 (top-level のみ実装済み)
- **複数節 `fun f 0 = "z" | f n = "n"`** — 現状は単節 + case 必須
- **`infix` / `infixr` / `nonfix` 宣言** — ユーザ定義中置子
- **substring / String.sub / String.implode** 等の標準ライブラリ
- **`Array` / mutable 列**

### 標準ライブラリ
- `Real` モジュール: `Real.fromInt`, `Real.toString`, 三角関数等
- `Real.Math`
- `String.substring` / `explode` / `implode` / `tokens`
- `Char.toString`
- `IO` (`TextIO.openIn`, `inputLine`, `closeIn`)
- `OS.Process`

## 性能向上のための今後の課題

### ✅ A. closure に静的 is_leaf を立てる (Phase 2 で実装済)

`ex_is_leaf(EX *e)` を実装、`node_fn` の is_leaf 引数に渡す。fib / refloop
で大幅な高速化 (10× / 3.5×)。

### ✅ B. AOT compile flags 追加 (Phase 2 で実装済)

`maybe_aot_compile` で `ASTRO_EXTRA_CFLAGS` に
`-fno-stack-clash-protection -fno-stack-protector -flto
-finline-limit=10000 ...` を設定。fib AOT が 1.59s → 0.16s に。

### ✅ B'. tail-call rewrite (Phase 2 で実装済)

`mark_tail_calls(NODE *)` post-pass を `lower` 後に呼んで、tail-position の
app1/app2 を `_tail_app*` に書換。50M 段の tail recursion が定数スタックで
回るようになった。

### 🔥 C. 関数の N-ary 直接呼び出し最適化 (次の最優先)

現状: `f x y` は parser で `app1(app1(f, x), y)` に lower される。`f` が
2-arg closure の場合、`ml_apply` 1 段目で partial-state 生成 (heap
malloc!) → 2 段目で combine して `ml_apply(f, 2, [x, y])` を再呼出。

提案: `parse_app` の loop で連続適用を `node_app2` / `node_app3` に折り
畳む。fn が closure なら直接 N-arg call (既存の APPN_FAST_PATH(2/3) に乗る)。
`partial_state` malloc は arity mismatch 時のみに退ける。

期待効果: ack(3,9) で 1.52 → 0.5 s 程度、tak / nqueens でも同程度。
SML/NJ との 30× ギャップが ~5× 程度まで縮まる見込み。

### D. PGO (`-fprofile-use`)

astocaml の `make pgo` 二段ループ (binary + SD ごと) を asml にも入れる。
SD の hot path (再帰呼び出し / arm 分岐) で hit-rate ベースの分岐予測情報が
入って 5〜10% 削れる見込み。

### E. lref0 micro-specialisation

ホット引数アクセスの `node_lref(0, idx)` を `node_lref0_K` (K = 0..7) に
特殊化して `c->env->slots[K]` の add 演算を fold。astocaml の同名最適化
参照。

### F. record field access IC

`#field e` の field name → array index は linear search。call-site IC で
last-seen `o->rec.fields` ポインタ + 解決済 index を覚えれば、同じ record
構造の連続アクセスは 2 load + 1 cmp で済む。

### G. tuple alloca

`(a, b)` のような short-lived tuple を pattern match の中だけで使う
ケースが多い。escape analysis 風に「ヒープ alloc 不要」を検出して
スタック alloca に。tak / nqueens で効きそう。

## 追加すべきテスト

- 多相関数を引数として渡すケース (`map (fn x => x + 1) ...` 等は通っている
  が、ネストした多相適用は要検証)
- 型推論で意図せず monomorphic 化するケース (value restriction の境界)
- 例外型と datatype constructor の混在 (`exception` 未対応の代替パターン)
- 巨大プログラム (~1000 行) で線形時間で動くか

## 既知の制限・注意点

- **fun の and 内 body skip** が `if` / `case` の nesting を track しない。
  `and` を含む let 内の fun が `if .. and .. then` のような書き方を
  すると header skipper が壊れる。実用ではほぼ起きないが要注意。
  (実装は `parse_one_decl` の TK_FUN ブランチ参照)
- **`val` 内 `fun` の二段パース**でレキサ位置を巻き戻す関係上、文字列
  リテラル内の `(` `)` `let` `end` を本気で含むコードは壊れる可能性。
  実用範囲では問題ない。
- **エラー時の partial state** — 型エラーで exit 2 した時、`tenv` や
  `code_store` 等は cleanup しないが、process exit するので OK。
