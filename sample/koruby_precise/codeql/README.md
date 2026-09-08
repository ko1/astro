# koruby_precise — CodeQL GC-safety / encapsulation gate

Static checks that keep koruby's **moving-GC pointer rules** enforced.  koruby's
raw payload buffers (`KorbStrBuf` / `KorbArrayItems`) move when the GC runs, so a
raw pointer into them is only valid until the next allocation.  These queries,
plus the `ARO_BORROW` accessor discipline, make that safe by construction.

Design & rationale: [`../docs/c_ext_api_design.md`](../docs/c_ext_api_design.md) §4.1.

## Run it after changes

```sh
make codeql-check        # = sh codeql/run.sh   (~100 s warm, 4–7 min cold)
```

Each rule is **self-tested on a fixture** (so a broken query can't silently pass)
and then **required clean on the real koruby build**.  Non-zero exit on any
failure.  Needs the CodeQL CLI (`gh extension install github/gh-codeql`);
`codeql pack install` and the DBs (under `codeql/.db/`, gitignored) are handled
automatically.

### Where the time goes

| step | time |
|---|---|
| koruby DB (make clean + full rebuild under CodeQL tracing) | ~37 s |
| fixture DBs ×3 | ~3 s each |
| query evaluation, per query (koruby DB, warm) | ~6–7 s |
| whole gate, warm (5 koruby runs + 4 self-tests) | **~100 s** |
| first run / empty `.cache` (adds ~20 s query compilation each + pack install) | 4–7 min |

Per-query cost is flat (~6–7 s regardless of rule complexity — dominated by
evaluator startup + DB load, not query logic), and the koruby DB rebuild is a
fixed cost independent of rule count.  **Adding a rule costs ~+6 s warm**, so
the barrier to new rules is low.  Query compilation is cached under `.cache/`
next to the pack; only cold runs pay it.

## The `ARO_*` attributes (`runtime/aro_gc_effect.h`)

Markers that expand to nothing under gcc (the queries key on the *macro
invocation*, so there is no unknown-attribute warning):

| macro | meaning |
|---|---|
| `ARO_BORROW` | this function hands out / returns a raw pointer into a movable GC object.  **Only** `ARO_BORROW` functions may reach into the raw payload. |
| `ARO_MAYGC` | this function may trigger a GC (reserved for the may-gc effect layer). |
| `ARO_NOGC`  | this function must not trigger a GC (reserved). |

The payload fields are named `data_priv` so that any direct access **outside an
accessor is a compile error** — the compiler is the primary spatial guard, and
the `interior_encapsulation` query is the CodeQL backstop.

## The rules (all currently clean)

| query | kind | what it enforces |
|---|---|---|
| `borrow_after_gc.ql` | **temporal (pointer)** | A raw borrow (an `ARO_BORROW` accessor's return, or a `->data`/`->data_priv` field on `KorbStrBuf`/`KorbArrayItems`) held in a local and **used after a may-GC call** is stale under the moving GC → error.  SSA-precise: re-deriving the pointer (a new SSA def) is treated as safe, so the re-derive-each-iteration idiom does not false-positive.  Follows the borrow through conversions, pointer arithmetic, `&elem`, and local aliasing. |
| `value_after_gc.ql` | **temporal (VALUE)** | The VALUE companion to `borrow_after_gc`: a `VALUE` produced by a may-GC call (a potentially movable heap object) held in a plain C local and **used after another may-GC call** is stale — the moving GC updates rooted slots (`slots[]`, `VALUE_REF` cells) but not a bare local → error.  The safe idiom stages into `slots[]` (an array element, not a `StackVariable`) and re-reads it, which is not flagged; a re-read into a local (`v = slots[i]` again) is a fresh SSA def, also safe.  Follows the VALUE through conversions and local aliasing. |
| `interior_encapsulation.ql` | **spatial** | Direct access to a raw payload field (`KorbStrBuf`/`KorbArrayItems` `::data`/`::data_priv`) **outside an `ARO_BORROW` function** → warning.  Backstops the compiler (field rename): all interior access must go through the accessor chokepoint, so the representation can change by editing accessors alone. |
| `borrow_escape.ql` | **escape** | A raw borrow that **escapes a non-`ARO_BORROW` function** — returned from it, or stored into a struct field / global — hands the caller a borrow without its lifetime → warning.  Fix: mark the function `ARO_BORROW` (if it is deliberately an accessor) or copy the bytes out. |
| `aro_borrow_unused.ql` | **hygiene** | A function marked `ARO_BORROW` whose body **touches no raw payload and calls no accessor** — the annotation is a lie that needlessly exempts it from `interior_encapsulation` and makes `borrow_after_gc` treat its return as a borrow (false positives on callers) → warning.  Fix: remove `ARO_BORROW`. |
| `maygc.ql` | helper | Infers the may-gc effect of every function by transitive closure over direct calls from the single seed `korb_alloc` (the only GC publish point).  Used by `borrow_after_gc` to decide what a "may-GC call" is; not a pass/fail gate. |

## Fixtures (query self-tests)

- `test/borrow_cases.c` — 5 true positives (linear hold / loop-carried hold /
  `&data[i]` / alias / via-accessor) + 3 true negatives (use-before-gc /
  no-gc-between / re-derive-loop) for `borrow_after_gc`.
- `test/value_cases.c` — 3 true positives (local held across may-GC / held
  across a second producer / alias) + 4 true negatives (no-gc-between /
  staged-in-`slots[]` / re-read-from-slot / consumed-as-argument) for
  `value_after_gc`.
- `test/param_cases.c` — the blind spot `value_after_gc` has (see below); not
  part of the gate.
- `test/annotation_cases.c` — one escape + one unused-annotation case for
  `borrow_escape` / `aro_borrow_unused`.
- `test/encapsulation_cases.c` — direct-access-outside-accessor case for
  `interior_encapsulation`.

## Known blind spot: a VALUE that arrives as a parameter

`value_after_gc.ql` only tracks a VALUE whose defining value is a may-GC call
*inside the same function* (`VALUE v = korb_str_new(...)`).  A VALUE the caller
passed in is never seeded, so a parameter held across a may-GC call is not
flagged.  That is exactly the shape of the 2026-08-19 `korb_re_str_span` SEGV
(`group_or_nil` was live across the match run, which allocates).

Measured on `test/param_cases.c` (2026-08-19): `value_after_gc.ql` reports only
the local case, `value_param.ql` (an unwired experiment kept beside it) reports
the parameter case and stays quiet on the slot-parked one.  `value_param.ql` is
NOT in the gate: run against the real koruby DB it did not finish within 9
minutes — seeding on every VALUE parameter makes the `reach` recursion blow up.
Closing the gap needs a narrower seed (e.g. only parameters of functions that
themselves write `slots[]`), not just the query as it stands.

## Files

```
qlpack.yml                  CodeQL pack (deps: codeql/cpp-all)
run.sh                      the gate (invoked by `make codeql-check`)
cqbuild.sh                  clean, ccache-disabled build for DB extraction
borrow_after_gc.ql          temporal check (raw pointer)
value_after_gc.ql           temporal check (bare VALUE)
value_param.ql              EXPERIMENT: same for VALUE parameters (not in the gate)
interior_encapsulation.ql   spatial check
borrow_escape.ql            escape check
aro_borrow_unused.ql        annotation-hygiene check
maygc.ql                    may-gc inference helper
test/                       fixtures
```


## 6. unsequenced-gc-arg (`unsequenced_gc_arg.ql`)

**一つの引数リストに、GC を起こしうる呼び出しと、GC が動かす VALUE の読みが
同居していないか。** C は引数の評価順を規定しないので、VALUE の読みが先に
起きうる。そうなると移動前のアドレスが渡り、**誰も直さない** — ルート走査が
更新するのはスロットであって、すでに取られたコピーではないから。

これは新しい規則ではなく、既存の「確保をまたいだら再読み込み」を、名前付き
ローカルが無いために寿命が見えない場所から見たもの。だから
`value_after_gc.ql` (StackVariable を鍵にする) では原理的に見えない。

順序こそ標準が開けている部分なので、このクエリは制御フローの順序を一切見ず、
**同居**だけを問う。

実例: `File::NULL` の owner (`builtins/file.c`)。2026-09-07 に修正。stale な
owner が `vm->const_owners` に焼かれ、後に `Set` の ancestor 配列の途中に
偽ヘッダを書き込んでいた。修正前のコードでこのクエリを流すと 4 件出て、修正後は
0 件になることを確認済み。

自己テスト: `test/unseq_cases.c` (BAD 3 / GOOD 4)。

## 7. value-read-after-gc (`value_read_after_gc.ql`) — ラチェット

`value_after_gc.ql` が追うのは **may-GC 呼び出しが「作った」** VALUE だけ。
実際に多いのは、スロット・引数・構造体フィールドから **「取り出しただけ」** の
VALUE を GC 後に使う形で、そちらは検出されていなかった (`test/gap_cases.c` を
`value_after_gc.ql` に流すと 0 件)。このクエリは源を「定数でない任意の VALUE 式」
まで広げる。

**これだけはゼロにできない。** 静的には即値 (絶対に動かない) と heap オブジェクトを
区別できず、実際に GC する経路かどうかも分からないため。`VRAG_BASELINE` として
run.sh に置き、**増えたら落ちる**ようにしてある。

**件数は use の数なので、そのまま「箇所の数」ではない。** 最初の実測は 189 行
だったが、(ファイル, 変数, 定義行) で束ねると **36 箇所**しかなく、しかも
`korb_send_impl` の `self` (定義行 9056) 1 個で **118 行 = 62%** を占めていた。
そしてそれは真陽性だった:

```c
if (UNLIKELY(vm->refinements_active)) {
    RESULT rr;
    if (korb_refined_dispatch(...)) return rr;   /* 扱ったら return */
    /* ← 扱わなかったら落ちてくる。ここで self を読み直していなかった */
}
if (KORB_OBJECT_P(self) && ...)                  /* 以降ずっと self を使う */
```

`korb_refined_dispatch` は `korb_dispatch_method` を呼ぶので Ruby を走らせうる。
同じ関数の少し上には `#to_str` ディスパッチ後の読み直しが既にあり、こちらだけ
抜けていた。`vm->refinements_active` のときだけなので踏まれていなかった。
1 行足して **189 → 71 行**。

仕分けの手順としては、行ではなく (ファイル, 変数, 定義行) で束ねてから
上から見るのがよい。

自己テスト: `test/gap_cases.c` (BAD 2 / GOOD 2)。

### 指摘を閉じる: `v = KORB_NOT_REF(v);`

この指摘が偽陽性になるのは「ここには参照が来ない」ときだけなので、それを
may-GC 呼び出しの**直前**に書く。クエリは `korb_not_ref` から来た定義を
「動かない」として扱い、その指摘を落とす — ベースラインの数字として残さない。
`-DKORB_STALE_CHECK` ビルドでは同じ呼び出しが実行時に主張を検査し、**GC 無しで**
heap 値の到達だけで反証される。

**文の形ではなく代入**にすること、`korb_not_ref` を**全ビルドで実関数**にすることの
2 点が要る (この DB は release ビルドから作られるので、消えるマクロは見えない)。
詳細と実測は [../docs/done.md](../docs/done.md) の 2026-09-07 節。
