# naruby 性能メモ

ASTro 論文評価対象として **「Plain / AOT / Profile-Guided / JIT を 1
バイナリで切り替え可能な最小の Ruby 系」** という位置づけ。絶対性能を
尖らせる方向ではなく、**4 モードの差を比較する** ことに焦点。

## 計測環境

- WSL2 + Linux 6.8、host binary は `gcc-13 -O3 -ggdb3 -march=native`。
- code_store SD は `runtime/astro_code_store.c` の既定 CFLAGS
  `-O3 -fPIC -fno-plt -fno-semantic-interposition -march=native`
  で `make` ビルド (= 内側 cc は環境変数 `CC` に従う、デフォルト `gcc`)。
- 全ベンチ末尾で `p result`。最適化系の whole-loop DCE 防止。
- 各セルは 5 回の median (`/usr/bin/time -f "%e"`)。
- `naruby/plain` = `./naruby -i`、`naruby/pg` = `./naruby -p` で warmup
  後 `./naruby -p -b` を時間計測 (warmup でビルドされた `code_store/all.so`
  を `-b` で再ロード)。
- `pg+lto` は warmup 時に `ASTRO_EXTRA_CFLAGS=-flto`、
  `ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto"` を渡したケース。
- ccache を `~/.cache/ccache` に書けない sandbox 環境では `CCACHE_DISABLE=1`
  を環境変数で渡す。

## ベンチ題材一覧

| ベンチ | 内容 | 期待結果 |
|---|---|---|
| **fib** | naïve `fib(40)` (再帰) | 165580141 |
| **ackermann** | `ack(3, 11)` (深い再帰) | 16381 |
| **tak** | `tak(30, 22, 12)` (Takeuchi、深い再帰) | 13 |
| **gcd** | `gcd(2³¹−1, 2³⁰−1)` を 5×10⁷ 回累積 | 50000000 |
| **call** | 10 段尾呼出 `f0→f1→…→f9` を 10⁸ 回 | 42 |
| **zero** | builtin `zero` を 10⁹ 回 (1 段) | 100 |
| **chain20** | 20 段尾呼出を 5×10⁷ 回 | 42 |
| **chain40** | 40 段尾呼出を 2.5×10⁷ 回 | 42 |
| **chain_add** | 10 段で各 `n+1` を 10⁷ 回 | 10 |
| **compose** | 3 段合成 `f(g(h(x)))` を 3×10⁷ 回 | 19 |
| **branch_dom** | 固定分岐で `if (n<1e9) n+1 else n*2` を 5×10⁷ 回 | 43 |
| **deep_const** | 10 段の getter `j → i → … → a = 1` を 10⁸ 回 | 1 |

## 結果 (主要マトリクス)

各セル median of 5、単位: 秒。

### gcc 13 デフォルト

| bench | plain | pg | pg+lto | Δ (pg→pg+lto) |
|-------|-----:|----:|-------:|--------------:|
| fib | 5.61 | 0.55 | 0.56 | 0% |
| ackermann | 7.54 | 0.79 | 0.75 | -5% |
| tak | 1.12 | 0.11 | 0.11 | 0% |
| gcd | 3.85 | 0.17 | 0.16 | -6% |
| **call** | 9.47 | 1.15 | **0.49** | **-57%** |
| **zero** | 15.89 | 2.68 | **0.42** | **-84%** |
| **chain20** | 9.67 | 1.23 | **0.65** | **-47%** |
| **chain_add** | 1.33 | 0.11 | **0.05** | **-55%** |
| **compose** | 1.50 | 0.10 | **0.03** | **-70%** |
| **branch_dom** | 1.24 | 0.07 | **0.02** | **-71%** |
| **deep_const** | 3.76 | 1.18 | **0.58** | **-51%** |

### gcc 16 (`CC=gcc-16`)

LTO 周りと recursive 関数の inlining が大幅改善。LTO なし時点で
gcc-13 + LTO を凌ぐ:

| bench | plain | pg(g16) | pg+lto(g16) |
|-------|-----:|--------:|------------:|
| fib | 5.38 | **0.47** | 0.48 |
| ackermann | 7.44 | **0.46** | 0.47 |
| tak | 1.13 | **0.05** | 0.05 |
| gcd | 3.78 | 0.17 | 0.17 |
| call | 9.93 | 1.15 | **0.49** |
| zero | 16.18 | 0.67 | 0.69 |
| chain20 | 9.39 | 1.17 | **0.60** |
| chain_add | 1.22 | 0.11 | **0.04** |
| compose | 1.51 | 0.12 | **0.04** |
| branch_dom | 1.24 | 0.07 | **0.03** |
| deep_const | 3.35 | 1.13 | **0.47** |

### gcc 16 + LTO + 2-stage PGO

`-fprofile-generate=DIR` で instrument → 1 回実行で `*.gcda` 収集 →
`-fprofile-use=DIR` で再ビルド。loop 系で顕著に効く:

| bench | LTO only | +PGO | Δ |
|-------|---------:|-----:|---:|
| fib | 0.48 | 0.47 | -2% |
| ackermann | 0.47 | 0.44 | -6% |
| tak | 0.05 | 0.04 | -20% |
| **gcd** | 0.16 | **0.03** | **-81%** |
| call | 0.48 | 0.43 | -10% |
| zero | 0.67 | 0.67 | 0% |
| chain20 | 0.60 | 0.57 | -5% |
| chain_add | 0.05 | 0.04 | -20% |
| compose | 0.04 | 0.04 | 0% |
| branch_dom | 0.03 | 0.02 | -33% |
| deep_const | 0.49 | 0.41 | -16% |

## 推奨ビルド設定

| 用途 | 設定 |
|------|------|
| 通常 (gcc 13) | `-Wl,-Bsymbolic -flto` |
| gcc 16 が使える | `CC=gcc-16` + 上記 |
| 極限性能 (loop 中心) | `CC=gcc-16` + LTO + 2 段 PGO |

## なぜ chain 系で大きく速くなるか

ASTro の **HOPT / PGSD 投機チェイン** + LTO の貫通 inline:

```
PGSD_<top>     ─inline→ PGSD_<HOPT(f0_body)>
                           ─inline→ PGSD_<HOPT(f1_body)>
                                       ─inline→ ...
                                                  ─inline→ literal
```

各 pg_call site の sp_body を HOPT に含めて baked するので、chain
各レベルの SD が下流のすべての構造を符号化したユニークなシンボルになる。
LTO 併用時は `static inline` を直接呼ぶ前提で gcc/clang inliner が
クロス TU で chain を貫通 inline できる。

ガード check (`cc->serial == c->serial`) は各レベルで残るが、ループ本体
全体が 1 つの関数体にまとめられるので副次的にしか効かない。

詳細は [runtime.md](runtime.md) §5.3.1。

### asm レベルでの確認

**call (10 段尾呼出)**: gcc-16 + LTO で生成されたコード:

```asm
26d4: mov 0x68(%rsi),%rsi       ; sp_body 読み込み (= f1's body)
26dc: cmp 0x58(%rsi),%rax       ; cc check #1 (f0 → f1)
26e0: jne <deopt_1>
26e6: mov 0x68(%rsi),%rsi       ; (= f2's body)
26ee: cmp 0x58(%rsi),%rax       ; cc check #2 (f1 → f2)
... (cc check #3〜#10) ...
2768: cmp 0x58(%rsi),%rax       ; cc check #10
276c: jne <deopt_10>
2772: mov $0x2a,%ecx            ; ★ 42 を即値 load (f9 returns n=42)
... (RESULT pack + ret) ...
```

10 個の guard + 即値 load + return。**chain f0→f1→…→f9→n は
"10 個の guard + return 42" に折り畳まれた**。

**chain_add (10 段で各 +1)**: 各 fN が `n+1` を計算するチェイン。

```asm
2772: mov $0xa,%ecx              ; ★ 10 を即値 load (= 0+1+1+…+1)
```

10 段の `+1` がコンパイル時 constant folding で **literal 10** にまで
畳み込まれる。`p f0(0)` が `p 10` 相当の機械語まで baked される。

## なぜ recursive 系で chain と同じ効果が出ないか

`fib_body → pg_call(fib) → sp_body=fib_body` は cycle。HOPT 計算は
`is_hashing` フラグでサイクル検出して低情報量の値で打ち切るので、
`HOPT(fib_body)` は `HORG(fib_body)` と本質的に同じ識別性しか持たない。
結果として `PGSD_<HOPT>` ≈ `SD_<HORG>` で、LTO inline できる範囲も同じ。

ただし gcc-16 では plain な再帰関数でも IPA-SRA + frame elimination が
効いて、call 境界のオーバーヘッドが大きく減少する (fib 0.55→0.47、
ackermann 0.79→0.46)。これは PGSD 機構と独立した cc 側の改善。

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

## 計測の取り方 (再現手順)

```sh
# クリーンビルド (sandbox 環境では CCACHE_DISABLE=1)
make clean
make

# 単一ベンチを 5 回 median で測る
B=fib
rm -rf code_store
./naruby -q -p bench/$B.na.rb >/dev/null   # warmup (build SDs + load)
for i in 1 2 3 4 5; do
    /usr/bin/time -f "%e" ./naruby -q -p -b bench/$B.na.rb >/dev/null
done

# LTO 込みで測る
rm -rf code_store
ASTRO_EXTRA_CFLAGS="-flto" \
ASTRO_EXTRA_LDFLAGS="-Wl,-Bsymbolic -flto" \
    ./naruby -q -p bench/$B.na.rb >/dev/null
# 以下 timed loop 同様

# gcc-16 を強制
CC=gcc-16 ASTRO_EXTRA_CFLAGS="-flto" ... ./naruby -q -p bench/$B.na.rb

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
# 以下 timed loop で計測
```

bench 数値が大きく動いたとき:

1. `make clean && make` で再生成漏れ確認。
2. `code_store/c/SD_*.c` `code_store/c/PGSD_*.c` を全消しして再ベイクが
   走っているか確認。
3. 同じソースで 2 回 bench (warm cache 状態で比較)。
4. `code_store/hopt_index.txt` を見て期待した PGSD が baked されているか確認。
