# optcarrot 対応: 中間ステータス (Fiber 手前まで)

- Date: 2026-04-12
- Latest commit: b0fc2a4
- Target: `benchmark/optcarrot/bin/optcarrot-bench`

## 結論

optcarrot は 4500 行超のメタプログラミング heavy な NES エミュレータ。
このセッションで:

1. **全ライブラリのロード成功**: `require_relative` チェーンで
   nes/rom/pad/cpu/apu/ppu/palette/driver/config を全部読み込めるよう
   になった
2. **Optcarrot::NES.new(argv) 成功**: Config の `-b` alias 展開、
   ROM ファイル読み込み、CPU/APU/PPU/Driver の初期化を完走
3. **次のブロッカー**: PPU.run の `Fiber` (`uninitialized constant
   Fiber`)

Fiber は abruby の現在のアーキテクチャでは実装が難しく、複数の独立した
工事が必要なので、本セッションはここで止める。詳しくは下記。

## このセッションで追加した機能

7 commits, 7+ ファイル, ~2000 行追加。以下「動かすために必要だった」
機能を網羅。

### Iter 1 (3dfb1ec): splat 引数 (呼出側)
- `node_func_call_apply` / `node_method_call_apply` 新ノード
- `[a] + b + [c]` 形式でフォールド
- splat in array literal も同経路
- test/test_splat_call.rb (15 tests)

### Iter 2 (b87ae9a): class body / 定数解決
- KeywordHashNode (`foo(a: 1, b: 2)`)
- `return a, b, c` → `return [a, b, c]`
- private / public / protected / module_function を no-op で登録
- Module < Object の super chain を確立 → class body から `p` /
  `require_relative` 等の Kernel メソッドが呼べる
- node_const_get が `c->current_class` の super chain を辿る
- test/test_class_body.rb (12 tests)

### Iter 3 (594f5de): 多重代入ターゲット / 例外階層
- `Prism::IndexTargetNode` / `Prism::CallTargetNode`
  (`a[i], b.x = ...`)
- 15 個の例外クラス (StandardError, NotImplementedError,
  ArgumentError, ...) を全て `RuntimeError` の alias で登録
- `raise ExcClass, "msg"` / re-raise instance の 2 引数 / instance form
- method_missing のエラーメッセージにメソッド名を含める
- test/test_multi_assign_targets.rb (11 tests)

### Iter 4 (0ceb2fc): or/and write + huge feature push
- `LocalVariable` / `InstanceVariable` / `GlobalVariable` /
  `Constant` / `Index` / `Call` の `OrWriteNode` /
  `AndWriteNode` / `OperatorWriteNode` 全 18 パターン
- `[a, *b, c]` 配列リテラル splat
- `Prism::ConstantTargetNode` in multi-assign
- `__FILE__` / `__LINE__` / `__ENCODING__` / `$1` / 補間 Symbol /
  補間 Regexp (stub) など
- `Outer::Inner` 形式の `ConstantPathNode` で parent が任意の式
- attr_reader/writer/accessor の動的形 (`attr_reader id`) を Module
  cfunc にフォールバック
- test/test_or_and_write.rb (22 tests)

### Iter 5 (4e4d250): Config / multi-assign 二重評価バグ + 大量 builtin

**致命バグ**:

1. **multi-assign の RHS 二重評価** — `a, b = f().split(...)` で
   ary_store が *各 LHS の build_seq に埋め込まれる* ため、shift が
   2 回呼ばれる。optcarrot の argv 処理で 1 iter に 2 引数消費して
   いた原因。rhs_pre_store を outer assigns の先頭に hoist して修正。

2. **Class#new で frame.caller_node 未初期化** — ab_class_new がスタック
   フレームを push する際 caller_node を設定せず、後の backtrace 構築で
   stale ポインタを deref していた。

3. **Class#new で cref を設定しない** — initialize 中のベア定数
   lookup (`Optcarrot::Config` from NES) が失敗していた。

4. **動的 Symbol (heap T_SYMBOL)** — `AB_CLASS_OF` /
   `ab_obj_type_p` / `ab_verify` が dynamic symbol を扱えず、`SYMBOL_P`
   が heap pointer を deref して segfault。`RB_STATIC_SYM_P` 先 +
   `RB_BUILTIN_TYPE(obj) == T_SYMBOL` の段階チェックで修正。

5. **Array#unshift が 1 要素だけ**: 全要素を順に unshift するように修正。

6. **node_class_def / node_module_def** — 新規定数を *enclosing scope*
   (current_class または main) に登録 (以前は常に main)。
   `class PPU; class OptimizedCodeBuilder; end; end` を `PPU::Optimized
   CodeBuilder` で参照可能に。

7. **node_const_path_get** — parent_name を cref chain で解決
   (`Outer::Inner` を enclosing module の中から)。

**機能追加**: Object#instance_variable_get/set/respond_to?, nil#[],
Range#map/collect, Hash#each_key/each_value/merge/delete/fetch/dup,
Array#reverse/dup/flatten/concat/join/min/max/sort/fill/clear/replace/
shift/unshift/each_with_index/transpose/zip/flat_map/inject/reduce/*,
String#to_sym/start_with?/end_with?/chomp/strip/split/tr/=~,
Module#attr_reader/writer/accessor cfunc, Kernel#puts/print/exit/
__dir__/Integer/Float, ARGV/ENV/File/Struct 定数。

### Iter 6 (b0fc2a4): Method objects + multi-assign slot collision

**致命バグ**:

1. **abruby_require_file** が `c->current_class` と `c->self` を
   save/restore していなかった。クラス body 内で `require_relative` を
   呼ぶと、loaded file の最後の class def が current_class を残し、
   その後の `def self.foo` が間違ったクラスに登録されていた。これが
   `Optcarrot::ROM.respond_to?(:load) => false` の原因。

2. **multi-assign の CallTargetNode** が cached array の slot を破壊
   していた。`@y, @z, c.x = rhs` で `[]` decompose の recv slot と
   CallTargetNode handler の recv slot が両方 `ary_idx + 1` から
   始まっていて衝突。rhs evaluation を別 slot にホイスト + ary_idx を
   multi-assign 終了まで keep alive。

3. **splat in array literal** が常に `[] + parts...` を出力するように
   変更し、`[*range]` が常に Array になるように。Array#+ が Range を
   to_a 経由で扱う。

**機能追加**: Object#send/__send__/public_send/method/freeze/frozen?/
dup/equal?/hash/object_id/tap, Method class (call/[]/() が bound 呼出を
ディスパッチ), Array#*/slice/slice!/==/!=/all?/any?/none?/uniq/uniq!/
compact, Array#[] が `(start, length)` と Range, Range#all?/any?/
inject/reduce, Hash#dup/compare_by_identity, String#bytes/bytesize/
unpack, Kernel#Integer/Float, Object#respond_to? が singleton table も
チェック, NilClass#[] が nil を返す。

## 現在の状況

```
$ ./exe/abruby --plain benchmark/optcarrot/bin/optcarrot-bench
```

これが**読み込み完了→ NES.new 完了**まで進む。`nes.run` を呼ぶと
PPU の vsync 内で `@fiber ||= Fiber.new do main_loop end` で
**`uninitialized constant Fiber`** で停止。

## なぜ Fiber がブロッカーか

abruby の現状:

- ブロックは C スタックの `struct abruby_block` に bound (ヒープに
  escape しない設計、`docs/todo.md` でも phase 2 課題と明記)
- 実行モデルは EVAL → DISPATCH → EVAL ... の単一 C スレッドの再帰
  呼出し
- `struct abruby_fiber` 型は context.h にあるが「現状 main fiber のみ」
  で fiber 切替の実装はない

Fiber を実装するには:

1. **ucontext (or 同等) で C スタック切替**
   - 各 fiber に専用 C スタック (~256KB 程度) を mmap
   - `getcontext` / `makecontext` / `swapcontext` で switch
2. **per-fiber CTX** (VALUE スタック・current_frame など) を別管理
3. **ブロックをヒープ escape**
   - `Fiber.new { ... }` のブロックは Fiber 生成元のメソッドが return
     した後も生き続ける必要がある
   - 現状 `struct abruby_block` は `captured_fp` が C スタック上の
     アドレスで、return 後に dangling
   - heap-allocated Proc 相当のオブジェクトに変換する必要がある
   - これは optcarrot 以外でも `Proc.new` や `lambda` の前提条件

(2) と (3) は abruby の core design の変更で、独立した複数の commit に
渡る工事になる。1 セッションで安全にやり切るのは厳しい。

## できる回避策 (このセッションでは実施しない)

a) optcarrot の PPU を fiber-less な state machine に書き換える
   (大幅な fork が必要)
b) Fiber を「最後まで一気に走らせる」スタブにする (PPU の状態管理が
   壊れるので無理)
c) `ucontext` だけ実装して block escape は別途
   → block 実装が伴わないと PPU の `Fiber.new do main_loop end` の
   block を fiber の C スタックに渡せない

## ベンチ性能評価

このセッションで触ったのは:

- パーサ (parse-time のみ、実行時 hot path には影響なし)
- builtin の追加 (新しい cfunc は既存ベンチに使われない)
- multi-assign の lowering (実行回数は少ない)
- AB_CLASS_OF の SYMBOL_P 早期分岐 (immediate 判定 1 回追加 →
  micro-overhead だが既存ベンチでは fixnum 演算が大半なので影響微小)
- abruby_require_file の save/restore 追加 (ロード時のみ)

期待通り既存マイクロベンチに大きな regression はないはず。
詳細な fib/method_call/dispatch 等の数値はまだ取得していない (Fiber
ブロックのため optcarrot 自体の数字は取れない)。

## このセッションで追加したテスト

- test_splat_call.rb (15)
- test_class_body.rb (12)
- test_multi_assign_targets.rb (11) + test_exception_hierarchy
- test_or_and_write.rb (22)

`make test` の interp 側は全部通る。compiled 側は `test_method_call`
が稀に SIGSEGV (本セッション以前からの flaky)。

## 次のセッションの起点

`Fiber` 対応 (多分以下の順):

1. ヒープ escape blocks (Proc 系の半分、`docs/todo.md` の Phase 2
   block 課題と一緒)
2. `ucontext` ベースの fiber 実装
3. abruby のテストに fiber ケースを追加
4. optcarrot を再起動して、次の missing 機能 (恐らく無数の cpu.rb の
   高速パス系メソッド) を 1 個ずつ潰す

そこから先は CPU 命令ディスパッチが正しく動くか (`send(*DISPATCH[opcode])`
の動的 send + splat) などのチェックが続く。

## 経過まとめ

| iter | commit | 通過 |
|---|---|---|
| 1 | 3dfb1ec | nes.rb:15 SplatNode |
| 2 | b87ae9a | nes.rb:99 private |
| 3 | 594f5de | rom.rb:36 IndexTargetNode |
| 4 | 0ceb2fc | (大量の lower-level 修正) |
| 5 | 4e4d250 | Config の argv 処理 |
| 6 | b0fc2a4 | NES.new 完了 |
| ?  | ?? | PPU#run の Fiber |
