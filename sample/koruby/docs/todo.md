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

### 3.1 Frozen check
- 入ってる: `Array#<<`, `Array#push`, `Array#pop`, `Array#[]=`, `Hash#[]=`, `String#concat`
- 入ってない: `Array#unshift`, `Array#shift`, `Array#clear`, `Array#delete`, `Array#delete_at`, `Array#concat`, `Array#fill`, `Hash#delete`, `Hash#clear`, `Hash#merge!`, `String#prepend`, `String#insert`, `String#tr!`, `String#sub!`, `String#gsub!` 等
- 全 mutator に `CHECK_FROZEN_RET(c, self, Qnil)` を撒く必要

### 3.2 Object.ancestors
- `Object.ancestors` が `[Object]` のみ
- 期待: `[Object, Kernel, BasicObject]`
- `BasicObject` クラスがそもそも存在しない可能性

### 3.3 Numeric edge cases
- Bignum × Float × Rational mixed 演算の網羅未確認
- `1 ** -1` → Rational 期待だが Float 返る
- Integer#coerce, Numeric#abs2 未実装

### 3.4 Heredoc edge cases
- 基本 `<<~`, `<<-`, single-quoted heredoc は通る
- ネスト heredoc / 同行に複数 heredoc / heredoc 内の interpolation 込みの複雑形は未確認

### 3.5 Pattern match (`case/in`)
- 基本パターン (literal / array / hash) は通る
- `^x` (pin operator), `=>` への local bind, `[*a, x, *b]` の split, find pattern (`[*, x, *]`) 等は不確か

### 3.6 method_defined? 系
- `method_defined?(:foo)` 動く
- `private_method_defined?` / `public_method_defined?` / `protected_method_defined?` 未確認
- `instance_methods(false)` (own only flag) の挙動確認必要

### 3.7 Ruby 3.x 構文
- Endless method `def f(x) = x+1` — 未確認
- Numbered block param `_1`, `_2` — 未確認
- Hash shorthand `{x:, y:}` (Ruby 3.1+) — 未確認
- Pattern `=>` (rightward assignment) — 未確認

### 3.8 Backtrace
- `e.backtrace` 動くが内容が CRuby と異なる
- `Kernel#caller` も簡易版

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
