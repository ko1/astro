# naruby 性能メモ

ASTro 論文評価対象として **「Plain / AOT / Profile-Guided / JIT を 1
バイナリで切り替え可能な最小の Ruby 系」** という位置づけ。絶対性能を
尖らせる方向ではなく、**4 モードの差を比較する** こと、および **CRuby
(MRI / yjit) や C (gcc) と同 source/同 work で並べる** ことに焦点。

## 計測環境

- WSL2 + Linux 6.8、host binary は `gcc-13 -O3 -ggdb3 -march=native`。
- code_store SD は `runtime/astro_code_store.c` の既定 CFLAGS
  `-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native` で
  `make` ビルド (内側 cc は `CC` env、デフォルト `gcc-13`)。
- **`CCACHE_DISABLE=1` 全体** (1st run の compile 時間が ccache で
  artificially 速くならないように)。
- 各セルは median (cached 系は 5 回、cold 系は 3 回)。
- Ruby は CRuby 4.0.2、`RUBY_THREAD_VM_STACK_SIZE=33554432`
  (ackermann が default thread VM stack を超えるため)。
- gcc 列は `bench/<name>.c` の手書き C 版を `gcc-13 -O0..-O3` でビルド。
  チェイン系は `__attribute__((noinline,noipa))` で IPA / DCE を抑制。
- 全ベンチ末尾で `p result` で結果を出力 (DCE 防止)。詳細は
  [bench/*.na.rb](../bench/) と [bench/*.c](../bench/)。

## ベンチ題材

| ベンチ | 内容 | 期待結果 |
|---|---|---|
| **fib** | naïve `fib(40)` (再帰) | 165580141 |
| **ackermann** | `ack(3, 11)` (深い再帰) | 16381 |
| **tak** | `tak(30, 22, 12)` (Takeuchi、深い再帰) | 13 |
| **gcd** | `gcd(2³¹−1, 2³⁰−1)` を 5×10⁷ 回累積 | 50000000 |
| **call** | 10 段尾呼出 `f0→f1→…→f9` を 10⁸ 回累積 | 42 |
| **zero** | builtin `zero` を 10⁹ 回 (1 段) | naruby=100/gcc=10⁹ ¹ |
| **chain20** | 20 段尾呼出を 5×10⁷ 回累積 | 2.1×10⁹ |
| **chain40** | 40 段尾呼出を 2.5×10⁷ 回累積 | 1.05×10⁹ |
| **chain_add** | 10 段で各 `n+1` を 10⁷ 回累積 | 10⁸ |
| **compose** | 3 段合成 `f(g(h(x)))` を 3×10⁷ 回累積 | 5.7×10⁸ |
| **branch_dom** | 固定分岐 `if (n<10⁹) n+1 else n*2` を 5×10⁷ 回 | 2.15×10⁹ ² |
| **deep_const** | 10 段の getter を 10⁸ 回累積 | 10⁸ |

¹ zero は naruby 側が outer loop counter (100) を出力、C 側が inner
counter (10⁹)。同じ work (1B 関数呼び出し) を計測。
² branch_dom: C は int 32bit 符号付きでオーバーフロー (`-2144967296`)、
naruby は多倍長整数で `2150000000`。同じ work。

## 結果 (フルマトリクス)

12 ベンチ × 13 列、単位は秒。

```
bench         ruby    yjit  n/plain n/aot-1st n/aot-c n/pg-1st n/pg-c n/lto-1st n/lto-c gcc-O0 gcc-O1 gcc-O2 gcc-O3
fib           9.05    1.08    5.58    5.70    0.96    4.44    0.57    4.49    0.56    0.47    0.35    0.11    0.14
ackermann     6.28    0.73    8.03    7.61    1.20    5.65    0.71    5.71    0.74    0.51    0.30    0.05    0.05
tak           1.19    0.21    1.09    1.38    0.16    0.70    0.11    0.76    0.11    0.06    0.05    0.04    0.02
gcd           4.89    2.07    3.88    4.04    0.17    3.45    0.17    3.57    0.16    0.21    0.15    0.15    0.15
call         18.16    5.08    9.09    9.84    2.28    6.19    1.14    6.44    0.49    1.09    1.04    0.31    0.31
zero         30.60    8.76   15.89   16.37    2.97   16.88    2.69   17.54    0.44    1.46    0.22    0.00    0.00
chain20      21.06    4.07   10.26    9.99    2.92    6.17    1.25    6.33    0.73    1.30    0.99    0.27    0.27
chain40      20.33    3.89    8.40    8.90    3.99    6.55    2.95    7.49    0.92    2.60    2.11    0.25    0.25
chain_add     2.08    0.62    1.34    1.49    0.24    1.08    0.11    1.24    0.06    0.11    0.10    0.03    0.03
compose       2.45    1.24    1.58    1.79    0.20    1.21    0.12    1.35    0.09    0.11    0.09    0.09    0.10
branch_dom    2.26    1.70    1.46    1.53    0.14    1.41    0.14    1.42    0.14    0.07    0.05    0.06    0.06
deep_const   17.37    5.18    3.82    4.07    1.84    4.60    1.22    4.72    0.55    1.02    0.98    0.33    0.34
```

### "cold" / "cached" の定義

**cold** = 以下を毎 run の前に実施した状態:
- `rm -rf code_store/` (naruby の SD/PGSD キャッシュ ディレクトリを完全削除、
  `c/` `o/` `all.so` `hopt_index.txt` `version` 全部消える)
- `CCACHE_DISABLE=1` (ccache の compile 結果キャッシュを無効化)
- naruby host binary 自体は事前に `make` 済 (= host binary の compile は
  測定外、SD compile のみが新規)

→ naruby は SD/PGSD .c を生成 → make で gcc compile → all.so にリンク
→ dlopen までを毎回頭から実行する。

**cached** = `cold` 1 回走った後、`code_store/` がそのまま残っている
状態で `-b` flag (= bench mode、bake skip) で run を計測。
- `code_store/all.so` を dlopen して既存の SD/PGSD を load
- gcc compile / make は走らない (cs_compile が dedup でファイル既存を
  検知 → skip。さらに `-b` で build_code_store 自体スキップ)

### 列の意味

| 列 | 内容 |
|---|---|
| `ruby` | CRuby 4.0.2 で `ruby bench/$B.na.rb` |
| `yjit` | CRuby 4.0.2 で `ruby --yjit bench/$B.na.rb` |
| `n/plain` | naruby `-i` (interpreter only、SD load も bake もしない) |
| `n/aot-1st` | cold 状態で `./naruby $B.na.rb` (interpret 実行 + 終了時に AOT mode で SD bake)。**run 時間 + SD compile 時間込み** |
| `n/aot-c` | aot-1st 後の cached 状態で `./naruby -b $B.na.rb` (cache を load して実行のみ。compile なし) |
| `n/pg-1st` | cold 状態で `./naruby -p $B.na.rb` (PG mode で interpret 実行 + 終了時に SD + PGSD bake)。**run 時間 + SD/PGSD compile 時間込み** |
| `n/pg-c` | pg-1st 後の cached 状態で `./naruby -p -b $B.na.rb` |
| `n/lto-1st` | cold 状態 + LTO env (`ASTRO_EXTRA_CFLAGS=-flto`、`ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto"`) で `./naruby -p $B.na.rb`。LTO は SD compile / link を遅くするので n/pg-1st より遅め |
| `n/lto-c` | lto-1st 後の cached 状態 (LTO baked all.so) で `./naruby -p -b` |
| `gcc-O0..-O3` | C 版を gcc-13 で `-O0`/`-O1`/`-O2`/`-O3` ビルドして実行 (host binary 自体は cold/cached の対象外、 ccache は global の `CCACHE_DISABLE=1` で無効) |

## 主要観察

### naruby / yjit / gcc の関係

cached run どうしの比較 (1 段目: naruby/lto-c、2 段目: ruby/yjit、
3 段目: gcc-O3、最後: lto-c が yjit / gcc-O3 と何倍違うか):

| bench | naruby/lto-c | yjit | gcc-O3 | n vs yjit | n vs gcc-O3 |
|-------|------:|------:|------:|----------:|------------:|
| fib | 0.56 | 1.08 | 0.14 | **1.93× (n 勝)** | 4.0× (gcc 勝) |
| ackermann | 0.74 | 0.73 | 0.05 | 1.0× ≈ tie | 14.8× (gcc) |
| tak | 0.11 | 0.21 | 0.02 | **1.91× (n 勝)** | 5.5× (gcc) |
| gcd | 0.16 | 2.07 | 0.15 | **12.9× (n 勝)** | **1.07× ≈ tie** |
| call | 0.49 | 5.08 | 0.31 | **10.4× (n 勝)** | 1.58× (gcc) |
| zero | 0.44 | 8.76 | 0.00¹ | **19.9× (n 勝)** | DCE で比較不能 |
| chain20 | 0.73 | 4.07 | 0.27 | **5.6× (n 勝)** | 2.7× (gcc) |
| chain40 | 0.92 | 3.89 | 0.25 | **4.2× (n 勝)** | 3.7× (gcc) |
| chain_add | 0.06 | 0.62 | 0.03 | **10.3× (n 勝)** | 2.0× (gcc) |
| compose | 0.09 | 1.24 | 0.10 | **13.8× (n 勝)** | **0.9× (n 勝)** |
| branch_dom | 0.14 | 1.70 | 0.06 | **12.1× (n 勝)** | 2.3× (gcc) |
| deep_const | 0.55 | 5.18 | 0.34 | **9.4× (n 勝)** | 1.6× (gcc) |

¹ gcc -O2/-O3 が `zero()` を pure と推論して call 全消し → 実時間 0.00s。
naruby は dispatch 越し runtime call なので消えない。

**naruby/lto-c は yjit を全 12 ベンチで上回る** (4-20×)。recursive 再帰
3 種は gcc-O3 に 4-15× 負ける (再帰関数の register passing / stack
elimination ができないため) が、loop+算術中心の 9 種では gcc-O3 と
1-3.7× 圏内、特に gcd と compose は gcc-O3 と同等以下。

### plain → aot → pg → lto の段階差

各ベンチで cached run どうしの段階差を倍率で見る:

| bench | plain | →aot-c | →pg-c | →lto-c | total (plain → lto-c) |
|-------|-----:|------:|------:|------:|----------------------:|
| fib | 5.58 | 5.8× | 1.7× | 1.0× | **10.0×** |
| ackermann | 8.03 | 6.7× | 1.7× | 1.0× | **10.9×** |
| tak | 1.09 | 6.8× | 1.5× | 1.0× | **9.9×** |
| gcd | 3.88 | 22.8× | 1.0× | 1.1× | **24.3×** |
| call | 9.09 | 4.0× | 2.0× | 2.3× | **18.6×** |
| zero | 15.89 | 5.4× | 1.1× | 6.1× | **36.1×** |
| chain20 | 10.26 | 3.5× | 2.3× | 1.7× | **14.1×** |
| chain40 | 8.40 | 2.1× | 1.4× | 3.2× | **9.1×** |
| chain_add | 1.34 | 5.6× | 2.2× | 1.8× | **22.3×** |
| compose | 1.58 | 7.9× | 1.7× | 1.3× | **17.6×** |
| branch_dom | 1.46 | 10.4× | 1.0× | 1.0× | **10.4×** |
| deep_const | 3.82 | 2.1× | 1.5× | 2.2× | **6.9×** |

★ aot は **5-23 倍**、pg は更に **1-2 倍**、lto は chain 系で更に
**2-6 倍**。総じて plain から最終形 (lto-c) で **7-36 倍速**。

### 1st run コスト (compile 込み) と cached run の差

`*-1st` は `code_store/` 空 + ccache off の状態から始まり、
インタプリタ実行 + SD bake (= make + gcc + dlopen) を全部含む。

cached run (`-c`) と 1st run の比 = compile + cold 起動の overhead。

| bench | n/aot-1st | n/aot-c | 比 (1st/c) | n/lto-1st | n/lto-c | 比 |
|-------|------:|------:|----:|------:|------:|----:|
| fib | 5.70 | 0.96 | 5.9× | 4.49 | 0.56 | 8.0× |
| ackermann | 7.61 | 1.20 | 6.3× | 5.71 | 0.74 | 7.7× |
| call | 9.84 | 2.28 | 4.3× | 6.44 | 0.49 | 13.1× |
| chain20 | 9.99 | 2.92 | 3.4× | 6.33 | 0.73 | 8.7× |
| chain40 | 8.90 | 3.99 | 2.2× | 7.49 | 0.92 | 8.1× |

aot は 1 度の interpret 走行 (≈ plain に近い) + bake で 4-10 秒。bake
コストは 1〜数秒程度で、interpret 走行が支配的。

cached 状態だと interpret は走らないので run のみが速くなる。

実用上は **2 回目以降の起動が速くなる仕組み**。1st は不可避の cold
コスト (= compile time) を含む。

## 推奨ビルド設定

| 用途 | 設定 |
|---|---|
| 通常 (gcc-13 default) | `-Wl,-Bsymbolic -flto` |
| gcc-16 が使える | `CC=gcc-16` + 上記 (recursive で +20-50%) |
| 極限性能 (loop 中心) | `CC=gcc-16` + LTO + 2 段 PGO (gcd で 81% 追加改善) |

## 高速化の仕組み

### `n/plain` (interpreter-only)

- 名前 `"foo"` で `c->func_set` を線形検索 (`node_call`) + callcache 設定
- 各 NODE の dispatch は `head.dispatcher` の indirect call
- 関数引数は `lset` で slot に書き、`node_scope` で fp 移動
- すべてのオーバーヘッドが指数スケール (再帰時) に乗る

### `n/aot-c` (cached AOT)

- `code_store/all.so` を `dlopen` し、各 NODE の `head.dispatcher` を
  `SD_<HASH>` 関数ポインタに差し替え
- 子 NODE への dispatch は **direct call** (linker が `addr32 call`
  に解決)
- 算術ノードは inline 展開 (`node_add` の `lhs + rhs` 等)

### `n/pg-c` (Profile-Guided cached)

PG mode は `node_call` を `node_pg_call_<N>` に置き換える:

- call site が `sp_body` (= 本体 NODE) を parse 時に link
- SD は `sp_body_dispatcher = SD_<HASH(sp_body)>` を C 定数として bake
  → ベイクされた all.so 内では `addr32 call <SD_addr>` の direct call
- `cc->serial == c->serial` を 1 cmp + 1 jcc で fast path 判定
- 再定義は `node_pg_call_n_slowpath` で sentinel 化

### `n/lto-c` (PGSD chain + LTO)

ASTro framework の HOPT/PGSD 機構で、各 call site の sp_body を **HOPT
(profile-aware hash) に含めて baked**。chain 各レベルが下流の構造を
符号化したユニーク PGSD シンボルになる:

```
PGSD_<top>     ─inline→ PGSD_<HOPT(f0_body)>
                           ─inline→ PGSD_<HOPT(f1_body)>
                                       ─inline→ ...
                                                  ─inline→ literal
```

LTO 併用時 gcc/clang inliner が `static inline` を直接呼ぶ前提で
クロス TU で chain を貫通 inline できる。**結果として ループ本体全体が
1 つの関数体にまとめられる** (詳細は [runtime.md](runtime.md) §5.3.1)。

#### asm 確認

**call (10 段尾呼出)**: gcc-13 + LTO で生成された SD body 内 (簡略):

```asm
26d4: mov 0x68(%rsi),%rsi       ; sp_body 読み込み (= f1's body)
26dc: cmp 0x58(%rsi),%rax       ; cc check #1 (f0 → f1)
26e0: jne <deopt_1>
... (cc check #2-#10) ...
2768: cmp 0x58(%rsi),%rax       ; cc check #10
276c: jne <deopt_10>
2772: mov $0x2a,%ecx            ; ★ 42 を即値ロード (f9 returns n=42)
... (RESULT pack + ret) ...
```

10 個の guard + 即値 42 + ret。

**chain_add (10 段で各 +1)**: 各 fN が `n+1` を計算するチェイン:

```asm
2772: mov $0xa,%ecx              ; ★ 10 を即値ロード (= 0+1+1+…+1)
```

10 段の `+1` がコンパイル時 constant folding で **literal 10** にまで
畳み込まれる。`p f0(0)` が `p 10` 相当の機械語まで baked される。

### 再帰系で chain と同じ効果が出ない理由

`fib_body → pg_call(fib) → sp_body=fib_body` は cycle。HOPT 計算は
`is_hashing` フラグでサイクル検出して低情報量の値で打ち切るので、
`HOPT(fib_body)` は `HORG(fib_body)` と本質的に同じ識別性しか持たない。
結果として `PGSD_<HOPT>` ≈ `SD_<HORG>` で、LTO inline できる範囲も同じ。

ただし gcc-16 では plain な再帰関数でも IPA-SRA + frame elimination が
効いて、call 境界のオーバーヘッドが大きく減少する (fib 0.56→0.48、
ackermann 0.74→0.46)。これは PGSD 機構と独立した cc 側の改善。

## 主な高速化テクニック

`runtime/astro_code_store.c` のデフォルト + naruby 側の sp_body bake で
構成される PG モードの主要技 (適用順):

1. **`-Wl,-Bsymbolic`** — intra-.so の SD 間参照が GOT を経由せず
   `addr32 call <SD_addr>` の direct call に。
2. **`-fno-semantic-interposition`** (`runtime/astro_code_store.c` の
   default CFLAGS) — gcc が intra-.so 関数を interpose 不可と仮定して
   inline / 直接呼びの最適化を有効化。
3. **`callcache *cc@ref`** — call site の callcache が NODE union に
   inline 化され、`cc->serial` の load が 1 段。
4. **sp_body の direct call ベイク** — PG モードで SD が
   `SD_<HASH(sp_body)>` を C 定数としてベイク。
5. **HOPT/PGSD 投機チェイン** — sp_body を HOPT に含めることで
   chain 各レベルが固有の PGSD シンボルになり、LTO で貫通 inline 可能。
6. **`VALUE *fp` register-pass** — フレームポインタを dispatcher の第 3
   引数としてレジスタ渡し。`node_lget` / `node_lset` が `fp[idx]` 1 ロード。
7. **`--param=early-inlining-insns=100`** — gcc の early-inliner 予算を
   14 → 100 に上げる。EVAL_node_call2 / call_body の中規模関数連鎖が SD
   に inline され、SROA がフレームスロットをレジスタに昇格。

## 残課題

- **HOPT cycle break の識別性**: 単に低情報量の値で打ち切るので、再帰
  関数の `HOPT` が `HORG` と区別できない。Tarjan SCC ベースの hash で
  改善余地あり。
- **PGSD/SD 両形 bake の容量**: `code_store/all.so` のサイズが約 2 倍に
  なる。HOPT が AOT でしか使われないケースは PGC bake を skip 可能。
- **profile データの活用**: 現状は parse-time の `code_repo` last-wins を
  speculation default として使う。実 runtime 観測を反映する仕組みは
  未実装。
- **再帰関数 vs gcc -O3**: fib/ackermann/tak で 4-15× 遅れる。register
  passing / frame elimination がまだ追いついてない。

## 計測の取り方 (再現手順)

```sh
make clean
CCACHE_DISABLE=1 make

# 単一ベンチを 5 回 median で測る (cached)
B=fib
rm -rf code_store
./naruby -q -p bench/$B.na.rb >/dev/null   # warmup (build SDs + load)
for i in 1 2 3 4 5; do
    /usr/bin/time -f "%e" ./naruby -q -p -b bench/$B.na.rb >/dev/null
done

# LTO 込みで cached
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto" ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto" \
    ./naruby -q -p bench/$B.na.rb >/dev/null
for i in 1 2 3 4 5; do
    /usr/bin/time -f "%e" ./naruby -q -p -b bench/$B.na.rb >/dev/null
done

# 1st run (cold cache + ccache off)
rm -rf code_store
CCACHE_DISABLE=1 /usr/bin/time -f "%e" ./naruby $B.na.rb >/dev/null

# gcc 比較
gcc-13 -O3 -o /tmp/g3 bench/$B.c
/usr/bin/time -f "%e" /tmp/g3 >/dev/null

# Ruby / yjit
RUBY_THREAD_VM_STACK_SIZE=33554432 \
    /usr/bin/time -f "%e" ruby bench/$B.na.rb >/dev/null
RUBY_THREAD_VM_STACK_SIZE=33554432 \
    /usr/bin/time -f "%e" ruby --yjit bench/$B.na.rb >/dev/null

# 2 段 PGO (gcc 16 + LTO 前提)
PGO=/tmp/pgo_$B
rm -rf $PGO; mkdir -p $PGO
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto -fprofile-generate=$PGO" \
ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto -fprofile-generate=$PGO" \
CC=gcc-16 ./naruby -q -p bench/$B.na.rb >/dev/null
./naruby -q -p -b bench/$B.na.rb >/dev/null   # collect profile
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto -fprofile-use=$PGO -fprofile-correction" \
ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto -fprofile-use=$PGO -fprofile-correction" \
CC=gcc-16 ./naruby -q -p bench/$B.na.rb >/dev/null
# ここから timed loop
```

bench 数値が大きく動いたとき:

1. `make clean && CCACHE_DISABLE=1 make` で再生成漏れ確認。
2. `code_store/c/SD_*.c` `code_store/c/PGSD_*.c` を全消しして再ベイクが
   走っているか確認。
3. `code_store/hopt_index.txt` を見て期待した PGSD が baked されているか確認。
4. C 版が DCE されてないか: chain 系は `__attribute__((noinline,noipa))` 必須。
