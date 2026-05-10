# baruby TODO

完了済みは [done.md](done.md) を参照。仕様詳細は [spec.md](spec.md)、
実装ノートは [runtime.md](runtime.md)。

## P1 — 言語拡張

- [ ] **`each` どうするか**。block 入れない方針なので、`for x in arr;
      ...; end` を desugar するか、`while + index` で書かせ続けるか。
      → 今のところ後者で困っていない。
- [ ] **負の `String#[]` slice 末尾基準**。今は `[-3, 2]` までは動くが
      `s[-3..]` 形式は Range 必要。
- [ ] **`Array#<=>`**。Ruby 仕様だと要素ごとに `<=>` を取って最初に
      非ゼロが出たところを返す。再帰呼び出しで実装可能だがまだ書いて
      ない。

完了: nil/false 分離・true/false/nil リテラル・to_s/to_i・String 順序
比較・String/Array slice・interpolation・`<=>`・`*` repeat・`<<`・
escape (`\n`/`\t` 等のハンドリング、`p` 表示の inspect 化)。

## P1 — パフォーマンス

- [ ] **String literal の intern pool**。今は eval 毎に fresh alloc。
      これは GC bench としては feature だが、実用には遅い。parse-time に
      `BaString *` を生成して `node_str_lit_const(BaString *)` ノードで
      参照するパスを用意する。bench/string_concat とは別の bench を
      作って差を測る。
- [ ] **`node_add` の specialization**。
      profile で int+int だけと判明したら `_int` variant を baked SD に
      落としたい。今は generic node 1 本。`call_size_ary` / `call_size_str`
      も同様。
- [ ] **小さい配列の inline allocation**。len ≤ N の場合 `items` を
      header 直後に置く。一層化で alloc 1 回 → 0 回 (header が大きくなる
      だけ)。

## P2 — GC 基盤の本命

- [ ] **`docs/gc_design.md` の `value.def` の最初の対象として baruby を使う**。
      naruby は `none` GC tier だが、baruby は値表現が
      LSB-tag fixnum + Array + String の 3 種類だけなので一番素直に
      precise GC が書ける。手順:
  1. `value.def` の DSL 形式を docs/gc_design.md §4 で固める
  2. baruby に `value.def` を追加 (BaArray / BaString のフィールド宣言)
  3. ASTroGen で `node_gc.c` 相当 (alloc / mark) を生成
  4. libgc を draft 版 precise non-moving に置換、bench で比較
- [ ] **frame iterator (root 列挙)**。CTX.fp + locals_cnt から root を
      列挙する関数を生成する仕組み。

## P2 — 言語の毛色

- [ ] **`break` / `next` / `redo`** in `while` ループ。
- [ ] **多重代入** `a, b = x, y`。
- [ ] **デフォルト引数** `def f(x, y = 0)`。
- [ ] **splat** `*args` / `*arr` 展開。

これらはすべて GC testbed としては不要。実装するとしたら baruby を
拡張するか、別 fork を切るか要相談。

## メンテ

- [ ] `make bench` の plain 行と AOT/PG 行を比較表で出す (今は plain のみ)。
- [ ] CRuby の参考時間と並べて表に出す (binary_trees / list_alloc /
      string_concat 同等を CRuby で実行してベースラインに)。
