# vs_cpython.md — pystro vs CPython 3.12 の現状分析

2026-05-08 時点。 全数字は best-of-5 / `perf stat` /
`perf record -F 4000` で取得した実測値。

## 結論: 現状

| ベンチ | py3 sec | pystro sec | py3 instr | pystro instr | inst比 | 結果 |
|---|---:|---:|---:|---:|---:|---|
| **richards** | 1.07 | 0.49 | 12.4 B | 5.2 B | 0.42× | **2.18× FASTER** |
| **crypto_pyaes** | 0.54 | 0.37 | 7.0 B | ~5 B | 0.71× | **1.46× FASTER** |
| **deltablue** | 0.17 | 0.14 | 2.0 B | ~1.6 B | 0.80× | **1.21× FASTER** |
| **raytrace** | 0.89 | 0.84 | 10.1 B | ~9 B | 0.89× | **1.06× FASTER** |

全 4 macro で命令数も python3 を下回る (= やる仕事が少ない)。 IPC は
CPython がやや上 (bytecode dispatch の予測しやすさ) だが、 pystro 側で
absolute work を減らした結果が時間差を生んでいる。

## なぜ勝てるのか

### 共通要因

1. **AST direct dispatch + AOT SD 化** — 関数 body も含めて全 hot
   path が baked C code。 CPython の bytecode interpreter が
   1 命令ごとに opcode dispatch するのに対し、 pystro の SD は
   通常の C 関数呼び出しの直線コード。
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

#### richards (2.18× faster)

OS scheduler simulation。 6 つの Task subclass を todo list で
混合 iteration。 全 method 呼び出しが 4-way PIC で命中 (richards は
6 subclass を超えるが、 hot loop 内では 4-way で足りる)。 attr_get/set
も 4-way PIC。 SD baked code が時間の 60%+ を占める。

CPython は bytecode dispatch + frame setup + PEP 659 specialization
それぞれにコストが乗る。 pystro はそれが直線 C コードに展開済み。

#### crypto_pyaes (1.46× faster)

AES-CTR 実装。 Phase 8 の最適化が効いた:
- bytes/list の `arr[i]` (S-box lookup, AES round) を fixnum-index
  fast path で直 access
- `i % 4` / `i // 4` を fixnum 直 op に
- **`_remaining_counter += [...]` を in-place extend に** (O(N²) → O(N)、
  memmove 12% → 2.5%)
- `aes.T1` / `self.S` のような class-level table 参照を
  `inst_ca_*` cache で memoize (py_class_lookup_method_slow 9% を撤去)

残存ボトルネックは bytes copy/zero (memmove + memset = 24%) と GC alloc。
これらは libc + Boehm 層で SD から触れないが、 既に python3 比で速い。

#### deltablue (1.21× faster)

Constraint solver。 hot loop は `self.input().value = self.output().value`。
**parser の LHS fuse** が決定打:

- 修正前: LHS の `self.output()` が `node_call_0(node_attr_get(self, "output"))`
  で、 毎回 bound method alloc + `py_apply_slow` 経由
- 修正後: LHS も RHS と同じ `node_method_0(self, "output")` に fuse
  → 4-way PIC + closure fast path が両側で効く

加えて `pyclass.fast_new` で Constraint subclass instantiation
(`__new__` dispatch) を skip、 classmethod IC で `Strength.weakest_of`
等の chain を IC 化。

#### raytrace (1.06× faster)

Vector arithmetic + ray-sphere intersection。 Phase 5〜7 で:
- Vector の `__add__/__sub__/__mul__` は per-call-site `binop_cache`
- attr_set new-instance fast path で `Vector(x,y,z)` の `__init__`
  経路で descriptor 確認撤去
- `math.sqrt(x)` 等の module method を IC 化
- `pyclass.fast_new` で Vector instantiation の `__new__` skip

辛勝。 残存は `__truediv__`、 `__neg__` 等の minor dunder の linear
MRO walk + strcmp。 これを slot 化すればさらに数 % 削れる見込み。

## 共通の構造的優位 (vs CPython)

CPython 3.12 にあって pystro に有利な要素:

1. **specialized bytecode (PEP 659)** —
   `LOAD_ATTR_INSTANCE_VALUE` / `BINARY_OP_INT` / `CALL_FUNCTION_EX` 等。
   pystro の IC は AST node 単位で粗いが、 hit 時のコストは PEP 659 と
   同等以下 (compare → load → call)。
2. **refcount + 小オブジェクト arena** —
   per-alloc bookkeeping が Boehm の 1/5。 pyaes / raytrace のように
   alloc が hot な benchmark で 5〜10% 差が出る (が、 pystro は他で
   稼いでいる)。
3. **builtin 型の C-implemented dunder** —
   `int.__add__` / `bytes.__xor__` 等が ALL の path で C-inline。
   pystro は Phase 6 の bit op fixnum fast path で数値系は対応、
   bytes 系は Phase 8 で部分対応。
4. **string-key dict layout** —
   CPython の dict は str-key 専用 path で hash 事前計算済み + strcmp
   不要。 pystro は generic open-addressing 1 種のみ →
   `dict_bench` で 1.27× slow (唯一の負け)。
5. **C-level type slots (`tp_add` / `tp_init` 等)** —
   pystro の dunder slot は pyclass に inline 解決値を持つだけで、
   dispatch そのものは Python レベル。 CPython は slot を直接 C
   関数として呼ぶので関数呼出 overhead が無い。

## pystro の構造的優位 (vs CPython)

逆方向。 CPython に無くて pystro にある武器:

1. **AST → C 直接コンパイル (AOT SD)** —
   bytecode interpreter loop なし、 opcode dispatch なし。
   Hot path が gcc -O3 -march=native で最適化された直線 C コード。
2. **AST node 単位の polymorphic IC** —
   bytecode 単位より粗いが、 各 IC slot は独立してチューニング可能
   (method PIC は `(cls, fn)`、 attr PIC は `(cls, eidx, sv)`、
   binop は `(cls, fn)`、 class-attr は `(cls, value, sv)`)。
3. **`shape_version` 経由の eidx 共有** —
   class shape が変わらない限り、 同 class の全 instance が同じ
   eidx で attr access。 CPython の dict resize は eidx を保証
   しない (ので毎回 hash + index lookup が必要)。
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

1. **dict_bench を 1× に** —
   str-key 専用 dict layout を追加。 hash 事前計算 + strcmp 撤去。
   現状唯一の負け bench。
2. **operator dunder 残り** —
   `__truediv__` / `__rmod__` / `__matmul__` 等を slot 化。 pyclass
   struct を肥らせない方法: slot を別 struct に切り出して
   pyclass→slot table へポインタ 1 個だけ持たせる pattern。
   raytrace でさらに 5% 程度削れる見込み。
3. **bytes 操作の C-inline** —
   pyaes の memmove / memset を撤去するには、 bytes XOR / slice / concat
   を Boehm を経由しない arena から確保する必要。 ASTro 全体で GC
   方針 (`idea.md`) を変えるタイミングと合わせる。
4. **JIT / type inference** —
   IPC を上げる方向。 ASTro 全体方針と要相談。

詳細な実装履歴と各 phase の変化は [`perf.md`](perf.md) を参照。
