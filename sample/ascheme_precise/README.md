# ascheme_precise — R5RS Scheme on ASTro (precise GC version)

`sample/ascheme/` を fork し、 GC を **libgc (= Boehm conservative GC)** から
**ASTro precise GC framework** (`runtime/precise_gc/`) へ移行した版。

機能 (= R5RS Scheme インタプリタ、 AOT、 PGO) は ascheme と同等。 差分は
GC まわり:

- **17 個の GC backend を build-time 切替**: `make GC=copy`, `make GC=mark`,
  `make GC=copy_scramble` 等
- **precise rooting** — sample が自前で root を管理 (= sframe + sp scratch)
- **finalizer infra** — OBJ_BIGNUM / OBJ_RATIONAL の GMP buffer を
  `mpz_clear` で回収
- **audit knobs** — `BARUBY_GC_STRESS=1` (= 高頻度 GC) と
  `BARUBY_GC_PURGE=1` (= from-space munmap) で root tracking gap を検出

実装の詳細は [`docs/runtime.md`](./docs/runtime.md)、 移行経過は
[`docs/migration.md`](./docs/migration.md)、 perf 評価は
[`docs/perf.md`](./docs/perf.md) を参照。

## ascheme との関係

`sample/ascheme/` は libgc 版で安定実装。 ここ `ascheme_precise/` は precise
GC framework の testbed として fork。 言語仕様 / R5RS 互換 / AOT pipeline は
共通。 perf 比較は [`docs/perf.md`](./docs/perf.md) 参照 (= libgc baseline
あり)。

## ハイライト

- **完全な R5RS 数値タワー** — fixnum / bignum (GMP) / rational (GMP) /
  flonum (Ruby 流 inline 符号化) / complex。 `(+ 1/2 1/3)` は `5/6`、
  `(expt 2 100)` は exact bignum、 `(make-rectangular 3 4)` は `3+4i`。
- **末尾呼出最適化 + leaf-closure frame 再利用**
- **`call/cc`** (= one-shot downward escape continuation、 setjmp/longjmp 実装)
- **多値、 promise、 port、 quasiquote**
- **特化ノード** — `(+ a b)` `(< a b)` `(car x)` `(vector-ref v i)`
  `(null? x)` 等を専用ノードに、 R5RS の `(set! + my+)` 再定義検出付き
- **AOT** — ASTro 特化器で C 化、 `gcc -O3` で build → `dlopen`
- **PGO** — `--pg-compile` でホットエントリのみ AOT
- **17 GC backend** — copy / copy_gen / mark / mark_gen / mark_compact /
  mark_compact_gen / mark_bump_gen / mark_freelist / mark_bitmap_gen /
  mark_card_gen / mark_gen_inc / copy_gen_inc / immix / immix_gen / none /
  bump / copy_scramble
- **R5RS 互換** — chibi-scheme `tests/r5rs-tests.scm` を機械変換した
  179 件を 100% パス (= default mode)

## インストール

### 前提パッケージ (Ubuntu/Debian)

```sh
sudo apt install build-essential ruby libgmp-dev libreadline-dev
```

- `build-essential` — gcc / make
- `ruby` (3.x) — ASTroGen の実行
- `libgmp-dev` — bignum / rational (= libgc は **不要**、 precise GC が
  framework で provided)
- `libreadline-dev` — REPL の行編集 (auto-detect、 なくても build 可)

`make bench` で chibi-scheme / guile と比較するなら `chibi-scheme` /
`guile` も。

## ビルドと実行

```sh
make                           # default backend で build (= GC=none、 leak-as-go)
make GC=copy                   # Cheney semispace
make GC=mark                   # mark&sweep
make GC=copy_scramble          # audit backend (= mark/move 漏れ検出)
make test                      # 16 自前テスト + 179 R5RS chibi tests
make clean
```

CLI 規約は [`docs/sample_cli.md`](../../docs/sample_cli.md) 共通仕様に準拠
(= `--plain` / `--aot-compile` / `--pg-compile` / `--quiet` 等は framework
管理、 `-e` 等 sample 固有 flag は維持)。 詳細は `--help`:

```sh
./ascheme_precise --help
```

REPL:

```sh
$ ./ascheme_precise
ascheme> (define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))
ascheme> (fact 50)
30414093201713378043612608166064768844377641568960512000000000000
```

## GC backend の選び方

`make GC=<name>` で切替。 主な選択肢:

| backend | 特徴 | 用途 |
|---|---|---|
| `none` | 何もしない (= libc malloc、 leak) | bench baseline、 短時間 |
| `bump` | bump pointer のみ | bench baseline |
| `mark` | mark&sweep | シンプル、 全 workload PASS |
| `mark_gen` | gen mark&sweep | 短命 obj が多い workload |
| `copy` | Cheney semispace | balanced、 GC heavy で高速 |
| `copy_gen` | gen Cheney | 長期 + 短期 obj 混在 |
| `mark_compact_gen` | gen + sliding compact | 生存率高い workload |
| `mark_bump_gen` | nursery bump + tenured mark | mixed lifetime |
| `mark_freelist` | freelist 管理 | slow-allocator workload |
| `mark_bitmap_gen` | per-page bitmap | small-payload heavy |
| `mark_card_gen` | card-marking gen | card-table 実験 |
| `immix` | block / line mark-region | uniform、 fragmentation-resistant |
| `immix_gen` | gen immix | 大規模 |
| `copy_scramble` | per-cycle XOR scramble | **audit / debug** (= mark/move 漏れ検出) |

production 推奨: **`copy_gen`** または **`mark_compact_gen`**。
GC-light なら **`mark`** / **`mark_freelist`**。 perf 詳細は
[`docs/perf.md`](./docs/perf.md)。

## audit (= mark/move 漏れ検出)

precise rooting のバグを検出する仕組み 2 つ:

```sh
# 1. stress = GC trigger 点ごとに必ず GC 発火 (= 高頻度)
BARUBY_GC_STRESS=1 ./ascheme_precise script.scm

# 2. purge = Cheney 系で from-space を munmap (= stale ptr deref 即 SEGV)
BARUBY_GC_PURGE=1 ./ascheme_precise script.scm

# 3. scramble = VALUE storage を per-cycle XOR (= forget ARO_LOAD 検出)
make GC=copy_scramble && ./ascheme_precise script.scm

# 4. 全部組合せ = 最強 audit (= 旧 BARUBY_GC_STRESS 相当)
BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1 ./ascheme_precise script.scm
```

scramble の仕組みは [`../../docs/gc_design.md`](../../docs/gc_design.md)
§3.3 を参照。

## libgc との性能比較

[`docs/perf.md`](./docs/perf.md) で 9 workload × libgc baseline + 全 17
precise backend を実測:

- **GC-heavy workload で -7〜-42% 高速化** (= matmul -42%、 fannkuch -22%、
  deriv -18%、 sieve_big -7%)
- **整数 workload で fib35 のみ +110% overhead** (= 純再帰の sp[] 更新 cost)
- **GC-light な numeric workload** (= nbody) でも flonum 内挿 + 良 cache layout
  で **逆に -23% 速い** (= 0.41s vs libgc 0.53s)
- 全 9 workload geomean は `copy` / `copy_scramble` / `immix` で libgc と
  ~tie (= 1.00–1.02×)、 GC-heavy 5 workload に絞ると **0.81–0.87×** で勝つ

## 制限 / 非対応

ascheme 本家と同様 (= R5RS subset):
- `dynamic-wind` 未対応 (= `call/cc` は escape のみ)
- `syntax-rules` 未実装 (= `quasiquote` のみ reader / compiler 展開)
- 演算子の再定義は正しく動く (= 各特化ノード `arith_cache` の runtime check)

precise GC 固有の状況:
- **全 17 backend が default + stress mode で test suite (= 17 ascheme + 179
  R5RS) PASS** (= Phase 8 完了)。 過去は `mark_gen` / `mark_gen_inc` 等で
  root tracking gap があったが、 typed-ptr field の VALUE 化 + framework
  freelist encoding bug fix で全 17 PASS
- `mark_compact` のみ bench workload 3 個 (= nbody / fannkuch / matmul) で
  SEGV (= sliding-compact phase の edge case bug、 root tracking は OK)
- `mark_freelist` / `mark_bitmap_gen` / `mark_card_gen` の matmul は 60–100s
  outlier (= 外部 GMP buffer の external_bytes pressure と GC trigger 頻度の
  相性、 [`docs/perf.md`](./docs/perf.md) §7.2)

## ファイル構成

```
sample/ascheme_precise/
├── README.md             この文書
├── docs/
│   ├── runtime.md        実装詳細 (= 言語 pipeline + precise GC integration)
│   ├── spec.md           R5RS subset 言語仕様
│   ├── migration.md      libgc → precise GC framework migration 経過 + 教訓
│   └── perf.md           17 backend × libgc baseline の実測
├── context.h             VALUE / sobj / CTX / GC contract macros
├── node.h                NodeHead / NODE / EVAL macros
├── node.c                ランタイム配線
├── node.def              AST ノード定義 (40 種)
├── main.c                リーダ・コンパイラ・プリミティブ・ドライバ
├── parse.c               S-expression reader
├── ascheme_gen.rb        ASTroGen 拡張
├── Makefile              build / test / bench
├── test/                 16 件の自前テスト + chibi r5rs-tests 機械変換
├── bench/                bench scripts (= ascheme と共有)
├── code_store/           AOT 生成物 (gitignore)
└── .built_gc             current GC backend selection marker (gitignore)
```

## 関連 docs

- [`../../docs/gc_design.md`](../../docs/gc_design.md) — precise GC framework
  全体設計、 contract、 migration 教訓 (= ascheme の経験から書かれた §7.7
  あり)
- [`../../docs/sample_cli.md`](../../docs/sample_cli.md) — 共通 CLI 規約
- [`../../docs/idea.md`](../../docs/idea.md) — ASTro framework 設計思想
- [`../baruby_precise/`](../baruby_precise/) — 同 precise GC framework 上の
  Ruby サブセット実装 (= precise GC framework の reference sample)
