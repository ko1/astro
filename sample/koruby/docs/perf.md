# perf.md — koruby 性能改善の記録 (成功 / 失敗)

本書は **どんな最適化を試したか** と **その結果** を一覧する。
成功例だけでなく **見送ったもの** も同じ重みで記録する (再評価のために)。

## ベンチマーク環境

- CPU: x86_64
- OS: Linux 6.8 (Ubuntu 24.04)
- コンパイラ: gcc 13.3 (-O2 / -O3)
- Ruby (比較対象): CRuby 3.4 系 (no-JIT / `--yjit`)

## 2026-05-03 コンパイラ別 optcarrot bench (180 frames, best/3)

System: ruby 4.0.2 +PRISM, x86_64 Linux 6.8, koruby HEAD `04553d7`
optcarrot: `sample/abruby/benchmark/optcarrot/bin/optcarrot-bench`
checksum: 59662 (ruby と全 koruby ビルド一致 ✅)

### Baseline

| runner | fps |
|---|---:|
| ruby (no JIT) | 42.30 |
| ruby --yjit | 177.97 |

### koruby interp / AOT (compiler matrix)

`-O3 -flto=auto` baseline; AOT additionally compiles each SD_*.c with
`-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`.

| compiler | -O2 interp | -O3 interp | AOT (-O3) | AOT speedup |
|---|---:|---:|---:|---:|
| gcc-13 | 46.36 | 50.63 | **100.96** | 1.94× |
| gcc-14 | 48.00 | 50.73 | 99.12 | 1.95× |
| gcc-15 | 45.43 | 51.62 | 97.21 | 1.94× |
| gcc-16 | 46.59 | 50.90 | 99.77 | 2.05× |
| clang-17 | 48.95 | 47.27 | 89.41 | 1.92× |
| clang-18 | 47.84 | 49.90 | 90.78 | 1.88× |
| clang-19 | 49.61 | 49.62 | 88.31 | 1.84× |
| clang-20 | 50.11 | 50.66 | 92.32 | 1.85× |
| clang-21 | 49.26 | 49.35 | 90.82 | 1.92× |

### gcc-16 PGO 効果

| variant | fps |
|---|---:|
| gcc-16 -O3 interp | 50.90 |
| gcc-16 PGO interp | 52.88 |
| gcc-16 -O3 AOT | 99.77 |
| gcc-16 PGO + AOT | 100.75 |

### 観察

- **interp**: clang-20 (-O2: 50.11) と gcc-15 (-O3: 51.62) がほぼ同着
- **AOT**: gcc 系全員 ~100 fps、 clang は ~90 fps で頭打ち。
  AOT-emitted SD_*.c には @noinline / 局所 DISPATCH 構造が多く、 gcc の方が
  アグレッシブに inline + clone してくれているように見える
- AOT speedup は ~2× (interp の 50 → AOT の 100)
- PGO 追加効果は ~1 fps と限定的 — AOT 化された SD_*.c がホットパスを
  取り切るので、 PGO で main 側の dispatch を絞ってもほぼ無関係
- vs ruby (no JIT): koruby AOT は **2.4× 速い**
- vs ruby --yjit: yjit が **1.76× 速い** (koruby AOT は yjit の 0.57×)

---

## 2026-05-03 拡張スイープ: コンパイラ × フラグ ~100 通り

`tools/bench-matrix.sh` で系統的に全コンパイラ × 主要フラグを総当たり後、
top の 14 + cross-compiler 8 を `RUNS=10 FRAMES=300` で再測 (validate)。
checksum 検証付き、 `taskset -c 0`。

**ルール**: AOT all.so は `-fno-lto` 固定 (LTO は実用 PGO とぶつかる)。
koruby 本体は通常 `optflags=-O3 -flto=auto` (Makefile デフォルト)。

### 最終ランキング (gcc-15 系のフラグ・スイープ, RUNS=10 FRAMES=300)

| AOT 追加フラグ                              | best fps | median fps | size  |
|---------------------------------------------|---------:|-----------:|------:|
| `-O2 -march=native` (baseline)              |  106.32  | **105.61** | 3.5MB |
| `-O2 -march=native -Wl,-O3`                 |  106.24  |   105.17   | 3.5MB |
| `-O2 -march=native -fipa-cp-clone`          |  106.04  |   105.14   | 3.5MB |
| `-O2 -march=native -fno-tree-vectorize`     |  106.27  |   105.02   | 3.6MB |
| `-O2 -march=native -fno-stack-protector`    |  105.92  |   104.82   | 3.5MB |
| `-O2 -march=native -fuse-ld=gold`           |  105.39  |   104.79   | 3.5MB |
| `-O2 -march=native --param=max-inline-insns-auto=300` | 105.00 | 104.52 | 3.5MB |
| `-O2 -march=native -Wl,--gc-sections`       |  105.49  |   104.37   | 3.5MB |

**結論**: ベース `-O2 -march=native` がそのまま最強。 試した advanced
フラグ (`-fno-tree-vectorize`, `--param=max-inline-insns-auto=*`,
`-fipa-pta`, `-fipa-cp-clone`, `-funroll-loops`, `--param=inline-unit-growth=*`,
`-fno-stack-protector`, `-Wl,-O3`, `-Wl,--gc-sections`, `-fuse-ld=gold/lld`,
`-Wl,--icf=all`, ...) はどれもベース ±1 fps、 median ベースで **全敗**。

### Cross-compiler / -O level (gcc-15 vs 13/14/16, vs clang-21)

| compiler  | AOT flags             | best fps | median fps | size  |
|-----------|-----------------------|---------:|-----------:|------:|
| gcc-15    | -O2 -march=native     |  105.72  | **104.79** | 3.5MB |
| gcc-15    | -O2 (no -march)       |  105.01  |   104.01   | 3.4MB |
| gcc-13    | -O2 -march=native     |  104.58  |   103.73   | 3.3MB |
| gcc-14    | -O2 -march=native     |  103.80  |   102.35   | 3.4MB |
| gcc-15    | -O3 -march=native     |  102.37  |   100.48   | 4.0MB |
| clang-21  | -O3 -march=native     |   95.50  |    94.41   | 3.8MB |
| clang-21  | -O2 -march=native     |   95.56  |    93.45   | 3.8MB |
| gcc-15    | -Os -march=native     |   85.52  |    84.94   | 2.2MB |

(gcc-16 baseline は別 run で 104.31; 今回スイープでは gcc-15 とほぼ同等。)

### 重要な観察

1. **gcc-15 が最強。 gcc-14 で軽い回帰 (-2 fps median)**、 gcc-16 で持ち直すが
   gcc-15 を超えない。 gcc-13 は gcc-15 の -1 fps 程度。
2. **AOT は `-O2` が `-O3` より速い**: median で +4 fps、 size 3.5MB → 4.0MB。
   AOT-emitted SD_*.c は既に手で `@noinline` + 局所 DISPATCH で
   構造化されており、 `-O3` の追加 inline / vectorize は icache 圧を
   増やすだけ。
3. **clang は gcc から ~10 fps 離される** (`-O2`/`-O3` どちらでも)。
   AOT-emitted の cold/hot dispatch パターンに対する code-gen で
   gcc が優位 (おそらく switch-table と局所 inline のバランス)。
4. **`-march=native` の効果は ~1 fps** (gcc-15 比較で 104.01 → 104.79)。
   tight loop は SSE 程度しか触っていないので AVX も大差ない。
5. **linker option / advanced -f フラグはほぼ全敗**。 `-Wl,-O3`,
   `--gc-sections`, `-fuse-ld=gold/lld`, `-Wl,--icf=all` も baseline と
   同等以内。 `-fno-tree-vectorize` も僅差で負ける (validate run 時点では)。
6. **-Os は破壊的 (-20 fps)** — size 2.2MB と小さくなるが hot path が
   inline されず壊滅。

### 採用設定 (推奨)

```sh
# koruby 本体 (Makefile デフォルトでよい)
CC=gcc-15 optflags="-O3 -flto=auto" make

# AOT (all.so + 各 SD_*.so)
CC=gcc-15 \
CFLAGS="-O2 -march=native -fPIC -fno-plt -fno-semantic-interposition" \
LDFLAGS="-fno-lto" \
./koruby --aot-compile <prog>
```

`tools/bench-matrix.sh` は今後の compiler 探索を再現可能にするため
スクリプトを `sample/koruby/tools/` に commit 済 (cf. `bench-matrix.sh`,
`bench-summary.sh`, `bench-validate.sh`)。

### 真 PGO (koruby 本体 instrument → optcarrot 60f profile → 再リンク)

`tools/bench-pgo.sh` で `make koruby-pgo` (Makefile に既存) を回した結果、
RUNS=10 FRAMES=300:

| compiler  | koruby PGO | AOT flags          | best fps | median fps | size  |
|-----------|------------|--------------------|---------:|-----------:|------:|
| gcc-15    | -O2 + PGO  | -O2 -march=native  |  107.62  | **106.71** | 3.5MB |
| gcc-15    | -O3 + PGO  | -O2 -march=native  |  107.00  |   106.51   | 3.5MB |
| gcc-16    | -O2 + PGO  | -O2 -march=native  |  106.78  |   105.61   | 3.7MB |
| gcc-13    | -O2 + PGO  | -O2 -march=native  |  102.87  |   102.15   | 3.4MB |
| gcc-15    | -O2 + PGO  | -O3 -march=native  |  102.36  |   101.38   | 4.0MB |

**PGO 効果**: gcc-15 baseline 比で median **+1.1 fps (+1.0%)**。
小さいが安定的。 hot dispatch loop の branch hint と inline 判断が
profile-guided で改善される。 AOT 化されてもなお koruby 本体の dispatch
コードは大半触られるため効く。

PGO 下でも `-O2 koruby + -O2 -march=native AOT` が最強。 `-O3` は
ここでも regression、 gcc-13 は -4 fps の大きな差。

### AOT-level PGO (all.so 自体を profile-guided でリビルド)

`tools/bench-aot-pgo.sh` で 3-pass を実装:
1. koruby を普通にビルド
2. all.so を `-fprofile-generate=$DIR` で instrument 付きでビルド
3. optcarrot 120f 走らせて profile 収集
4. all.so の `o/*.o` を `rm` → `-fprofile-use=$DIR -fprofile-correction` で再ビルド (.so 再リンク)
5. 計測

ルール (`-fno-lto` on all.so) は維持。 結果 (RUNS=10 FRAMES=300, 複数 run の median):

| compiler  | koruby PGO | AOT flags          | best fps | median fps | size  |
|-----------|------------|--------------------|---------:|-----------:|------:|
| gcc-16    | -          | -O3 -march=native  |  113.11  | **112.04** | 3.0MB | ★
| gcc-16    | -          | -O2 -march=native  |  112.42  |   111.56   | 2.9MB |
| gcc-16    | + (-O2)    | -O2 -march=native  |  113.51  |   111.53   | 2.9MB |
| gcc-15    | -          | -O2 -march=native  |  112.05  |   110.94   | 2.7MB |
| gcc-15    | -          | -O3 -march=native  |  111.22  |   110.15   | 2.8MB |
| gcc-15    | + (-O2)    | -O2 -march=native  |  110.93  |   109.75   | 2.7MB |
| gcc-13    | -          | -O2 -march=native  |  107.27  |   105.87   | 2.7MB |

**AOT-PGO の効果は劇的**:
- 非 PGO baseline (gcc-15): median 105.61
- AOT-PGO (gcc-16):         median 112.04 → **+6.4 fps (+6.1%)**
- koruby-PGO 単独 (gcc-15): median 106.71 → +1.1 fps のみ

ホットコードの大半は AOT-emitted SD_*.c に集中しているので、 そっちを
profile-guided に並べ替える効果が圧倒的。 koruby 本体の PGO は、 既に
AOT 化された後だと効きどころが少なく、 AOT-PGO に上乗せしても
no-op に近い (median 111.53 vs 111.56)。

**追加観察**:
- AOT-PGO 下では **`-O3` が `-O2` を 0.5 fps 上回る**: 普段は -O3 の
  bloat が icache を圧迫するが、 PGO で hot block first にレイアウト
  されると -O3 の追加 inline / unroll が効くようになる。
- AOT-PGO 下で **gcc-16 が gcc-15 を 1 fps 上回る** (普段は逆)。
  PGO meta-data 読み込み + register allocation の改善が新世代 gcc で
  進んでる模様。
- size: 非 PGO 3.5MB → AOT-PGO 2.9-3.0MB。 cold path が削られて
  20% 小さくなる (副次効果として配布サイズも改善)。
- gcc-13 は AOT-PGO でも 105 fps 止まり (-6 fps 差)。 PGO 関連の
  改善が gcc-15+ で大きく入ったことが伺える。

### 最終確定 ベスト構成 (production)

```sh
# 1. koruby 本体は普通にビルド (-O3 -flto=auto, Makefile デフォルト)
CC=gcc-16 make

# 2. AOT を instrument 付きでビルド (PGO pass 1)
CC=gcc-16 \
CFLAGS="-O3 -march=native -fPIC -fno-plt -fno-semantic-interposition -fprofile-generate=$PWD/aot-pgo-data" \
LDFLAGS="-fno-lto -fprofile-generate=$PWD/aot-pgo-data" \
./koruby --aot-compile <prog>

# 3. workload を 1 度走らせて profile 収集
./koruby <prog>

# 4. all.so を profile-use で再ビルド
rm code_store/o/*.o code_store/all.so
CC=gcc-16 \
CFLAGS="-O3 -march=native -fPIC -fno-plt -fno-semantic-interposition -fprofile-use=$PWD/aot-pgo-data -fprofile-correction" \
LDFLAGS="-fno-lto" \
make -C code_store all.so

# 5. 本番実行
taskset -c 0 ./koruby <prog>   # → ~112 fps
```

実装としては `tools/bench-aot-pgo.sh` の `run_aot_pgo_config` がそのまま
雛形。 Makefile に `koruby-aot-pgo` ターゲットとして組み込めば
1 コマンドで完結 (今後 todo)。

vs CRuby 4.0 + YJIT (177.97 fps): **0.63×** (旧 0.57× → 改善)。
AOT 単独 105 → AOT+PGO 112 で **1.07× 改善**。

---

## 2026-05-02 現状サマリ (検証付き)

過去の数字は checksum 検証なしで取られていて、optcarrot の rendering loop
が壊れた状態 (空フレーム返す bug) のまま速い数字を記録していた。
今は `tools/bench-optcarrot.sh` が checksum 行を比較して mismatch 検出
するので、以下は **CRuby 出力と一致した状態での実測**。

### optcarrot (Lan_Master.nes, 600 frames headless)

`taskset -c 0`, koruby は `gcc -O2 -flto`, SDs は `gcc -O3 -fPIC -fno-plt -march=native`.

| 構成 | fps | checksum | vs CRuby no-JIT |
|---|---:|---:|---:|
| ruby (no JIT) | 38.0 | 60838 | 1.00× |
| **koruby (interp)** | **50.1** | 60838 | **1.32×** |
| **koruby (AOT-cached, `--aot-compile`)** | **87.4** | 60838 | **2.30×** |
| ruby --yjit | 162.7 | 60838 | 4.28× |

YJIT との差は ~1.86×。 まだ縮める余地あり。

### whileloop (`n += i; i += 1` を 100M iter)

| 構成 | wall time |
|---|---:|
| ruby (no JIT) | 1.61 s |
| ruby --yjit | 1.58 s |
| koruby (interp) | 2.02 s |
| **koruby (AOT-cached)** | **0.28 s** ← yjit の 5.7×、ruby の 5.8× |

整数しか出ない tight loop は AOT の partial-eval が効いて圧勝。
optcarrot のように method call + ivar + array indexing 中心になると
2.3× にとどまる。

### 直近の修正で効いた correctness 系 (perf 数字を歪めていたバグ)

* **masgn の attribute setter** — `@a, @b, @cpu.next_frame_clock = ...` の
  3つ目が無視されていて optcarrot の PPU が常に空フレーム返してた。
  fix 後も "速さ" は変わらず (むしろ正しく描画する分遅くなる) が、
  **以前の "248 fps" は嘘** で、実際は CRuby に勝ってもなかった。
* **基本演算 redef guard の絞り込み** — `class Integer; def gcd; end` で
  `+` の fast path まで無効化されていた。 名前が basic op か検査するよう
  修正 → whileloop 1.65s → 0.30s (5.5×)、 fib(30) 0.49s → 0.17s (2.9×)。
* **block-as-&proc slot collision (parse.c)** — `f arg, ary.map(&proc)` で
  arg が proc の param 値で上書きされていた。 `pop_frame` で
  parent.arg_index を child.max_cnt まで上げる + `block_floor` で
  rewind を抑止。 perf には直接効かないが正しく動かない code が動く。
* **proc.call の env 共有化** — `proc.call` が env を snapshot して block
  内の outer 変数書き戻しが伝搬しなかった。 直接共有に変更で正しく動く
  (パフォーマンスは ~わずかに上がる: 1 アロケーションを削減)。
* **per-iteration closure capture** — `(1..3).each { |i| procs << proc { i } }`
  で全 proc が同じ env を見ていた。 parse-time に block 体を walk して
  inner block_literal がある block にだけ `creates_proc` flag を立て、
  yield 時に fresh env を allocate + outer slot copy-back する slow path に
  切替。 普通の `each` body (proc を作らない) は fast path のまま。

---

## 過去の summary table (historical, 2026-04-30 以前)

過去のベンチ表は checksum 検証前の数字。 そのまま残す:

| 構成 | fps (median) | vs CRuby no-JIT |
|---|---:|---:|
| ruby (no JIT) | 41.0 fps | 1.00× |
| abruby (plain interp, CRuby C-ext) | 42 fps | 1.02× |
| koruby (interp, plain) | 42 fps | 1.02× |
| koruby (interp + PGO — `make koruby-pgo`) | 51 fps | 1.24× |
| abruby (--aot-compile-first, AOT only) | 71 fps | 1.73× |
| abruby (--aot + --pg-compile, AOT + PGC) | 75 fps | 1.83× |
| koruby (AOT — `make koruby-aot`) | 80 fps | 1.95× |
| koruby (AOT + PGO — `make koruby-pgo-aot`) | 110 fps | 2.68× |
| ruby --yjit / --jit | 175 fps | 4.27× |

**注**: `koruby AOT + PGO` の 110 fps は当時の rendering loop が完全に
動いてたかは怪しい。 今は AOT-cached で ~87 fps が再現可能な値。
PGO + AOT は再測定 (TODO)。

#### 26 bench の現状 (best of 3, taskset -c 0)

| 系統 | bench (vs ruby --yjit, smaller=koruby better) |
|---|---|
| **win (6)** | collatz 0.9×, each 0.83×, fannkuch 0.7×, gcd 0.8×, string 0.8×, times 0.9× |
| **tied 5%以内 (7)** | array, array_access, array_push, hash, inject, sieve, while |
| Float-heavy | mandelbrot 1.66×, nbody 1.42× ← FLONUM + 即値 fast path で改善 (元 7×/3.7×) |
| call-heavy | ack 2.4×, fib 2.3×, tak 2.5×, dispatch 2.4×, method_call 1.7×, factorial 1.9× |
| block-heavy | (each/inject/times は yield inline で勝ちまたは引き分けに) |
| その他 | ivar 1.89×, object 2.31×, binary_trees 1.79×, map 1.60× |

直近 round の主な投入:
- **inline `korb_yield`** (single-arg, single-param fast path): each 0.49→0.23s (2×)
- **`korb_object_new` ivar 事前確保**: object 0.53→0.42s (-20%)
- **leaf-pure prologue** (yield/super/block_given/const 不使用 method): fib -27%, tak -26%, method_call -28%, ack -21%
- **immediate Float (FLONUM)** + Float fast path: mandelbrot 7×→1.7× behind YJIT
- **basic-op redef guard** (correctness): `class Integer; def +; end` を尊重
- **`==` identity short-circuit + NaN!=NaN preserve**

残ったレバーは:
- **method body inlining (PGSD)**: fib/ack/tak の prologue を call 側にインライン化、~50% 削減見込み
- **polymorphic IC (3+entry)**: dispatch bench は 3-way poly で 2-entry では効かず
- **map allocator**: GC pressure (現状 Boehm GC、bump allocator にすると map/binary_trees 改善)

#### この round の big wins

render_pixel kernel を抽出して perf 解析しながら段階的に 80 → 110 fps:

| 変更 | optcarrot fps | kernel ms |
|---|---:|---:|
| baseline | 80 | 905 |
| block body を `code_repo` に登録 (parse.c) — block 内の hot loop が SD 化 | 85 | 670 |
| Array#<< fast path + `korb_ary_aref` を object.h で inline (SD 内に展開) | 87 | 250 |
| `korb_hash_aref` (FIXNUM/SYMBOL key) + `korb_class_of_class` (heap path) を object.h で inline | 98 | 195 |
| `EVAL_node_(method/func)_call(_block)` を `korb_dispatch_call_cached` 経由に — IC + prologue を SD 内 inline | **110** | 185 |

ASTro 原則: 全部 fast-path を SD の TU に持ち込んだだけ。新しい NODE 種別も
AST rewrite も入れていない。 C コンパイラに委ねられる範囲を広げただけ。

(注: cold-tail を `korb_node_*_slow` として koruby 本体に hoist
し SD 内に複製しないことで `all.so` のサイズ増を抑える。LTO は koruby
本体のみ、all.so の 388 SDs は LTO 関係外。)

#### abruby +cf にキャッチアップ済み (72 fps)

koruby AOT は abruby +cf と同等の 72 fps に到達した。実装した最適化:

1. **specialized prologues** — `mc->prologue` 機構: `prologue_cfunc` /
   `prologue_ast_simple_{0,1,2,3,N}` / `prologue_ast_general` から
   method_cache_fill 時に選択。 dispatch は **単一 indirect call** で
   方策分岐なし。
2. **inline prologues** (`prologues.h`) — `static inline always_inline` の
   prologue 本体を header 化。 各 SD `.so` が独自のコピーを持ち、
   `korb_dispatch_call_cached` の guarded direct call が、 SD の TU 内で
   prologue 本体を直接インライン展開。 cross-`.so` indirect call も
   関数呼出 frame もなくなる。
3. **inline ivar_get_ic / ivar_set_ic** (`object.h`) — fast path を
   ヘッダに移して SDs に直接インライン化。 cache hit 時は load + 比較 +
   load の 3 命令で完了。
4. **TLS 廃止** — `current_block` を `__thread` から外すだけで +3 fps。
   single-threaded 実行では `__tls_get_addr` 呼び出しは pure tax。
5. **stack overflow check 省略** — 16M slot の値スタックは
   pathological 再帰でしか溢れず、 SIGSEGV で落ちれば backtrace で
   分かる。 hot path から外して 1 比較分削減。
6. **frame trim** — `caller_node` / `fp` / `locals_cnt` フィールド省略
   (3 stores/call 削減)。

#### 段階的な改善

* **24 fps** (compare_by_identity 修正後 — レンダリング正常化直後)
* **32 fps** ←← Hash#aref が 47% / aset が 19% 食ってた。実装が full linear
  scan O(N) になっていたのを proper chained hash に直して 1.65× 高速化。
* **43 fps** ←← ivar_get が 36%。`class.ivar_names` を線形スキャンしていた
  のを、AST node に `struct ivar_cache { klass; slot; }` を `@ref` operand
  として inline cache 化して 1.6× 高速化。
* **44 fps** ←← `-flto=auto`。クロス TU の関数インライン化。
* **51 fps** ←← **PGO** (`make koruby-pgo`)。 optcarrot の 60-frame run で
  プロファイル収集 → re-build with `-fprofile-use`。 ホット分岐の予測が改善。
* **54 fps** ←← **AOT 特化** (`make koruby-aot`)。abruby と同じ per-method
  SD_*.c → all.so → dlopen 方式。 optcarrot を 30 フレーム走らせて全 entry
  を `code_store/c/SD_<hash>.c` に書き出し、`gcc -O3 -fPIC -fno-plt -march=native`
  で 388 個の SD を `code_store/all.so` に。 起動時に `koruby_cs_init` が
  dlopen し、`OPTIMIZE` で各 NODE の hash 値で `dlsym("SD_<hash>")` を引いて
  dispatcher を差し替え。
* **60 fps** ←← **specialized prologues + inline cache fast path**。
  `method_cache.prologue` フィールドを追加し、`method_cache_fill` の時点で
  `prologue_ast_simple` / `prologue_ast_general` / `prologue_cfunc` の中から
  選ぶ。 dispatch は `mc->prologue(...)` の **単一 indirect call** で内部に
  cfunc vs AST 判定なし。 さらに `EVAL_node_method_call` 内に inline cache 
  hit fast path を追加し、 cache hit 時は `korb_dispatch_call` を呼ばずに
  直接 `mc->prologue` を呼ぶ。 frame init も .caller_node / .fp / .locals_cnt
  を省いて 3 stores 削減。

#### ビルド方法

```sh
make koruby-pgo    # PGO build (51 fps)
make koruby-aot    # AOT build (54 fps) — 実行時 KORUBY_CODE_STORE で dir 指定可
```

#### YJIT との差 — 2.4× (理論的には縮められる)

YJIT 174 fps に対して koruby は 72 fps、 2.4× の差がある。
**原理的には koruby も C compiler 経由で生 x86 を吐いている** ので、
YJIT に勝てる余地はある。 残っている主な技術:

* **PGSD (Profile-Guided Specialized Dispatcher)** — 実行プロファイルから
  call site ごとの `mc->prologue` を観測し、 PGSD として **直接呼出** で
  焼き込む。 現状 inline prologue は guarded direct call で実現してるが、
  guard 自体が runtime overhead。 PGSD なら guard も消せる。
* **method body inlining** — abruby compiled でもやっていない、 YJIT 専有の
  技。 hot な caller-callee ペアで callee の body を caller の SD に直接
  インライン化する。 polymorphic ならガード付きで 2-3 件まで対応。
* **型に基づくノード rewrite** (`node_plus` → `node_fixnum_plus`) — 算術
  演算でメソッドディスパッチを完全省略。
* **FLONUM 即値化** — 現在 Float はヒープ。 即値化すればアロケーションが
  消える。
* **polymorphic IC** — `mc->klass[2]` 程度。 type-flapping call site で毎回
  miss するのが防げる。

### fib(35)

| 構成 | 時間 |
|---|---:|
| ruby (no JIT) | 1.17 s |
| **ruby --yjit** | **0.23 s** |
| koruby (interp, -O2) | 1.00 s |
| koruby (interp + AOT 特化, -O3) | 0.24 s |

### fib(40)

| 構成 | 時間 |
|---|---:|
| ruby (no JIT) | 4.5 s |
| **ruby --yjit** | **1.20 s** |
| koruby (interp + AOT 特化, -O3) | 2.69 s |

### 解釈
- 純インタプリタ単体 (-O2) で fib では **CRuby (no-JIT) より 1.17× 速い**
- AOT 特化で **5× 速い**
- optcarrot のように method call + ivar 中心のワークロードでも **CRuby (no-JIT) を 1.05× 超える**
- YJIT には負けるが (interp vs JIT なので妥当)、optcarrot で 3× 程度

## 成功した改善

### ✅ 1. ASTro AOT 特化
**効果**: fib(35) 0.55s → 0.24s (2.3× 高速化)

仕組み:
- 各 AST ノードに対し `SD_<hash>(c, n)` という関数を生成
- 子ノードへのディスパッチも特化された `SD_<child_hash>` を直接呼ぶ
- 関数は static inline 連鎖になるので、C コンパイラがツリー全体を 1 関数にインライン化できる
- 特に `node_plus(node_lvar_get, node_int_lit)` のような閉じた小さなサブツリーは数行のアセンブリに畳まれる

実装ファイル:
- `node.def` / `koruby_gen.rb` で SPECIALIZE タスク
- `./koruby -c script.rb` で `node_specialized.c` 生成
- 2 回目の build で `#include "node_specialized.c"` して取り込む

**学び**: C コンパイラに任せられる範囲が想像より広い。`-O3` + `static inline` + ノードハッシュ共有 の3点で大きな効果。

### ✅ 2. インラインメソッドキャッシュ (`struct method_cache`)
**効果**: fib のような呼出主体ベンチで顕著 (推定 30-40% 削減)

ナイーブなディスパッチ:
```c
m = klass->method_table_lookup(name);  // hash search
m->u.ast.body->head.dispatcher(c, m->u.ast.body);  // 2-step indirect call
```

キャッシュ済みディスパッチ:
```c
if (mc->serial == method_serial && mc->klass == klass) {
    mc->dispatcher(c, mc->body);  // 1-step indirect call
}
```

`mc` は call site の NODE に **per-site インライン**で埋め込まれる。`method_serial` はメソッド再定義のたびに +1 してキャッシュ全体を無効化。

実装上のポイント:
- 当初 `mc->method->u.ast.body->head.dispatcher` を毎回辿っていたが、3 段階間接参照だった
- `mc->body` と `mc->dispatcher` を直接持たせて 1 段階に減らした
- `mc->locals_cnt`/`mc->required_params_cnt` も持たせて method 構造体への参照を完全に消した

### ✅ 3. Fixnum 高速パス (オーバフロー検出付き)
**効果**: 算術重ベンチで重要

```c
NODE_DEF
node_plus(...) {
    VALUE l = EA(c, lhs); VALUE r = EA(c, rhs);
    if (LIKELY(FIXNUM_P(l) && FIXNUM_P(r))) {
        long a = FIX2LONG(l), b = FIX2LONG(r), s;
        if (LIKELY(!__builtin_add_overflow(a, b, &s) && FIXABLE(s)))
            return INT2FIX(s);
        return korb_int_plus(l, r);  // GMP に昇格
    }
    /* method dispatch fallback */
}
```

- `__builtin_add_overflow` は x86-64 で 1 命令 (`jo` / `jno`)
- FIXABLE は SHL/SHR で範囲チェック (CRuby 互換)

### ✅ 4. CRuby 互換 VALUE 表現
**効果**: 即値判定が極めて軽い

```c
#define FIXNUM_P(v)  (((VALUE)(v)) & FIXNUM_FLAG)   // 1 命令 (test)
#define NIL_P(v)     ((v) == Qnil)                  // 1 命令 (cmp)
#define RTEST(v)     (((VALUE)(v)) & ~Qnil)         // 1 命令 (test)
```

CRuby と同じビット配置にしたので、`RTEST` は `Qnil` と `Qfalse` を同時に false 判定できる魔法のマスク (両者の AND が 0) が効く。

### ✅ 5. EVAL_ARG マクロでの状態伝搬
**効果**: setjmp/longjmp 不要 + コンパイラ最適化が効く

```c
#define EA(c, n) ({                                       \
    VALUE _v = EVAL_ARG(c, n);                            \
    if (UNLIKELY((c)->state != KORB_NORMAL)) return Qnil; \
    _v;                                                   \
})
```

- 例外を起こせない部分木 (整数演算のみなど) では C コンパイラが state チェックを **完全に DCE**
- 一方 method call を含む部分木では分岐が残る (正しい)
- branch predictor 的にも `UNLIKELY` で正常パス側にバイアスがかかる

### ✅ 6. 共有 fp によるクロージャ
**効果**: yield のオーバヘッドが極小

`each { |i| s = s + i }` のようなループでは、

- ブロック作成: `proc->env = c->fp;` だけ (コピーなし)
- yield: `c->fp` 変更なし、`fp[param_base + i] = arg` でパラメータ書き込みのみ

CRuby 比でも軽い (CRuby は CFP を作って locals 配列をリンク)。

### ✅ 7. クラスごとの ivar shape
**効果**: ivar アクセスは配列添字 1 回

クラス側に「@x が slot 0、@y が slot 1...」というテーブルを持たせ、オブジェクト側は素朴な `VALUE *ivars` 配列。**書き込み初回だけ** klass の hash table を見る (新 slot 確保のため)。読み取りは固定 slot で 1 メモリアクセス。

CRuby の object_shape はもっと洗練されているが (transition tree)、最適化観点での効果は近い。

### ✅ 8. Boehm GC (実装速度の最適化)
直接の性能改善ではないが、**実装速度が劇的に上がった**:
- mark 関数を1個も書かなくて良い
- ルート登録不要 (C スタックも自動)
- ノード / オブジェクトに mark フラグや hook を付ける必要なし

これにより koruby 全体を短期間で立ち上げられた。中長期的には世代別 GC 等への移行を検討するが、現段階では Boehm のオーバヘッド (mark cost) は許容範囲内。

## 試したが採用しなかった改善

### ❌ NaN-boxing
当初検討したが **ユーザ指示で禁止**。理由は:
- VALUE 表現を CRuby と分けると CRuby のソースを流用しにくくなる
- Float が固定 16 バイトでもメモリ局所性的に大差なし

代わりに **将来 FLONUM 即値化** (CRuby と同じ low-2-bits=0b10) を入れる予定 (`todo.md` 参照)。

### ❌ 自前 mark/sweep GC (短期)
- 実装コストが大きい (object.c の各 struct に mark 関数が必要)
- Boehm のオーバヘッドはまだ計測上の問題ではなかった
- 中長期では考える ([todo.md](./todo.md))

### ❌ 例外処理を setjmp/longjmp に変更
- C コンパイラ的に setjmp は **副作用順序の barrier** になる
- ASTro 特化されたコード (深いインライン展開された SD_xxx 関数群) との相性が悪い
- 正常パスのコストは setjmp の方が安いが、**特化された subtree の最適化を犠牲にする** ほどではない
- abruby も同じ判断 (RESULT 構造体の 2 レジスタ伝搬)

### ❌ block を escape 対応にする
yield 用に共有 fp 方式を採用した結果、**escape する Proc では env がスタックに残れない**。env を heap 化する案もあったが:
- escape しないブロックが大半 (yield ベース)
- 二段階構造 (escape したら heap 化) は実装コストが高く、まだボトルネックではない

そのため現状は escape しない前提で「速さ優先」。

## 計測 / 観察ノート

### perf stat (fib(40), AOT 特化)

```
2.62 sec real
4.73 IPC
0.02% branch miss
9.78 G branches
50.2 G instructions
```

非常に IPC が高く branch predictor も良好。**コードパスが短くて密** な状態。
これ以上短縮するには C 命令そのものを減らす方向 (PG-baked call_static) が必要。

### YJIT との差の分解 (推測)

| 項目 | YJIT | koruby (AOT) | 差 |
|---|---|---|---|
| メソッド call ディスパッチ | inline 完全展開 | mc キャッシュ + 間接呼び出し | 主因 |
| Fixnum 演算 | 直接アセンブリ | 直接 C 演算 | 同等 |
| GC | mark-sweep + compact | Boehm (mark のみ) | YJIT 有利 |
| frame setup | minimal | fp += arg_index, zero locals | YJIT 有利 |

ホットループ 1 反復あたり 5-10 ns 程度の差で、ほぼメソッド呼出オーバヘッド由来と推測。

## 今後の優先候補 (perf 観点)

詳細 → [todo.md](./todo.md) の「性能向上のための課題」セクション。

1. **PG-baked call_static**: プロファイル後に call site の dispatcher を呼出先 SD に焼き直す → YJIT 並みを狙う
2. **RESULT 構造体化**: state を 2 レジスタ伝搬で
3. **型に基づくノード rewrite** (`node_plus` → `node_fixnum_plus`)
4. **FLONUM 即値化**
5. **polymorphic IC** (mc->klass[2] 程度)
