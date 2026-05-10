# baruby TODO

完了済みは [done.md](done.md) を参照。仕様詳細は [spec.md](spec.md)、
実装ノートは [runtime.md](runtime.md)。

## P0 — 動作の基本確認

- [ ] **AOT (`-c`) で新ノードが正しく specialize されるか確認**
      `node_str_lit(const char *, uint32_t)` / `node_call_*` の HORG
      ハッシュと SD 出力 (`code_store/SD_*.c`) を確認。
      naruby の `node_call_builtin` の `const char *` 扱いに揃えれば
      動くはずだが要検証。
- [ ] **PG (`-p`) でも同様に動作確認**。
- [ ] **JIT (`-j`) を再有効化するか撤去するか決める**。`astro_jit.c` は
      フォーク元のまま残してあるが、新ノードが流れたとき何が起こるか
      未検証。当面 unwired のままで OK。

## P1 — 言語拡張

- [ ] **`<=>`** (3-way 比較)。`baruby_str_cmp` のラッパー。Integer
      もサポートしたいなら別ノードか `node_call_<=>` 形。
- [ ] **`each` どうするか**。block 入れない方針なので、`for x in arr;
      ...; end` を desugar するか、`while + index` で書かせ続けるか。
- [ ] **`String#*` / `Array#*`** (反復)。
- [ ] **`<<` 追加**。Array は push、String は append (mutating)。
      演算子なので `is_binop` に追加 + `node_add_assign` 系の別ノード。
- [ ] **String escape の検証**。今は prism の `unescaped` をそのまま
      コピーしているが、`"\n"` / `"\t"` / `"\xFF"` が期待通りに来るか
      未検証。
- [ ] **負の `String#[]` slice 末尾基準**。今は `[-3, 2]` までは動くが
      `s[-3..]` 形式は Range 必要。

完了: nil/false 分離・true/false/nil リテラル・to_s/to_i・String 順序
比較・String/Array slice・interpolation。

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
