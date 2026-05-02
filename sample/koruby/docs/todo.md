# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み。ここは **未実装 / 不完全 / 既知バグ** をまとめる。

現状: **test/ruby/ 124 ファイル全 pass**。
optcarrot は ruby と checksum 一致、CRuby の 2.2x、YJIT の 0.55x。

---

## 1. 意図的に未実装 (project memory 通り)

| 項目 | 理由 / 代替 |
|---|---|
| **Regexp** (`=~` / `String#scan` / `match` / リテラル `/.../`) | astrorge 経由で integrate する方針。自前で書かない (`feedback memory: 真の Regexp は sample/astrorge 待ち`) |
| **Thread / Mutex / Queue / ConditionVariable** | single-threaded only。並行性は当面持ち込まない |
| **Encoding-aware String** | byte sequence のみ。`force_encoding` / `encoding` / multibyte iteration 無し |
| **NaN-boxing 値表現** | 提案禁止 (`feedback memory: NaN-boxing は luastro でやらない`) |

---

## 2. 大きく欠けている領域

### 2.1 Enumerator / Enumerator::Lazy
- `each` / `map` / `select` 等の no-block 形は **Array stand-in** (CRuby は Enumerator)
- `(1..Float::INFINITY).lazy.map.first(N)` 系の lazy chain 不可
- `Enumerator.new { |y| y << ... }` 不可
- 影響: `each_with_index.to_a` など中間 chain は動くが、本物 lazy が必要なコードは死ぬ

### 2.2 IO / File / Dir
- `File.read(path)` / `File.open` 程度のみ
- `IO.read` / `IO.write` / `IO.pipe` / `IO.select` / `STDIN.gets` 限定
- `Dir.glob` / `Dir.entries` / `Dir.chdir` 無し
- `Pathname` モジュール無し

### 2.3 Marshal / シリアライゼーション
- `Marshal.dump` / `Marshal.load` 無し
- `JSON` / `YAML` 無し (gem も無し)
- `Object#to_yaml` 無し

### 2.4 ObjectSpace / 内省
- `ObjectSpace.each_object(Klass)` 無し
- `ObjectSpace.count_objects` 無し
- `Kernel#binding` 無し → `eval(str, binding)` 不可
- `Method#parameters` / `Proc#parameters` 無し
- `Proc#source_location` / `Method#source_location` 無し

### 2.5 Complex / Rational
- リテラル `1r` / `1+2i` 未対応
- `Rational` / `Complex` クラスは stub
- Numeric#coerce の rational/complex 経路無し

### 2.6 Comparable on user Numerics
- user-defined `Numeric` subclass + `coerce` で `1 + my_num` を動かす protocol 未対応
- 現状は組込みの Integer/Float/Bignum 間のみ自然に coerce

### 2.7 Hash key with custom #hash / #eql?
- user の `def hash; ...; end` を Hash 内部の `korb_hash_value` が呼ばない
- 影響: `class Pt; def hash; [@x,@y].hash; end; end; h[Pt.new(1,2)] = ...; h[Pt.new(1,2)]` は別オブジェクト扱いで取れない
- 修正には `korb_hash_value` を CTX 持ちに refactor

---

## 3. 部分実装 / 穴あり

### 3.1 Frozen check  ✅ 2026-05-02 完
- 主要 mutator + サブ mutator に `CHECK_FROZEN_RET` 配備:
  Array `<<` / push / pop / `[]=` / unshift / shift / clear / concat /
  delete / delete_at / delete_if / reject / insert / replace / fill /
  slice! / rotate! / reverse!,
  Hash `[]=` / delete / clear / merge! / replace / delete_if / keep_if /
  compact! / shift,
  String `<<` / aset / replace / prepend / insert
- 残: `String#tr!` / `sub!` / `gsub!` 等の `!` 形は未確認 (実装そのものが薄い)

### 3.2 Object.ancestors  ✅ 2026-05-02 完
- `Object.ancestors == [Object, Kernel, BasicObject]` (CRuby と一致)
- `BasicObject` クラスを root として導入、`Kernel` を Object の include に

### 3.3 Numeric edge cases  ✅ 2026-05-02 部分完
- 完: `Integer#coerce` / `Float#coerce` (TypeError on bad arg)、`Numeric#abs2`、
  `Float#to_s` の `1.0` / `Infinity` / `NaN` フォーマット
- 未: `1 ** -1` → Rational (現状 Float 返り。Rational 演算 protocol 全体の
  整備が必要)、Bignum×Rational 等の混合
- 未: `Numeric#coerce` を経由する user-defined Numeric subclass の混合演算

### 3.4 Heredoc edge cases  ✅ 2026-05-02 検証済
- 確認: 同行に複数 heredoc、heredoc を method 引数に、`#{}` 内に method call、
  `#{}` 内に nested heredoc、`<<-` (indent 維持) と `<<~` (strip) 混在、
  single-quoted heredoc (no interp)
- bonus: String#inspect が control char (`\n`/`\t`/etc.) を escape するように
  修正 (re-evalable な repr に)

### 3.5 Pattern match (`case/in`)  ✅ 2026-05-02 完
- 完: literal / array / hash / `^x` pin / `=>` capture (`Integer => x`) /
  rightward assignment (`expr => pat`) / array `[a, *mid, b]` (slot collision
  bug 修正) / find pattern `[*, x, *]` (n=1)、nested pattern
- 未: find pattern n>1 (`[*, p1, p2, *]`) — rare、n=1 ケースで walk 実装済
- 未: `case in` ガード式 + 複雑な destructure の組み合わせ

### 3.6 method_defined? 系  ✅ 2026-05-02 完
- `method_defined?` を visibility 込み (private は false 返し) に修正
- `public_method_defined?` / `private_method_defined?` /
  `protected_method_defined?` 追加
- `private_instance_methods` / `public_instance_methods` /
  `protected_instance_methods` (`include_inherited` flag) 追加

### 3.7 Ruby 3.x 構文  ✅ 2026-05-02 完
- Endless method `def f(x) = x+1` / `def hello = "hi"`
- Hash shorthand `{x:, y:}` (PM_IMPLICIT_NODE unwrap)
- Numbered block param `_1` / `_2` (PM_NUMBERED_PARAMETERS_NODE)

### 3.8 Backtrace
- `e.backtrace` 動くが内容が CRuby と異なる (line 番号 / file 名フォーマット)
- `Kernel#caller` も簡易版
- 細部は実用上問題ないので保留

---

## 4. 既知バグ (テストにコメント付きで残してあるもの)

無し — 2026-05-02 時点で全 documented バグは修正済み。
過去にあった `procs << proc { i }` per-iteration capture も `creates_proc` flag + fresh-env-with-writeback で解決。

---

## 5. やるなら次にやるべきこと (priority)

### High (実用度大)
1. **Frozen check 残り mutator に展開** — 機械作業、半日
2. **Enumerator stand-in 強化** — `Array#each.to_a` 系を一通り通す
3. **Comparable mixin の派生メソッド網羅** — `<`, `<=`, `==`, `>`, `>=`, `between?`, `clamp` が user `<=>` から自動的に来ることを確認
4. **Object.ancestors** に `Kernel`, `BasicObject` を入れる
5. **Numeric#coerce protocol** — `1 + custom_numeric` を user の `coerce` 経由で

### Medium
6. **Custom class as Hash key** (`korb_hash_value` を CTX 取れる版に refactor)
7. **Method#parameters / Proc#parameters**
8. **Endless method def, numbered block param** (Ruby 3.x 構文)
9. **`Kernel#binding`** + `eval(str, binding)`
10. **`__send__` / `public_send` の visibility 区別**

### Low (大きい・別 project 待ち)
11. **Regexp** — sample/astrorge 完成後に integrate
12. **Encoding** — まずは UTF-8 aware に
13. **Thread** — 並行性が必要になったら
14. **Marshal** — シリアライゼーションが必要になったら
15. **Enumerator::Lazy** — 本物の lazy chain
16. **Real File / IO / Dir / Pathname** — fixture 読み書きを超える用途が出たら

---

## 6. 性能 (互換性とは別軸)

`docs/perf.md` 参照。要点:
- optcarrot は CRuby 比 2.2x、YJIT 比 0.55x
- 「10x 出てほしい」の声あり → abruby 流の split-kind PIC + PGC type specialization が必要
- 現状 AOT-cached が interp と同等になるのは type-specialized node が無いため

---

(過去の todo は git history で `git log -p docs/todo.md` から参照可能。今回 2026-05-02 に大規模 rewrite。)
