# todo.md — outstanding work

ティア 1 (ISO 7185 必須 4 項目) は完了。残るのは ISO 7185 の細部、Free
Pascal 拡張、性能のうち高度なもの。「Pascal として完成」のラインは
越えていて、以下は「快適にする」「速くする」フェーズ。

## ISO 7185 の細部 / 残り

| # | 項目 | 工数 | 備考 |
|---|------|------|------|
| 1 | **`goto` / label runtime** | 中 | 構文受付済み (round 6)。実装は body-level dispatch wrapper が要る — EVAL の force-inline と setjmp が衝突するため、proc body 全体を setjmp ループに wrap して、longjmp 後に label idx で resume する設計が必要。 |
| ~~2~~ | ~~Subrange の範囲チェック~~ | — | **完了** (round 7+8)。代入時に node_range_check で catchable raise、`{$R+}/{$R-}` (および `(*$R+/-*)`) ディレクティブで toggle 可能 (round 8)。未知の `{$H+}` / `{$MODE OBJFPC}` 等は黙ってスキップ。 |
| 3 | **N 次元 (≥ 3) 配列** | 中 | per-array に stride table を持たせれば一般化。1D/2D の専用ノードは残してホットパス維持。 |
| ~~4~~ | ~~Open-array param `array of T`~~ | — | **完了** (round 8 続き)。dynamic-array 値型として受ける形で実装。callee 側は既存の dynarr ノード (length / a[i] / a[i] := v) をそのまま再利用、low/high 組込みも追加。静的配列の自動 wrap はまだ — 動的配列のみ受け付ける。 |
| ~~5~~ | ~~配列の `for-in`、set の `for-in`~~ | — | **完了** (round 8)。dynarr の場合 `for IDX := 0 to length(d)-1 do begin x := d[IDX]; BODY end` に desugar、set の場合 `for IDX := 0 to 63 do if IDX in S then begin x := IDX; BODY end` に desugar。 |
| 6 | **`Result` 暗黙型推論** | 小 | 現状は OK だが、関数戻り型の前方参照を厳密化。 |

## クラス機構の高度化（残り）

| # | 項目 | 工数 | 備考 |
|---|------|------|------|
| ~~7~~ | ~~Virtual / override 真のディスパッチ~~ | — | **完了** (round 5)。vtable + node_vcall。 |
| ~~8~~ | ~~`inherited`~~ | — | **完了** (round 5)。 |
| ~~9~~ | ~~`destructor`~~ | — | **完了** (round 5)。`obj.Done` で呼ぶ。リソース回収は libgc 任せ。 |
| ~~10~~ | ~~Properties~~ | — | **完了** (round 6)。field-backed と method-backed 両方。 |
| ~~11~~ | ~~`is` / `as`~~ | — | **完了** (round 6)。vtable 同定 + 親チェーン走査。 |
| ~~11b~~ | ~~Abstract methods~~ | — | **完了** (round 7)。pascal_raise で catchable に。 |
| ~~12~~ | ~~Class methods~~ | — | **完了** (round 7)。`class procedure / class function`、Self なしで T.foo(args) 呼び出し。 |
| ~~12b~~ | ~~Visibility 強制~~ | — | **完了** (round 8)。field と method の vis (`private` / `protected` / `public` / `published`) を保持し、`obj.field` / `Self.field` / `(p as T).field` のフィールド各アクセス点と `obj.method` / `b.method` のメソッド呼び出し点で `check_access` を呼ぶ。クラス外からの private/protected はエラー、protected は declaring class とその子孫からのみ可。property の visibility 強制は未対応 (property は内部的に field か method 経由なのでバックドアにはならない)。 |

## 文字列・コレクション

| # | 項目 | 工数 | 備考 |
|---|------|------|------|
| ~~13~~ | ~~AnsiString フル機能~~ | — | **完了** (round 6)。`copy / pos / insert / delete / setlength / IntToStr / StrToInt / FloatToStr / StrToFloat` + 文字↔文字列の自動 promote。 |
| ~~14~~ | ~~dynamic array `array of T` (値)~~ | — | **完了** (round 7)。slot に `int64_t *` (ptr[0]=length), `setlength` / `length` / `a[i]`。 |
| 15 | **`TStringList` 風の標準コレクション** | 中 | クラス基盤がそろったので追加可能。 |

## 性能

| # | 項目 | 期待効果 |
|---|------|---------|
| ~~P1~~ | ~~**Body NODE\* を SPECIALIZE 時に bake する pcall**~~ | **完了** (round 8)。recursion benches: fib 4.2× / tarai 5.5× / ack 5.8× / matmul 19× / mandelbrot 26-37×。`@nohash` 修飾子 + `pcall_K_baked` 系列 + post-parse fixup + eager dispatcher_name set。 |
| ~~P5~~ | ~~**Inline-cache for class method**~~ | **完了** (round 8)。`node_vcall` が `struct vcall_cache` を持ち、receiver vtable がヒットすれば proc-table lookup を skip して `pascal_call_baked` を直接呼ぶ。oop_shapes 14.6× → 7.6× (vs fpc -O3)。 |
| ~~P-extra~~ | ~~**`needs_display` フラグ**~~ | **完了** (round 8)。proc が nested children を持たないとき `pascal_call_baked` の display save/restore を gcc が DCE する。call-heavy bench で +5-10%。 |
| P2 | **PGC / Hopt** | 分岐が偏る real 数値計算で効く |
| P3 | **for-loop の literal bounds 特化** | `for i := 1 to 100` を fixed-trip に |
| P4 | **alloca フレーム / fp threading** | call-heavy bench で 1.5-2× 期待。c->fp / c->sp の毎-call メモリトラフィックが現在の主要ボトルネック (perf record で fib AOT は 99% が SD 内、再帰呼出し前後で gcc が c->fp / c->sp を reload するため)。共通 NODE_DEF 引数に `int fp` を追加して dispatch chain で thread すれば fpc -O3 のレジスタ受け渡しに近づける。316 NODE_DEF への引数追加が必要なので大改修。 |
| P5 | **Inline-cache for class method** | virtual 化したときの monomorphic な site に有効 |
| P6 | **JIT (L0 / L1 / L2)** | 他の ASTro サンプル並みの起動高速化 |

## サンプル整備

- **Free Pascal RTL self-tests** や **BSI Pascal validation suite** に
  pass 数を当てる。
- **`fpc -O3` ビルド**との直接比較 (現状は naïve interpreter と AOT)。
- **ベンチに OOP バージョン** を追加 (matmul-class とか)。
- **`pascalast -p FILE.pas`** — PGC 駆動。
