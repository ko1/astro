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
| R が何を refine しているか / 誰のものか | R の class-ivar `@__refine_target` / `@__refine_owner` |
| module M が持つ refinement 一覧 | M の class-ivar `@__refinements` = flat Array `[C0, R0, C1, R1, ...]` |
| 現在有効な activation set | `CTX.refinements` = flat Array `[C0, R0, M0, C1, R1, M1, ...]`、後ろほど優先 (後の `using` が勝つ)。空は `KORB_NIL`。`C == nil` の 3 つ組は「owner M を lookup 時に引く」遅延エントリ (refine block 用) |
| scope の退避 | `CTX.refine_saved` / `refine_n` / `refine_cap` (GC visit 済みスタック)。C ローカルに VALUE を持つと moving GC で stale になるので使えない |
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

## 4. 実装したもの

- `Module#refine(class_or_module) { }` — 匿名 Refinement module の生成/再利用、
  block を self=R / definee=R で実行、R を返す。
  ArgumentError (引数なし / block なし)、TypeError (Class/Module 以外)。
- `Refinement` クラス、`Refinement#target`、`#import_methods`。
  `include` / `prepend` は TypeError、
  `append_features` / `prepend_features` / `extend_object` は undef。
  refinement を `include` / `prepend` / `extend` に渡すと TypeError。
- `Module#refinements`
- `Module#using(M)` / `main.using(M)` — TypeError (module 以外 / Class)、self を返す。
- `Module.used_modules` / `Module.used_refinements` (Module の singleton に C で直接。
  prelude の Ruby wrapper 越しだと `korb_invoke_method` が呼び出し元の scope を
  捨ててしまい常に空になる)
- refined dispatch: 明示レシーバ send、暗黙 self call、
  `send` / `__send__` / `public_send`、`Symbol#to_proc` / `&`
- refinement method 内の `super` が「refine されていない元のメソッド」に届く
  (`korb_super_find`: def_class が refinement module ならレシーバの通常 MRO から
  探し直す。他の active な refinement は見ない)
- reflection: `respond_to?` / `method` / `public_method` (`korb_responds_to` 経由) と
  `Module#instance_method` (owner は refinement module)
- refine block 内の `alias` / `alias_method` が refine 対象クラスから元メソッドを引く
- refine block 内で定義された基本演算子 (`+` 等) は
  `korb_check_basic_op_redef` を target クラスに対して打って deopt する

## 5. 意図的に実装しないもの (既知の差)

- **`using` の method scope チェック** — CRuby は method の中で `using` すると
  `RuntimeError: Module#using is not permitted in methods`。koruby には
  実行時の frame stack を読む手段が無い (`korb_frames_*` は別ブランチが触っている)
  ので判定できず、黙って有効化してしまう。
  repro: `Module.new { def self.foo; using Module.new {}; end }.foo` が
  例外にならない。`core/module/using_spec.rb` 1 例、`core/main/using_spec.rb` 2 例。
- **string interpolation** — `"#{1}"` は node.def の専用パスを通るので
  refine した `Integer#to_s` が効かない。node.def は編集対象外。
  repro: `Module.new { using r; p "#{1}" }` (r は Integer#to_s を "foo" にする)。
- **`Module#instance_methods` に refinement のメソッドが入らない** —
  CRuby では refine block 内の `instance_methods` が refine 対象の一覧を返す。
  repro: `Module.new { refine(Array) { p instance_methods == Array.instance_methods } }`。
- **C 実装しか持たない module の `import_methods`** — koruby では builtin の
  module function は singleton 側にあり、`method_cnt == 0` に見えるので
  ArgumentError を出せない。repro: `refine(String) { import_methods Zlib }`。
- **block の definition-site set** — block は「定義された scope の set」ではなく
  「実行中フレームの set」を見る。C の builtin (`Array#map` 等) は set を
  変えないので `map { }` の類は CRuby と一致するが、Ruby で書かれた
  メソッドに block を渡すと callee の set になってしまう。
- **`m->is_simple` な callee への漏れの回避コスト** — refinement が有効な間、
  解決された simple ISEQ はその場で `is_simple = 0` に落ちる (以後
  `korb_invoke_method` 経由)。refinement を使うプログラムだけが払う。
- **Thread / Fiber** — activation は CTX ごと。別スレッドには伝播しない。
- **visibility** — refinement 経由で解決したメソッドは private/protected 判定を
  行わない (常に public 扱い)。
- **`Module#used_modules` の ancestors 展開** — `using` した M の ancestors から
  来た refinement も used_refinements には入るが、used_modules は
  M と ancestors の両方を返す (CRuby もそう)。

## 6. 実測 (2026-09-01)

| spec | before | after |
|---|---|---|
| `core/module/refine_spec.rb` | 12 / 38 | 36 / 38 |
| `core/module/using_spec.rb` | 4 / 20 | 19 / 20 |
| `core/refinement/import_methods_spec.rb` | 3 / 15 | 14 / 15 |
| `core/main/using_spec.rb` | 1 / 10 | 8 / 10 |
| `core/module/used_refinements_spec.rb` | 0 / 4 | 4 / 4 |
| `core/refinement/*` (他 7 file) | 3 / 11 | 10 / 11 |
| 上記 + `kernel/eval` + `language/constants` 合計 | 161 / 257 | 233 / 257 |

`perf stat -e instructions` / `fib(30)` (merge 済み master が基準):
interp 924.80M → 923.87M (-0.10%)、AOT 484.85M → 483.77M (-0.22%)。
未使用時に dispatch のホットパスへ命令を足していないので、差はノイズの範囲。
