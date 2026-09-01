# Refinements (`Module#refine` / `using`)

koruby は長らく refinement を「無害な no-op」(`refine` は nil を返し `using` は
何もしない) として扱ってきた。本ドキュメントは **部分実装** の設計・実装範囲・
意図的に落とした部分を記録する。

方針: **未使用時のコストをゼロにする**。`using` が 1 度も実行されていない
プログラムでは、dispatch のホットパス (node_send / node_call の inline cache
ヒット、`korb_invoke_simple`) に一切の追加命令が入らない。

## 1. データ表現

| もの | 置き場所 |
|---|---|
| refinement module R (`refine C do ... end` が作る無名 module) | 普通の匿名 module。`Refinement` を `#class` として被せる (`korb_klass_override_set`) |
| R が何を refine しているか | R の class-ivar `@__refine_target` |
| module M が持つ refinement 一覧 | M の class-ivar `@__refinements` = flat Array `[C0, R0, C1, R1, ...]` |
| 現在有効な activation set | `CTX.refinements` = flat Array `[C0, R0, M0, C1, R1, M1, ...]`、後ろほど優先 (後の `using` が勝つ)。空は `KORB_NIL` |
| method が定義された時点の activation set | `struct korb_method.refine_set` (VALUE、GC visit 済み) |
| 「プログラム中で refinement が 1 度でも有効になったか」 | `vm->refinements_active` (bool)。`korb_check_basic_op_redef` / `vm->basic_op_redefined` と同じ deopt flag パターン |

## 2. スコープ (activation) の扱い

CRuby の activation は cref(lexical scope) に属する。koruby には実行時 cref chain
が無いので、**「レキシカルスコープの入れ物ごとに `CTX.refinements` を save/restore
する」** という近似を採る。save/restore する場所:

- class / module body (`korb_class_body`)
- `Class.new { }` / `Module.new { }` の body block
- `class_eval` / `module_eval` / `instance_eval` の block と String 形式
- `refine C do ... end` の block (中では M の refinement 全部が有効 = CRuby 通り)
- メソッド呼び出し (`korb_invoke_method`): callee には **その method が定義された
  時点の set** (`m->refine_set`) を入れる。呼び出し元の set は漏れない
- ファイルの load / require (`korb_load_file_scope`)

`using M` は現在の `CTX.refinements` のコピーに M (と M の ancestors) の
refinement を追加したものを立てる。M の refinement 表は `using` 時点で
**スナップショット** される — CRuby と同じく、
「`using` 後に既存の R にメソッドを足す」のは効く / 「`using` 後に新しいクラスの
R を M に足す」のは効かない。

## 3. Dispatch

`vm->refinements_active` が false の間は一切変更なし。true になった瞬間に
`vm->method_serial++` で全 inline cache / mcache を無効化し、以後
**inline cache / call cache への書き込みを停止する** (`korb_ic_fill` /
`korb_cc_fill` が no-op になる)。これで node.def 側の inline fast path
(編集不可) も自動的にヒットしなくなり、全 send / call が
`korb_send_cached` / `korb_call_cached` に落ちる。そこで refinement を先に引く。

lookup 順 (`korb_refined_find`):

1. activation set を後ろから走査
2. entry の target C がレシーバの ancestor で、かつ R が mid を持つ
3. かつ「通常探索で見つかる定義の owner」が C の ancestry にある (= C より下の
   クラスで定義されていない) なら、R の method を採用

これで CRuby の "refinement は C の直前に挿入される" 規則の主要な帰結
(singleton が勝つ / subclass が勝つ / include したモジュールが勝つ /
refine(Enumerable) が Array に効く) を満たす。

`m->is_simple` な ISEQ は `korb_invoke_simple` (node.h、編集回避) を通り
callee set を入れられないので、refinement 有効時に解決された ISEQ は
**その場で `is_simple = 0` に落とす** (以後 `korb_invoke_method` を通る)。

## 4. 実装するもの

- `Module#refine(class_or_module) { }` — 匿名 Refinement module の生成/再利用、
  block を self=R / definee=R で実行、R を返す。
  ArgumentError (引数なし / block なし)、TypeError (Class/Module 以外)。
- `Refinement` クラス、`Refinement#target`、`#inspect`。
  `include` / `prepend` / `append_features` / `prepend_features` / `extend_object`
  は TypeError。
- `Module#refinements`
- `Module#using(M)` / `main.using(M)` — TypeError (module 以外 / Class)、
  method scope での RuntimeError、self を返す。
- `Module.used_modules` / `Module.used_refinements`
- refined dispatch: 明示レシーバ send、暗黙 self call、
  `send` / `__send__` / `public_send`
- refine block 内で定義された基本演算子 (`+` 等) は
  `korb_check_basic_op_redef` を target クラスに対して打って deopt する

## 5. 意図的に実装しないもの (既知の差)

- **`super` in a refinement** — refinement method 内の `super` が
  「refine されていない元のメソッド」に届かない。`korb_super` は
  `def_class` の superclass から探すので、R (superclass = nil) では届かない。
- **`Refinement#import_methods`** — 未実装 (NoMethodError)。
- **reflection** — `respond_to?` / `method` / `instance_method` /
  `public_method` / `Symbol#to_proc` / `&:sym` / `Kernel#binding` は
  refinement を見ない。CRuby ではアクティブなスコープからは見える。
- **block の definition-site set** — block は「定義された scope の set」ではなく
  「実行中フレームの set」を見る。C の builtin (`Array#map` 等) は set を
  変えないので `map { }` の類は CRuby と一致するが、Ruby で書かれた
  メソッドに block を渡すと callee の set になってしまう。
- **string interpolation / 基本演算子ノード** — `"#{x}"` や `a + b` は
  node.def の専用パスを通るので、そこに refinement は効かない
  (`basic_op_redefined` を打つ分だけ演算子は救われる)。
- **Thread / Fiber** — activation は CTX ごと。別スレッドには伝播しない。
- **visibility** — refinement 経由で解決したメソッドは private/protected 判定を
  行わない (常に public 扱い)。
- **`Module#used_modules` の ancestors 展開** — `using` した M の ancestors から
  来た refinement も used_refinements には入るが、used_modules は
  M と ancestors の両方を返す (CRuby もそう)。
