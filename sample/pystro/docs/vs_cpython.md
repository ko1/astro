# vs_cpython.md — pystro vs CPython 3.12 / 3.14 / +JIT 分析

2026-05-08 時点。 全数字は best-of-5 / `perf stat` /
`perf record -F 4000` で取得した実測値。

## 結論: 現状

### macro

| ベンチ | py3.12 | py3.14 | py3.14+JIT | pystro AOT | 結果 |
|---|---:|---:|---:|---:|---|
| **richards**     | 1.07 | 0.90 | 0.84 | **0.48** | pystro **1.75×〜2.23× FASTER** |
| **deltablue**    | 0.17 | 0.15 | 0.16 | **0.14** | pystro **1.07×〜1.21× FASTER** |
| **raytrace**     | 0.91 | **0.68** | 0.72 | 0.86 | py3.14 が **1.27× faster** |
| **crypto_pyaes** | 0.56 | 0.49 | 0.54 | **0.40** | pystro **1.23×〜1.40× FASTER** |

- vs CPython 3.12: pystro **4/4 全勝**
- vs CPython 3.14 (best of ±JIT): pystro **3/4** (raytrace で逆転される)

### micro (10 本)

micro は dict_bench を除いて 9/10 で pystro AOT 勝利、 これは 3.12 vs
3.14 vs +JIT どれを基準にしても変わらない。 詳細は
[`perf.md`](perf.md)。

### CPython 3.14 自体の改善

JIT 無しの 3.14 が 3.12 比で多くのベンチで速くなっている:

| 改善幅 | 該当 |
|---|---|
| **25% (raytrace)** | 0.91 → 0.68 — Tier 2 uops + Vector arithmetic 改善 |
| 16% (richards) | 1.07 → 0.90 |
| 32% (fib35) | 1.14 → 0.77 — recursive call の specialization |
| 22% (pyrange) | 2.33 → 1.81 |
| 13% (pyaes) | 0.56 → 0.49 |
| 12% (deltablue) | 0.17 → 0.15 |
| 中立〜微減 | dict_bench / list_bench / mandel / nqueens / while_loop |

主因は **PEP 744 の Tier 2 uops interpreter** (3.13 から)、
**adaptive specialization の拡充**、 frame 周りの最適化。 JIT (PEP 744
copy-and-patch) 自体は次節のとおり別話。

### CPython 3.14 +JIT は本ベンチ規模では負債

JIT 有効化で速くなったのは macro 4 本中 **richards のみ** (+7%)。
他は中立か悪化:

| ベンチ | py3.14 | +JIT | 効果 |
|---|---:|---:|---|
| richards | 0.90 | 0.84 | **+7%** |
| deltablue | 0.15 | 0.16 | -7% |
| raytrace | 0.68 | 0.72 | -6% |
| pyaes | 0.49 | 0.54 | -10% |

micro 10 本でも JIT が improvement なのは `recursive` (tak、 deeply
recursive) の +3% のみ。 残りは 0〜-19% で軒並み悪化。

**理由 (推測):**
- ベンチ規模 ~1s で JIT warmup コストを回収しきれない
- copy-and-patch JIT は trace の安定性 (= type の monomorphism) に
  依存していて、 short-running で type が落ち着く前に終わる
- Tier 2 uops interpreter 自体が既にかなり優秀で、 JIT との差を
  埋めにくい

長時間 (10s〜分単位) のワークロードや、 type が完全 monomorphic な
hot loop では JIT が効くと予想されるが、 pyperformance の macro
スコアでは現状 `--jit off` の方が良い結果。

## なぜ勝てるのか (pystro 側の武器)

### 共通要因

1. **AST direct dispatch + AOT SD 化** — 関数 body も含めて全 hot
   path が baked C code。 CPython の bytecode interpreter (Tier 1
   /Tier 2) が opcode dispatch を持つのに対し、 pystro の SD は
   通常の C 関数呼び出しの直線コード。 hot path の IPC は CPython
   側が上だが、 absolute work 量で勝つ。
2. **多層 IC** — method dispatch、 attr_get/set、 binop、
   instance class-attr に call-site polymorphic IC。 CPython の PEP
   659 specialised bytecode と同等か粗いが、 hit 時のコストは
   `pointer-compare → load → call` の 3 命令程度。
3. **dunder slot 化** — 24 種の dunder (`__init__/__eq__/...`) を
   pyclass struct の `slot_*` に pre-resolve。 pointer-compare で
   slot load → MRO walk + strcmp が消える。
4. **interned name** — `n->u.X.name` が parser 段階で intern pool に
   登録されており、 SD と runtime.c の `PYSTRO_INTERN_*` が同じ
   ポインタで pointer-compare 可能。
5. **fixnum / flonum tagged value** — 数値演算の hot path が pointer
   経由しない。 GMP は bignum overflow のときだけ。
6. **`fast_new` flag** — user class の `slot_new` が default
   `bi_object_new` で built-in base なし、 例外でなければ、
   instantiation で `__new__` dispatch 全スキップ。 alloc + `__init__`
   の 2 段になる。

### ベンチ別

#### richards (1.75〜2.23× faster vs CPython 各種)

OS scheduler simulation。 6 つの Task subclass を todo list で
混合 iteration。 全 method 呼び出しが 4-way PIC で命中、 attr_get/set
も 4-way PIC。 SD baked code が時間の 60%+ を占める。

CPython は (3.14 + JIT であっても) bytecode dispatch + frame setup +
specialization 各層にコストが乗る。 pystro はそれが直線 C コードに
展開済み。 これは **pystro 設計の本領**。

#### deltablue (1.07〜1.21× faster)

Constraint solver。 hot loop は `self.input().value = self.output().value`。
**parser の LHS fuse** が決定打 (Phase 8、 4× 飛躍)。 詳細は
[perf.md](./perf.md)。

3.14 が 12% 速くなったので 3.12 比 1.21× → 3.14 比 1.07× に縮んだが、
依然勝ち。 ただし差は小さく、 noise 圏内に入りつつある。

#### raytrace (1.27× SLOWER vs py3.14)

唯一の macro 負け bench。

3.14 自体が 25% も速くなった理由 (推測):
- Vector の `__add__/__sub__/__mul__/__truediv__/__neg__` が CPython
  3.14 の Tier 2 specialized で fold される
- 浮動小数の box/unbox を reuse する dataflow が pystro の inline
  flonum (CRuby 流) と同等以上
- per-Vector alloc が CPython の small-object arena で軽い

pystro 側で改善できる方向:
- minor operator dunder (`__truediv__`、 `__rmod__`、 `__neg__` etc.)
  を pyclass の slot 化。 主要 24 種は slot 化済みだが、 残り ~20 種が
  まだ MRO walk + strcmp に落ちる
- per-pixel の Vector alloc を pool 化 (毎回 GC_malloc しない)

#### crypto_pyaes (1.23〜1.40× faster)

AES-CTR 実装。 Phase 8 の最適化が効いた:
- bytes/list の `arr[i]` (S-box lookup) を fixnum-index fast path
- `i % 4` / `i // 4` を fixnum 直 op に
- **`_remaining_counter += [...]` を in-place extend に** (O(N²) → O(N))
- `aes.T1` / `self.S` のような class-level table 参照を `inst_ca_*`
  cache で memoize

残存ボトルネックは bytes copy/zero (memmove + memset = 24%) と GC alloc。
これらは libc + Boehm 層で SD から触れないが、 既に CPython 3.14 比で
1.23× 上回る。

## CPython の構造的優位 (vs pystro)

pystro より CPython が強い点:

1. **specialized bytecode (PEP 659 + Tier 2)** —
   `LOAD_ATTR_INSTANCE_VALUE` / `BINARY_OP_INT` / `CALL_FUNCTION_EX` 等。
   3.13 から Tier 2 uops で更に細粒度の specialization。 pystro の IC
   は AST node 単位で粗いが、 hit 時のコストは Tier 2 と同等以下
   (compare → load → call)。
2. **refcount + 小オブジェクト arena** —
   per-alloc bookkeeping が Boehm の 1/5。 raytrace / pyaes のように
   alloc が hot な benchmark で 5〜10% 差が出る。
3. **builtin 型の C-implemented dunder** —
   `int.__add__` / `bytes.__xor__` 等が ALL の path で C-inline。
   pystro は Phase 6 の bit op fixnum fast path で数値系は対応、
   bytes 系は Phase 8 で部分対応。
4. **string-key dict layout** —
   CPython の dict は str-key 専用 path で hash 事前計算済み + strcmp
   不要。 pystro は generic open-addressing 1 種のみ →
   `dict_bench` で 1.31× slow (唯一の負け micro bench)。
5. **C-level type slots (`tp_add` / `tp_init` 等)** —
   pystro の dunder slot は pyclass に inline 解決値を持つだけで、
   dispatch そのものは Python レベル。 CPython は slot を直接 C
   関数として呼ぶので関数呼出 overhead が無い。
6. **JIT の将来性** —
   現状 (3.14.4) では本ベンチ規模で win とは言えないが、 LLVM-driven
   copy-and-patch は long-running ワークロードで pystro の AST direct
   dispatch を超える可能性がある。 pystro 側にも JIT という手は残っ
   ている (未着手、 ASTro 全体方針と要相談)。

## pystro の構造的優位 (vs CPython)

逆方向。 CPython に無くて pystro にある武器:

1. **AST → C 直接コンパイル (AOT SD)** —
   bytecode interpreter loop なし、 opcode dispatch なし。
   Hot path が gcc -O3 -march=native で最適化された直線 C コード。
   この差は CPython が JIT を入れても完全には埋まらない (JIT-compiled
   code は dispatch overhead がゼロにはならず、 また trace の deopt
   コストもある)。
2. **AST node 単位の polymorphic IC** —
   bytecode 単位より粗いが、 各 IC slot は独立してチューニング可能
   (method PIC は `(cls, fn)`、 attr PIC は `(cls, eidx, sv)`、
   binop は `(cls, fn)`、 class-attr は `(cls, value, sv)`)。
3. **`shape_version` 経由の eidx 共有** —
   class shape が変わらない限り、 同 class の全 instance が同じ
   eidx で attr access。
4. **closure leaf の alloca フレーム** —
   leaf 関数は GC_malloc の代わりに alloca。 CPython の frame は
   freelist 経由だが pystro は完全 stack。
5. **interned-pointer name compare** —
   parser で全 name を intern pool に登録。 SD・runtime.c で同じ
   ポインタが流れるので、 dunder slot lookup は pointer-compare
   一発。

## 攻め所優先順 (現時点で残る gap)

`perf stat` の命令数差 + perf record の hot 関数 を組み合わせて
ROI 高い順:

1. **raytrace を 3.14 から取り戻す** —
   minor operator dunder (`__truediv__` / `__neg__` / `__rmod__` /
   `__matmul__` 等) を slot 化。 pyclass struct を肥らせない方法:
   slot を別 struct に切り出して pyclass→slot table へポインタ 1 個
   だけ持たせる pattern。 5〜15% 削減見込み。
2. **dict_bench を 1× に** —
   str-key 専用 dict layout を追加。 hash 事前計算 + strcmp 撤去。
   現状唯一の micro 負け。
3. **bytes 操作の C-inline (pyaes 残存)** —
   memmove / memset を撤去するには、 bytes XOR / slice / concat を
   Boehm を経由しない arena から確保する必要。 ASTro 全体で GC 方針
   (`idea.md`) を変えるタイミングと合わせる。
4. **JIT / type inference** —
   IPC を上げる方向。 ASTro 全体方針と要相談。 CPython 3.14 の JIT が
   現状 win でないことから、 pystro 側で焦って入れる必要は薄い。

詳細な実装履歴と各 phase の変化は [`perf.md`](perf.md) を参照。
