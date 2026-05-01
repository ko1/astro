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

### 完了 (round 8 一連)

| 項目 | 効果 |
|------|------|
| ~~P1: body NODE\* SPECIALIZE bake~~ | recursion benches を 2× → 4-6× に。 `@nohash` + `pcall_K_baked` 系列 + post-parse fixup + eager dispatcher_name。 |
| ~~P5: vcall inline cache~~ | oop_shapes 14.6× → 5.4× (vs fpc -O3)。 receiver vtable hit で proc-table lookup を skip。 |
| ~~P-needs_display~~ | nested children のない proc で display save/restore を AOT で DCE。 |
| ~~P4: fp threading (alloca-frames-lite)~~ | `int fp` を common parameter に。c->fp の memory traffic 全消滅。call-heavy で +8-29%。 |
| ~~P-via_body~~ | body の tail が `Result := expr` のとき C return path で値を返す。 fib/tarai/gcd で +13-22%、slot read 1 回 + slot write 1 回が消える。 |
| ~~P-zero-fill~~ | Pascal 仕様通り locals を uninit に。 pascal_call_baked から zero-fill loop を削除。 gcd 28%、ackermann 14% 改善。 |

### 残り

| # | 項目 | 期待効果 |
|---|------|---------|
| **P2/P6 (統合)** | **PGC / Hopt → JIT** | 本質的に同じ最適化。 PGC は静的二段 (`pascalast -p` で profile dump → `pascalast -c -P prof` で profile-guided AOT)、JIT は同プロセスで動的にトリガー。両方とも:<br>- vcall site の guarded direct call (devirtualization) で oop_shapes 4.6× → 1-2× が見込める<br>- branch 偏り情報で `__builtin_expect` を AOT 出力に注入<br>- 観測された定数を bake<br>JIT (P6) は実装的には PGC (P2) + 動的 trigger なので、P2 を先に作れば P6 は薄い差分。 |
| P3 | for-loop の literal bounds 特化 | `for i := 1 to 100` 等を fixed-trip に。AOT で from/to が node_int(literal) になっていれば gcc は既に展開しているはずだが、確認していない。 |
| P-cleanup-flag | exit_pending/loop_action check の skip | proc body に break/continue/exit/halt がなければ pascal_call_baked 末尾の UNLIKELY check 2 回が完全に DCE 可。 fib で +3-5% 見込み。 |
| P-leaf-no-sp | leaf proc で c->sp 更新を skip | 内部呼出しを持たない proc 専用。 fib/tarai のような recursion ヘビーなものには効かないが、math 系の inner function に効く可能性。 |

## サンプル整備

- **Free Pascal RTL self-tests** や **BSI Pascal validation suite** に
  pass 数を当てる。
- ~~**`fpc -O3` ビルド**との直接比較~~ ([`docs/compare_fpc.md`](./compare_fpc.md))
- **ベンチに OOP バージョン** を追加 ([`bench/oop_shapes.pas`](../bench/oop_shapes.pas) で着手済)。
- **`pascalast -p FILE.pas`** — PGC 駆動 (P2 と同じ)。
