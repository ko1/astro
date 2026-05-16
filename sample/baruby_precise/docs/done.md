# baruby Done

[spec.md](spec.md) — 言語仕様、[runtime.md](runtime.md) — 実装、
[todo.md](todo.md) — 残タスク、[perf.md](perf.md) — ベンチ。

## 2026-05-16 (4) — 9 つ目の backend: `mark_compact_gen` (gen + Lisp-2 hybrid)

`copy_gen` の major (semispace Cheney) を `mark_compact` (Lisp-2 sliding) に
差し替えた generational hybrid。

- Nursery: 16 MiB bump (`copy_gen` と同じ)
- Tenured: 512 MiB single region (copy_gen は 2×256 MiB だった)
- Minor: Cheney-style nursery → tenured (= copy_gen と同じ)
- Major: tenured 内で mark + Lisp-2 sliding compact (3-pass)
- WB / remset: copy_gen と同じ

メリット: tenured 仮想空間が 1×512 MiB (vs copy_gen は 2×256 MiB)。
デメリット: major が semispace より複雑 (3-pass) だが compact 自体は速い
(連続 marked を memmove で batch)。

性能 (plain, 1 run、 vs copy_gen / copy_gen_inc):

| Bench         | copy_gen | copy_gen_inc | **mark_compact_gen** |
|---------------|---------:|-------------:|---------------------:|
| binary_trees  |     0.82 |         0.82 |            **0.78** |
| list_alloc    |     0.97 |         0.96 |            **0.89** |
| string_concat |     0.59 |         0.53 |            **0.51** |
| fib_pair      |     0.95 |         0.92 |            **0.81** |
| substr_churn  |     0.92 |         1.04 |                0.93 |
| gc_combined   |     0.93 |         1.08 |                0.93 |
| interp_calc   |     1.00 |         0.98 |                1.00 |
| list_sort     |     1.13 |         1.16 |            **1.08** |

binary_trees / list_alloc / string_concat / fib_pair / list_sort の **5/8 で
mark_compact_gen が gen 系の中で最速**。 copy_gen の Cheney は 2 region 間
の memcpy が連続するので tenured へ大量 promote する worklload に強いが、
mark_compact_gen は **in-place compaction で 1 region で済む**ぶん帯域節約。

## 2026-05-16 (3) — mark_compact の slide 段階を batching

3-pass の最終 (slide) で、 連続 marked オブジェクトは src - dst delta が
共通なので 1 回の `memmove` に纏められる。 dead が間に挟まると delta が
変わるので runs を分割。 数百万回の memmove 呼び出しを runs 単位に削減。

影響は限定的: binary_trees / list_alloc などで誤差程度。 mark_compact の
ホットスポットは GC 自体ではなく dispatch (perf record で DISPATCH_node_if
13%, _ary_push 9% など) で、 GC 内最適化のリターンが小さいと判明。

## 2026-05-16 (2) — 8 つ目の backend: `mark_compact` (Lisp-2 sliding compactor)

`gc_mark` の per-object malloc/free を回避しつつ非 moving (compaction 時の
み移動) を実現する 8 つ目の backend。 単一 mmap'd region (1 GiB virtual,
lazy-paged) からの bump alloc + 古典的「Lisp 2」 圧縮:

1. **Mark**: BFS from roots via gray queue (= mark_gen と同じ)
2. **Forward-address pass**: region を線形走査、 marked オブジェクトの
   ->fwd に packed dest 計算
3. **Update-pointers pass**: 再び線形走査、 marked の outgoing pointer
   (a->items, s->bytes, items[i]) を target の ->fwd に書き換え。 root も
4. **Slide pass**: 各 marked を ->fwd へ memmove。 dst ≤ src なので
   memmove で安全、 連続 src だが間に dead があると memmove は分裂

### 詰まったポイント

- **stress mode で test_eq.ba.rb が SEGV**: `update_pointers` が
  `s->bytes` 0x7....0220 (region top の少し外) を deref → 高 sp slot に
  stale heap pointer が残っていて root scan で誤って live と判定された。
  copy_gen 同様に **high-water-mark zeroing** を追加 (前回の最深 sp 以下、
  かつ現在の sp_top より上の slot を 0 で埋める) で解決
- 全 test (plain + stress) + 全 bench で動作確認済み

### 性能 (plain mode, 1 run)

binary_trees で **mark の 7.18s → 0.59s** に (12×)。 list_sort や fib_pair
は世代別系 (copy_gen) には負けるが、 mark との比較では概ね optimal。

## 2026-05-16 — gen 系 backend の explicit remset + macro bench 追加

### 性能改善: explicit remembered set

mark_gen / mark_gen_inc / copy_gen / copy_gen_inc の 4 backend で、
旧版が minor GC で行っていた「dirty bit を求めて old/tenured 全走査」
(= O(|old|)) を、 WB で push される明示 remset (= O(|dirty|)) に置換。

- WB: holder->dirty が false なら remset に push し dirty = true
- minor: remset を走査して dirty=true のものだけ scan_outgoing
- major: remset を破棄して全 trace、 sweep で生存者の dirty を clear

perf record で interp_calc on mark_gen を見ると minor_gc が 44% を
占めていた。 remset 化で:

| Bench         | mark_gen 旧 | mark_gen 新 | copy_gen 旧 | copy_gen 新 |
|---------------|------------:|------------:|------------:|------------:|
| binary_trees  |        2.28 |    **1.56** |        1.11 |    **0.79** |
| interp_calc   |        2.87 |    **1.51** |        1.22 |    **1.07** |
| gc_combined   |        1.39 |        1.33 |        0.93 |        0.91 |
| list_sort     |        1.36 |        1.33 |        1.16 |        1.05 |

### マクロベンチ追加

- **`interp_calc.ba.rb`**: depth-12 AST を make_expr で構築 → eval_expr で
  再帰評価。 1000 反復。 build phase が alloc burst、 eval phase は
  純計算。 short-lived alloc + recursive read の典型
- **`list_sort.ba.rb`**: 2000 要素の整数 array に merge sort を 350 回
  実行。 merge 1 回が中規模 alloc burst を生み、 merge 完了で全部死ぬ
  パターン

## 2026-05-15 — GC backend を 7 種から build-time 選択可能に

`Makefile GC=<backend>` で 7 種類の GC アルゴリズムから build-time に
選べるようにした。 全 backend で test.ba.rb / test_ary / test_eq の
plain + stress mode、 bench 6 種が PASS。

### Backend 一覧

| GC値 | 名前 | 説明 |
|---|---|---|
| 1 | none | malloc + leak (rooting オーバーヘッドの baseline) |
| 2 | mark | non-moving mark&sweep (linked list of objects) |
| 3 | mark_gen | mark&sweep + 2-gen (nursery / tenured list) |
| 4 | mark_gen_inc | mark_gen + SATB 風 incremental marking infra |
| 5 | copy | semispace Cheney (現状の default) |
| 6 | copy_gen | nursery (bump) + tenured (semispace) |
| 7 | copy_gen_inc | copy_gen + 増分 major marking infra |

`make GC=mark_gen` のように選択。 未指定なら `GC=copy` (default)。
`-DBARUBY_GC=<N>` が Makefile から渡される。

### Infrastructure 整理

- `gc.h` を共通 interface 化 (BarubyGCKind / BarubyGCStats / WB hooks)
- backend ごとに `gc_<name>.c` (~200〜400 行)
- WB() macro: 非世代別 backend では no-op (`*slot = v`)、 gen 系は
  remset (dirty bit) を更新
- node.c / node.def の heap pointer 書込を全部 `baruby_gc_wb` /
  `baruby_gc_wb_bulk` 経由に統一 (6 箇所)
- stats output に `backend=<name>` と minor/major カウントを追加

### 実装と詰まったポイント

- **mark_gen の `promote()` バグ**: major GC で sweep_young が marked を
  clear してから sweep_old がスキャンすると、 新規 promote が unmarked と
  判定されて free される。 `promote(h, clear_marked)` を導入、 major では
  `clear_marked=false` で運用、 minor では `true` で運用
- **copy_gen の tenured 容量**: binary_trees の live tree は ~352 MB
  (header + payload 別 alloc で BaArray ノードは 88 byte/個)。 tenured
  semispace を 512 MiB に拡張
- **copy_gen の `from_end_cur`**: from-tenured の range check が region
  全体ではなく valid object 範囲 (= old_active_top まで) でないと、
  stale pointer が forward 経路に入って memcpy SEGV
- **copy_gen の pretenuring**: `nursery_size/2` を超える alloc は直接
  tenured に。 18 MB の string repeat (substr_churn) が小 nursery に
  入らない問題を回避
- **inc 系 backend の SATB 限界**: VALUE stack write には barrier が
  無いため、 純粋な SATB だけでは stack 経由で reachable になった
  オブジェクトを取りこぼす。 atomic root re-scan を追加したが、
  testbed としては安全側で「INC_WORK_PER_ALLOC = SIZE_MAX」 = 実質
  STW major としている。 infra (gray queue / SATB barrier) は残しているので
  stack-WB を入れれば真の incremental に切替可能

### 性能 (plain mode, 1 run, vs libgc baruby)

| Bench         | libgc | none  | mark  | mark_gen | mark_gen_inc | copy  | copy_gen | copy_gen_inc |
|---------------|------:|------:|------:|---------:|-------------:|------:|---------:|-------------:|
| binary_trees  | 0.91  | 0.60  | 7.17  | 2.28     | 2.30         | 0.53  | 1.11     | 1.16         |
| list_alloc    | 1.09  | 1.32  | 1.13  | 1.28     | 1.41         | 1.16  | 0.92     | 0.95         |
| string_concat | 0.97  | 1.70  | 1.72  | 1.64     | 1.75         | 0.94  | 0.50     | 0.55         |
| fib_pair      | 1.13  | 1.63  | 1.45  | 1.59     | 1.66         | 1.22  | 0.91     | 0.93         |
| substr_churn  | 1.36  | 1.74  | 1.23  | 1.64     | 1.78         | 1.31  | 0.87     | 0.92         |
| gc_combined   | 1.08  | 1.46  | 1.23  | 1.39     | 1.49         | 1.20  | 0.90     | 0.97         |

**観察**:
- **copy_gen が string-heavy で圧勝** (string_concat 0.50 s = libgc の 0.52×).
  短命 string の churn が nursery 経由でほぼ memcpy 不要に処理される
- **binary_trees は plain copy が最速** (0.53s). gen は long-lived tree
  の promote コストで遅くなる
- **mark は binary_trees が極端に遅い** (7.17s). 数百万オブジェクトの
  per-object malloc + sweep walk
- **none baseline は意外と遅い**: malloc の overhead で copy より遅い場面が
  多い。 bump alloc の威力

## 2026-05-14 — alloc 周りのオーバーヘッド削減

perf record で hot path を特定し、 string-alloc 系のオーバーヘッドを
潰した。 詳細 [perf.md §4](perf.md)。

### 変更内容

- `baruby_gc_alloc` を分割: 通常版 (zero-init payload) と
  `baruby_gc_alloc_byte` (memset スキップ)。 KIND_PAYLOAD_BYTE は
  caller が即座に bytes を埋めるので memset 不要
- `baruby_str_new` の malloc バッファ撤去。 caller が source の寿命を
  保証する前提に変更 (rodata / C スタック / GC-rooted)
- `baruby_str_slice(VALUE *src_ref, offset, len, sp_top)` を新設、
  heap interior 起点の slice (node_call_aget / _aget2 の STR 経路)
  はこちらに移動
- `baruby_gc_realloc_payload` も内部で kind 別に dispatch
  (PAYLOAD_BYTE は alloc_byte 経由)
- `Makefile`: `-flto=auto` を追加。 fib_pair 等で小さい alloc が
  inline されて -4% 効く

### 性能 (5 run 中央値、 plain mode、 vs `sample/baruby` libgc)

| Bench | conservative | precise (before) | precise (after) |
|---|---:|---:|---:|
| binary_trees | 0.907 s | 0.544 s | 0.576 s |
| list_alloc | 1.085 s | 1.152 s | 1.175 s |
| **string_concat** | 0.968 s | 1.160 s | **0.961 s** (-17%) |
| fib_pair | 1.127 s | 1.271 s | 1.285 s |
| **substr_churn** | 1.361 s | 1.594 s | **1.354 s** (-15%) |
| gc_combined | 1.079 s | 1.231 s | 1.244 s |

geomean ≈ 0.98× (precise が conservative より 2% 速い)。
string-heavy ベンチが parity 到達。 stress mode の全テスト PASS 維持。

## 2026-05-13 — semi-space moving GC + stress mode + ASTRO_ASSERT

mark&sweep の MVP を **Cheney 風 copying GC** に置き換え、 stress mode で
moving GC 特有のバグを総当たり退治した。 詳細 [runtime.md §5](runtime.md)。

### gc.c の刷新

- `BarubyGCNode` の linked-list + per-object malloc を捨て、
  **`mmap` 512 MiB の region 2 本を交互に使う semi-space** に変更
- alloc は `active_top` を bump するだけ。 collection は Cheney scan-loop で
  to-space を線形に処理
- `GCHeader { kind, size, fwd }` を payload 直前に置き、 forwarding pointer は
  この `fwd` に書く

### Stress mode (`BARUBY_GC_STRESS=1`)

- **毎 alloc で GC 起動** + 古い from-space を `mprotect(PROT_NONE)` +
  `madvise(MADV_DONTNEED)` で**恒久 retire**。 仮想アドレスは予約継続、
  物理ページは即解放
- 過去 GC 由来の stale pointer を deref すると確実に SIGSEGV
- 新しい to-space は毎 GC で `mmap` 取り直し (アドレス使い捨て)
- PRE-MARK 不変条件チェック: scan range の `IS_PTR(v)` が必ず現在の
  from-space を指す事を mark 前に検証

### 摘発したバグ

semi-space に切り替えた瞬間 `bench/binary_trees` が clobber data で
クラッシュ。 stress mode + verbose assert で次の根本パターンを発見:

- **C local rooting 漏れ** — `VALUE l = EVAL_ARG(c, lhs); VALUE r =
  EVAL_ARG(c, rhs);` で rhs eval が GC を引くと `l` が stale C local の
  まま。 該当箇所:
  - `baruby_ary_push`: x が realloc 後に stale → `VALUE *x_ref` に変更
  - `node_eq`, `_neq`, `_lt`, `_le`, `_gt`, `_ge`, `_mul`, `_spaceship`,
    `_call_aget`, `_call_aget2`: heap-typed operand を sp[] spill に統一
- **Helper 内部の C local** — `baruby_str_concat(VALUE av, ...)` の `av`
  が内部 alloc 後に stale。 → `VALUE *av_ref` に変更し、 alloc 後に
  `VAL2STR(*av_ref)` で post-GC アドレスを再取得 (`baruby_ary_plus`,
  `baruby_str_repeat`, `baruby_ary_repeat`, `baruby_str_append`,
  `baruby_str_concat`)

### `baruby_str_concat` 最適化

ref pattern 移行のついでに、 旧版で「source bytes を malloc 領域に
バッファコピーしてから alloc」 と書いていた回避コードを撤去。
source は ref で post-GC 再取得できるので malloc/memcpy/free を 1 set
削減 → **string_concat ベンチ 1.468 s → 1.160 s (-21%)**。

### ASTRO_ASSERT / ASTRO_DEBUG

framework 共通の assertion macro を `runtime/astro_debug.h` に新設:

```c
#if ASTRO_DEBUG
#  define ASTRO_ASSERT(expr) assert(expr)
#else
#  define ASTRO_ASSERT(expr) ((void)0)
#endif
```

baruby_precise では `ASTRO_DEBUG=1` がデフォルト (context.h)、
`make ASTRO_DEBUG=0` で release-shape build が可能。 gc.c の検証コード
(alloc 時 kind validity, process_object の type タグ、 stress mode の
PRE-MARK / FORWARD STALE 検出) は全て ASTRO_ASSERT に統一、
release build では完全に compile out。

### 検証

全テスト stress mode で PASS:

| Test | plain | stress |
|---|---|---|
| `test.ba.rb` | ✓ | ✓ |
| `test_ary.ba.rb` | ✓ | ✓ |
| `test_eq.ba.rb` | ✓ | ✓ |
| `bench/binary_trees` | ✓ (0.54 s) | (時間がかかるので未) |
| `bench/list_alloc` | ✓ (1.15 s) | (時間がかかるので未) |
| `bench/string_concat` | ✓ (1.16 s) | (時間がかかるので未) |

precise vs conservative の比較は [perf.md §2](perf.md) に。

## 2026-05-10 — bench 拡充 (GC stress 3 種追加)

既存の binary_trees / list_alloc / string_concat に追加で:

- **gc_combined** — 50k 要素配列を保持しつつ 10M 回の 4 要素配列 churn。
  「長寿命 + 短寿命チャーン」の **generational-friendly** 形 (今 libgc が
  非世代別なので差は出ないが、世代別 GC 投入時のベースライン)。
- **substr_churn** — 18 MB の text String を保持して、毎オフセットで
  `[i, 5]` slice。**fine-grained substring alloc + 1 long-lived**。GC
  回数は 52 と最低 (heap が text サイズで安定するため)。
- **fib_pair** — 再帰 fib が毎フレームで `[a, b]` 2 要素配列を返す。
  **frame-escape + deep stack** (depth 28、~317k フレーム peak)。precise
  GC を入れたとき frame iterator のスループットがここで効く想定。

各々 plain で ~1 s 持続、AOT 比 1.78〜2.74× 速い。perf.md §2 / §3 に
全 6 bench の表 (実測値 + 寿命プロファイル + GC 頻度) を整理。

## 2026-05-10 — A+B バッチ (`<=>` / `*` / `<<` / escape / AOT/PG verify / JIT 撤去)

### A — 残り P1 機能

- **`<=>`** (`node_spaceship`)。Int+Int / Str+Str は `-1`/`0`/`1`、
  混合型は `nil` (Ruby 互換)。`is_binop` / `alloc_binop` に追加。
- **`String#*` / `Array#*`** (`baruby_str_repeat` / `baruby_ary_repeat`)。
  `node_mul` を type branch に拡張。負の N は空。
- **`<<`** (`node_lshift`)。Int+Int は bit shift、Array は push、
  String は in-place append (`baruby_str_append`)。`is_binop` /
  `alloc_binop` に追加。`a << x << y << z` が左結合チェインで動く。
- **`p` の inspect 表示**。`baruby_print_value` / `to_s_inner` の String
  分岐で `\n` / `\t` / `\r` / `\\` / `\"` / `\xNN` (制御文字) を escape。
  prism の `unescaped` 経由のリテラル (`"a\nb"` 等) が
  正しく確認できるようになった (見た目は Ruby の `p` と同じ)。

### B — モード検証

- **AOT (`-c`)** 全 5 テスト + 3 bench 通過、plain と出力一致。新ノード
  (`node_str_lit` の `const char *` operand、`node_call_*`、`<=>` 等)
  も `code_store/SD_<hash>.c` 内で `EVAL_<name>(...)` 形に展開される。
  test_p1b のような複雑な script で SD は 1 ファイル内 inline 静的
  関数 ~400 個、public エントリ 4-5 個。
- **PG (`-p`)** も同様に通過。`PGSD_<hopt>.c` が出る。bench 結果は
  perf.md §2 に追記。
- **JIT (`-j`)** は `lstation.rb` ワーカーなしでは UDS 接続できないので
  パーサで `-j` 受信時に明示エラー + exit(1) させた。`astro_jit.c` の
  hooks は再有効化に備えて残置。

### モード別ベンチ結果 (perf.md §2 抜粋)

| bench         | plain  | aot    | pg     | aot 比 |
|---|---:|---:|---:|---:|
| binary_trees  | 0.96 s | 0.64 s | 0.94 s | 1.51× |
| list_alloc    | 1.16 s | 0.51 s | 0.50 s | 2.27× |
| string_concat | 1.02 s | 0.88 s | 0.88 s | 1.16× |

PG が plain と差が出にくい bench (binary_trees) は 1 回ループで
終わる構造 — prof-driven inlining 余地が小さい。alloc 量は libgc
の `GC_get_total_bytes` 由来で、モード間で不変 (~320MB / ~764MB /
~1.1GB)。

## 2026-05-10 — P1 言語拡張バッチ

`true` / `false` / `nil` リテラル、`to_s` / `to_i`、String 順序比較、
String / Array slice (2-arg `[]`)、文字列 interpolation を一気に入れた。

- **VAL_NIL を VAL_FALSE から分離** (raw 4 singleton)。`IS_FALSY` /
  `IS_TRUTHY` macro 追加、`node_if` / `node_while` を `IS_TRUTHY` 経由に
  書き換え (raw 4 は C 上 truthy なのでプレーン `if` だと nil が
  truthy 扱いになるバグを回避)。`IS_PTR` から VAL_NIL を除外。
- **`node_nil` ノード追加**。parser で PM_TRUE_NODE / PM_FALSE_NODE /
  PM_NIL_NODE を `node_true` / `node_false` / `node_nil` に流す
  (これまで全部 `unsupported` で死んでいた)。
- 既存の「nil 相当」フォールバック (if 無 else / 空 parens / 範囲外
  read / pop empty / aset auto-extend) を `VAL_FALSE` から `VAL_NIL` に
  切り替え。
- **`node_call_to_s` / `node_call_to_i`**。`baruby_to_s(v)` を node.c に
  追加 (libgc-backed StrBuf builder で配列の inspect 風文字列を組む。
  `open_memstream` + libc free は `free` macro shadow と相性が悪く
  leak 化するので使わない)。`p` 出力の inspect 表示と to_s top-level
  の string-without-quotes / nil→"" を分けて実装。
- **String 順序比較**。`baruby_str_cmp` を node.c に追加、`node_lt` /
  `node_le` / `node_gt` / `node_ge` を Int+Int / Str+Str の type branch
  に拡張。
- **`node_call_aget2`** (recv, idx, count)。String / Array 両方で
  サブスライス。clamp と negative index 込み。parser で
  `[]` の args_cnt==2 を分岐。
- **`PM_INTERPOLATED_STRING_NODE`**。parts 列を walk して、PM_STRING_NODE
  はそのまま、それ以外は `node_call_to_s` で wrap、左結合の `node_add`
  で連結。Empty parts は `""` 相当。`PM_EMBEDDED_STATEMENTS_NODE` も
  実装 (内側 statements を recurse、空 `#{}` は nil)。

検証は `test_p1.ba.rb` で全項目 (43 行)。fib / test_ary / test_eq の
regression なし、bench の alloc/GC も不変。

## 2026-05-10 — Ruby っぽい value semantics

`String#==` / `Array#==` / `Array#+` を実装、`true` / `false` を表示
できるよう singleton を分離。

- `baruby_value_eq(VALUE, VALUE)` を `node.c` に追加。raw 等価で
  fixnum / singleton / ポインタ identity を一発カバーし、違うときだけ
  String の byte 比較 / Array の再帰的要素比較に降りる。
- `node_eq` / `node_neq` を 2 段 fast path + helper に書き換え。
  int loop の hot path (`l == r` 直撃) は同じ命令数のまま。
- `node_add` の type branch に Array+Array (`baruby_ary_plus` で新配列
  を返す concat) を追加。
- `VAL_TRUE` を `INT2VAL(1) = 3` から **独立 singleton (raw 2)** に
  変更。`p (1 == 1)` が `1` ではなく `true` と表示されるようにし、
  `nil`/`false` と `true` が分かれるよう将来分離 ([todo.md](todo.md))
  への足場も用意。
- `IS_PTR` から `VAL_TRUE` を除外。`baruby_print_value` で `true` 表示
  対応。
- `PM_PARENTHESES_NODE` を実装 (空 `()` は `false`、それ以外は body を
  そのまま透過)。`(...)` を含む式が parser に通るようになった。

検証は `test_eq.ba.rb` で:
- 整数値比較 / mixed-type / String value-eq / Array value-eq
  (空・ネスト含む) / Array+Array (空配列・チェイン込み)。
- 既存テストの fib (10946) と test_ary も regression なし。
- 3 ベンチの alloc/GC 数は不変、wall は noise レンジ内。

## 2026-05-10 — 初期フォーク

`sample/naruby` から `sample/baruby` を切り出し、Array + String + libgc
を導入。GC testbed として独り立ちさせた。

### 言語面

- naruby の int64-only から **LSB-tagged VALUE** に拡張 (1 = fixnum、
  0 = ptr、raw 0 = false/nil)。
- ヒープ型 **Array (BaArray)** と **String (BaString)** を追加。
  共通 `ObjectHeader` に type tag。
- 比較 / `&&` / `||` を `VAL_TRUE` / `VAL_FALSE` 正規化に変更。
  既存の `&&` 実装が `node_num(0)` (= INT2VAL(0) = raw 1, truthy) を
  false 相当として使っていた潜在バグを修正。
- 専用ノード `node_true` / `node_false` 追加。

### ノード追加

- `node_ary_new` / `node_ary_push` — リテラル評価のチェイン展開用。
- `node_str_lit(const char *, uint32_t)` — eval 毎に fresh alloc。
- メソッド desugar 用 dispatch nodes:
  `node_call_size`, `node_call_aget`, `node_call_aset`,
  `node_call_push`, `node_call_pop`。型タグで Array/String を branch。

### パーサ

`PM_ARRAY_NODE` / `PM_STRING_NODE` の "unsupported" stub を実装に置換。
`PM_CALL_NODE` で receiver が non-NULL かつメソッド名が builtin 表に
ある場合は対応する dispatch ノードに lower。
`PM_OR_NODE` も実装 (`PM_AND_NODE` と同型)。

### 値表現と既存ノードの調整

- `node_num`: `INT2VAL(num)` で wrap。
- `node_add`/`sub`/`mul`/`div`/`mod`: untag → op → tag。`node_add` のみ
  string concat (`baruby_str_concat`) も runtime branch で受け持つ。
- `node_lt`/`le`/`gt`/`ge`/`eq`/`neq`: tagged 値のまま signed 比較
  (untag 不要)、結果を `VAL_TRUE`/`VAL_FALSE` に正規化。

### libgc 統合

- `context.h` で全 system header の後ろに `malloc` / `calloc` /
  `realloc` / `strdup` / `free` を `GC_*` macro で wrap (asom と同じ
  パターン)。
- `main.c` 冒頭で `GC_INIT()`。
- Makefile の link line に `-lgc`。
- `BARUBY_GC_STATS=1` で `__GC_STATS__` 行を出力 (alloc_bytes /
  heap_bytes / gc_count、libgc の `GC_get_*` 由来)。

### ベンチ

`bench/binary_trees.ba.rb` (depth 21、~1s)、`bench/list_alloc.ba.rb`
(10M iter、~1s)、`bench/string_concat.ba.rb` (5M iter、~1s)。
ランナー `bench/run.rb` が plain/aot/pg を選んで全 bench を順に実行、
時間 + GC 統計を表示。`make bench` でも一発実行可。

### 動作確認 (`--plain` のみ)

- `test.ba.rb` (fib 20) で再帰 + 整数演算 OK (10946)。
- `test_ary.ba.rb` で配列 / 文字列 / index / size / push / pop /
  concat の挙動が期待通り。
- 3 ベンチがすべて完走、時間が ~1s スケールで GC が走っていることを
  確認 (12〜1700 collections)。

AOT / PG / JIT モードでの新ノード動作は未検証 ([todo.md](todo.md) P0)。

### 削除した naruby 資産

- `naruby_codegen.rb` (本人コメントで obsolete)
- `naruby_code.c` (生成済み AST のテストダンプ)
- `lstation.rb` (JIT サーバ — `-j` 自体を unwired にした)

## 過去の経緯

baruby 命名: naruby = "**n**ot **a** ruby"、abruby = "**a b**it ruby"
の中間 — "**ba**rely a ruby" → baruby。
