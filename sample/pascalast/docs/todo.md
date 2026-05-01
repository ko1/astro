# todo.md — outstanding work

ティア 1 (ISO 7185 必須 4 項目) は完了。残るのは ISO 7185 の細部、Free
Pascal 拡張、性能のうち高度なもの。「Pascal として完成」のラインは
越えていて、以下は「快適にする」「速くする」フェーズ。

## ISO 7185 の細部 / 残り

| # | 項目 | 工数 | 備考 |
|---|------|------|------|
| 1 | **`goto` / label** | 小 | `break`/`continue`/`exit`/`raise` でほぼカバーできているが、ISO 仕様には残る。setjmp+longjmp で実装可能。 |
| 2 | **Subrange の範囲チェック** | 小 | 代入時 / for-loop 境界で min/max 検査。`{$R+}` ディレクティブで on/off。 |
| 3 | **N 次元 (≥ 3) 配列** | 中 | per-array に stride table を持たせれば一般化。1D/2D の専用ノードは残してホットパス維持。 |
| 4 | **Open-array param `array of T`** | 中 | base + length の 2 値で渡す必要がある。slot を 2 つ消費するか、heap struct を介す。 |
| 5 | **配列の `for-in`、set の `for-in`** | 小 | 配列は既に対応。set 列挙はビット走査ループに desugar。 |
| 6 | **`Result` 暗黙型推論** | 小 | 現状は OK だが、関数戻り型の前方参照を厳密化。 |

## クラス機構の高度化（残り）

| # | 項目 | 工数 | 備考 |
|---|------|------|------|
| ~~7~~ | ~~Virtual / override 真のディスパッチ~~ | — | **完了** (round 5)。vtable + node_vcall。 |
| ~~8~~ | ~~`inherited`~~ | — | **完了** (round 5)。 |
| ~~9~~ | ~~`destructor`~~ | — | **完了** (round 5)。`obj.Done` で呼ぶ。リソース回収は libgc 任せ。 |
| 10 | **Properties** | 中 | `property X: T read GetX write SetX` を field アクセス時にメソッド呼び出しに置換。 |
| 11 | **Abstract methods / `is`/`as`** | 中 | 型タグの runtime 比較。 |
| 12 | **Class methods** | 小 | static メソッド (Self なし)。 |

## 文字列・コレクション

| # | 項目 | 工数 | 備考 |
|---|------|------|------|
| 13 | **AnsiString フル機能** | 小〜中 | `copy / pos / delete / insert / val / str / setlength`。char* で大体行ける。 |
| 14 | **dynamic array `array of T` (値)** | 中 | length() / setlength() で確保。 |
| 15 | **`TStringList` 風の標準コレクション** | 中 | クラス基盤がそろったので追加可能。 |

## 性能

| # | 項目 | 期待効果 |
|---|------|---------|
| P1 | **Body NODE\* を SPECIALIZE 時に bake する pcall** | 再帰系 AOT が 2× → 4-6× に伸びる見込み (現在 cache はあるが SD 越しに inline できない) |
| P2 | **PGC / Hopt** | 分岐が偏る real 数値計算で効く |
| P3 | **for-loop の literal bounds 特化** | `for i := 1 to 100` を fixed-trip に |
| P4 | **alloca フレーム** | call-heavy bench で +10-30% |
| P5 | **Inline-cache for class method** | virtual 化したときの monomorphic な site に有効 |
| P6 | **JIT (L0 / L1 / L2)** | 他の ASTro サンプル並みの起動高速化 |

## サンプル整備

- **Free Pascal RTL self-tests** や **BSI Pascal validation suite** に
  pass 数を当てる。
- **`fpc -O3` ビルド**との直接比較 (現状は naïve interpreter と AOT)。
- **ベンチに OOP バージョン** を追加 (matmul-class とか)。
- **`pascalast -p FILE.pas`** — PGC 駆動。
