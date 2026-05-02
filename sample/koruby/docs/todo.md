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

### 2.1 Enumerator / Enumerator::Lazy  ✅ 2026-05-02 部分完
- 完: no-block 形 (each / map / select / reject / each_with_index /
  reverse_each / Range#each / Range#map) は **Array stand-in** を返し、
  `.to_a` / 後続 `.map { }` / etc. が機能
- 未: 真の `Enumerator::Lazy` (`(1..Float::INFINITY).lazy.map.first(N)`)
- 未: `Enumerator.new { |y| y << ... }`
- 影響: 普通の `.each.to_a` は動く。 無限 lazy が必要なコードのみ落ちる

### 2.2 IO / File / Dir
- `File.read(path)` / `File.open` 程度のみ
- `IO.read` / `IO.write` / `IO.pipe` / `IO.select` / `STDIN.gets` 限定
- `Dir.glob` / `Dir.entries` / `Dir.chdir` 無し
- `Pathname` モジュール無し

### 2.3 Marshal / シリアライゼーション
- `Marshal.dump` / `Marshal.load` 無し
- `JSON` / `YAML` 無し (gem も無し)
- `Object#to_yaml` 無し

### 2.4 ObjectSpace / 内省  ✅ 2026-05-02 部分完
- 完: `ObjectSpace.count_objects` (Boehm GC_get_heap_size 経由の :TOTAL)、
  `garbage_collect`、 stub `each_object` (yields nothing — Boehm に live-object
  enum API 無し)
- 完: `Method#parameters` / `Proc#parameters` ([[kind], ...]、 names は無し)
- 未: `Kernel#binding` → `eval(str, binding)` 不可
- 未: `Proc#source_location` / `Method#source_location`

### 2.5 Complex / Rational
- リテラル `1r` / `1+2i` 未対応
- `Rational` / `Complex` クラスは stub (bootstrap.rb で簡易実装)
- Numeric#coerce の rational/complex 経路は coerce protocol で動くが、Rational
  自体の演算が脆弱

### 2.6 Comparable on user Numerics  ✅ 2026-05-02 完
- `1 + my_num` で my_num.coerce(1) を呼ぶ protocol 実装
- bad return / no method の場合 fall through → TypeError raise

### 2.7 Hash key with custom #hash / #eql?  ✅ 2026-05-02 完
- `korb_vm->current_ctx` を main.c で set
- `korb_hash_value` が user の `#hash` を呼ぶ (T_OBJECT で Object/Kernel
  以外の class が override してれば)
- `korb_eql` も `#eql?` を delegation
- `class Pt; def hash; ...; end; def eql?(o); ...; end; end; h[Pt.new(1,2)] = ...; h[Pt.new(1,2)]` 動く

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
1. ~~Frozen check 残り mutator に展開~~ ✅ 完
2. ~~Enumerator stand-in 強化~~ ✅ 完
3. ~~Object.ancestors に Kernel, BasicObject を入れる~~ ✅ 完
4. ~~Numeric#coerce protocol~~ ✅ 完
5. **Comparable mixin の派生メソッド網羅再検証** — `<`, `<=`, `==`, `>`,
   `>=`, `between?`, `clamp` が user `<=>` から自動的に来ること
6. **Kernel#binding** + `eval(str, binding)` — caller の locals を見せる
   binding object

### Medium
7. ~~Custom class as Hash key~~ ✅ 完
8. ~~Method#parameters / Proc#parameters~~ ✅ 完
9. ~~Endless method def, numbered block param, hash shorthand~~ ✅ 完
10. **`__send__` / `public_send` の visibility 区別** — public_send は
    private を block するべき
11. **Method#source_location / Proc#source_location** — file:line を
    返す。 require して定義されたメソッドの位置情報

### Low (大きい・別 project 待ち)
12. **Regexp** — sample/astrorge 完成後に integrate
13. **Encoding** — まずは UTF-8 aware に
14. **Thread** — 並行性が必要になったら
15. **Marshal** — シリアライゼーションが必要になったら
16. **Enumerator::Lazy** — 本物の lazy chain (`(1..Float::INFINITY).lazy`)
17. **Real File / IO / Dir / Pathname** — fixture 読み書きを超える用途が出たら
18. **Rational / Complex 演算の網羅** — bootstrap.rb の薄い実装を厚くする

---

## 6. 性能 (互換性とは別軸)

`docs/perf.md` 参照。要点:
- optcarrot は CRuby 比 2.2x、YJIT 比 0.55x
- 「10x 出てほしい」の声あり → abruby 流の split-kind PIC + PGC type specialization が必要
- 現状 AOT-cached が interp と同等になるのは type-specialized node が無いため

---

(過去の todo は git history で `git log -p docs/todo.md` から参照可能。今回 2026-05-02 に大規模 rewrite。)
