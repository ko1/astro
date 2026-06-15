# koruby v2 ブロック / クロージャ設計 — env を slots ABI と moving GC に載せる

Status: **設計確定ドラフト** (2026-06-15)。実装未着手。長い設計議論の結論。
確定度: ✅ = 合意・設計から従う / 🤔 = 実装時に詰める perf 調整 / ❓ = M2 以降。

対象: M1 の block / yield / Proc / lambda / closure。eval / binding は **M2** だが、
設計が foreclose しないことを §10 で確認する。

関連:
- [v2_design.md](./v2_design.md) — slots ABI のコア(§7.8 lvar 一本、§13 #6 iterator)
- [v2_m0_status.md](./v2_m0_status.md) — 土台(M0): frame=直 slots、C スタック制御
- [closure_sp_model.md](./closure_sp_model.md) — v1 の破綻総括(本設計が排除する対象)

---

## 0. 結論を 1 枚で ✅

```
- frame: M0 のまま。own-local は slots[off] 直アクセス(fib に env 機構ゼロ)
- env: [prev | loc | vals] という単一 shape。生存中は frame の slots 内、
       escape 後は moving heap。アクセスは node->loc[idx]、チェーンは node->prev
- 「env はどーせすぐ消える」: 非escape の block は env オブジェクトを一切作らない
- escape(proc/binding 生成)した時だけ heap env オブジェクトを materialize(open)
- frame return = env が消える瞬間 = ここで close(slots→vals コピー、loc 差し替え)
- 複数 referrer は heap オブジェクト(安定 identity)を指して集約。close は中身差し替え
- moving GC が relocation を全部 fixup。CRuby が ep を手管理してた部分を GC に丸投げ
```

CRuby との対応: CRuby (non-moving) は escape 時 `vm_make_env` で env を heap 化し
`ep` を手で張り替える。v2 は **moving GC** なので、heap env の relocation・referrer
の追従を **GC が自動でやる**。手で要るのは slots→heap の close 1 ステップだけ。

---

## 1. 用語 ✅

- **env**: 1 スコープ活性化の変数置き場。`[prev | loc | vals]` 構造。
  - **open**: `loc` が **frame の slots 上の locals** を指す(生存中、コピー前)。
  - **closed**: `loc` が **自分の vals** を指す(escape して frame return 後)。
- **escape**: `proc{}` / `lambda` / `&blk` / `binding` が実行された瞬間。env が frame
  より長生きし得ることが確定する点。
- **close**: env が消える瞬間(その env を持つ frame の return)。slots→vals コピー +
  `loc` 差し替え。「escape を決めた瞬間」じゃなく「**消える瞬間**」にやるのが肝。
- **(depth, index)**: 変数参照を parse 時に解決した座標。depth=prev を辿る回数、
  index=その env 内の位置。
- **referrer**: env を指すもの(`Proc.env` / 子 env の `prev` / binding)。

---

## 2. frame レイアウト ✅

```
非捕捉スコープ (method = fib 等):
  slots: [ local0 | local1 | ... ]              ← env 機構ゼロ。M0 そのまま

捕捉スコープ (外側変数を使う/使われる block・proc):
  slots: [ PREV | LOC | local0 | ... | local_{n-1} | MY_ENV ]
           └────────── env 領域 (open form) ──────────┘
```

- **PREV**: 外側(lexical parent)の env を指す。outer 変数アクセスの起点。
  yield / proc.call 入口で設定。
- **LOC**: 自分の locals 先頭(`&local0`)を指す。子からの uniform アクセス
  (`node->loc[idx]`)用。生存中は自分の slots を指す。
- **MY_ENV**: 自分の env を heap 化した **オブジェクト**。**NULL のまま**で、
  escape した瞬間に生成。
- method(fib)は外側を捕捉しない(Ruby の method は closure じゃない)ので
  PREV/LOC/MY_ENV を持たない。**fib は完全に M0、env 機構の影響ゼロ**。

heap env オブジェクト `KorbEnv` も **同じ shape**:

```c
typedef struct KorbEnv {
    AroObjectHeader head;          // KORB_OBJ_ENV (moving heap)
    struct KorbEnv *ARO_GC_EDGE prev;   // 親 env (heap or NULL)
    VALUE *loc;                    // open: slots を指す / closed: &vals[0]。GC edge ではない(§7)
    uint32_t n;
    VALUE ARO_GC_EDGE vals[];      // closed 時に値が入る
} KorbEnv;
```

slots の env 領域と heap KorbEnv が同 shape なので、**env への参照は slots/heap を
問わず uniform に扱える**(これが yield と proc で body を共通化する鍵)。

---

## 3. アクセス ✅(✅ / caching は 🤔)

### 3.1 own-local

```c
slots[off]      // off は parse 時 bake。直アクセス、1 load。fib 無傷
```

捕捉スコープでも own-local は **直 slots**(LOC を経由しない)。env の loc とは
別腹。close 時に slots→vals へコピーされるが、それは frame 退出時なので own-access
は生存中ずっと直 slots で正しい(§5)。

### 3.2 outer 変数 (depth d, index i)

```c
KorbEnv *node = (KorbEnv *)PREV;   // 入口で設定済み
for (k = 1; k < d; k++) node = node->prev;
//   read : v = node->loc[i];
//   write: korb_env_store(node, i, v);   // §6 (WB)
```

- `node->loc[i]`: loc が slots(open)でも vals(closed)でも同じコード。
- depth 1(最頻 = block が直近 method の local を使う)は walk なし。
- 1 アクセス ≈ 2 load(loc, value)。CRuby の ep 相対と同程度。
- 🤔 perf: depth ごとの base を入口で 1 回解決して display 配列に置けば
  per-access の loc 間接を消せる(CRuby は per-access walk)。計測してから。

---

## 4. block 値の表現 ✅

block は first-class 値じゃないので **非 alloc**。呼び先 frame の隠しセル 3 つで運ぶ:

| セル | 内容 |
|---|---|
| `blk_iseq` | block 本体の immortal 記述子(NODE + arity)。奇数タグ immortal ポインタ(§7 で GC skip) |
| `blk_env` | 定義スコープの env(slots region or KorbEnv)。yield の outer chain 起点 |
| `blk_home` | 非局所脱出トークン(fixnum、§8) |

- block を渡さない呼び出しは `blk_iseq = nil`(= `block_given?` が false)。
- 隠しセルを持つのは yield / `block_given?` / `&param` を使う method だけ(parser 判定)。
- **`proc{}` / `lambda` / `&blk` でオブジェクト化した時だけ `KorbProc` を alloc**:

```c
typedef struct KorbProc {
    AroObjectHeader head;          // KORB_OBJ_PROC
    const korb_iseq_t *iseq;       // immortal(SCAN しない)
    struct KorbEnv *ARO_GC_EDGE env;  // 捕捉した env(= 定義スコープの KorbEnv)
    VALUE self;
    uint32_t flags;                // is_lambda 等
} KorbProc;
```

---

## 5. yield / escape / close ✅

### 5.1 yield(= callee が biseq の call。非escape の常態)

```c
korb_yield(c, slots, argc):
  blk_iseq が nil → LocalJumpError "no block given (yield)"
  base = slots - argc                 // staged 引数 = block params 窓(M0 の call と同型)
  block frame を base+locals_cnt に積む(常に top。v1 の A<B divergence は構造的に無い)
  block frame の PREV = blk_env、blk_home を継承
  body 実行
```

**非escape の block は env オブジェクトを一切作らない。** block の outer アクセスは
PREV(= 定義スコープの slots env 領域)を辿るだけ。`each { p a }` も `each { a += 1 }`
も **alloc ゼロ・WB ゼロ**(a は親 slots = root、直読み/直書き)。

### 5.2 escape(`proc{}` / `binding` 等が実行された瞬間)

捕捉チェーンを上に辿り、**各スコープに heap KorbEnv を open で確保**(まだ無ければ):

```c
korb_escape_chain(capturing_scope):
  for each scope S from capturing_scope up to outermost-captured:
    if S.MY_ENV == NULL:
      E = korb_alloc(KorbEnv, S.n)
      E->loc  = &S.slots.local0     // open: S の slots を指す
      E->prev = (S の親).MY_ENV     // 親も同ループで確保済み
      S.MY_ENV = E
  KorbProc.env = capturing_scope.MY_ENV
```

- escape 時点では **値は slots のまま**(loc → slots)。コピーしない。
- 複数 proc が同じスコープを捕捉 → `MY_ENV` を見て **同じ E に集約**(共有 mutation)。
- escape は **C primitive の中で実行時に起こる**(`korb_proc_new` / `korb_binding_new`)。
  alias / send 経由でも同じ C 関数に来るので **検出漏れしない**(§10)。

### 5.3 close(env が消える瞬間 = その frame の return)✅

```c
frame return (korb_call / korb_yield の戻り 1 経路):
  if MY_ENV != NULL:                  // escape したスコープだけ
    memcpy(MY_ENV->vals, &local0, n)  // slots → vals(§6 で WB)
    MY_ENV->loc = &MY_ENV->vals[0]    // open → closed
```

- referrer(Proc.env / 子の prev)は **E オブジェクトを指したまま**。loc が中身ごと
  差し替わるので open→closed は透過。**referrer を探して回る必要なし**(集約済み)。
- `a=0; [1].each{ $g=proc{a}; a=99 }; p $g.call` → block が a=99 を slots に書く →
  $g は MY_ENV->loc(open=slots)経由で同じ a を見る → close で 99 が vals に固定 →
  `$g.call` は 99。✅ 共有が正しい。

### 5.4 全 exit 経路で close(= v1 の地雷回避)✅

close は **正常 return / `next` / `break` / `return` / 例外**の全経路で漏れなく走る
必要がある。**frame の出入りを korb_call / korb_yield の 1 経路に集約**して、そこで
「MY_ENV != NULL なら close」を 1 箇所だけ書く。v1 は close が散らばって事故った。
RESULT で unwind する koruby では、unwind が通過する各 frame 境界(= korb_call /
korb_yield の戻り)で必ず close を通す。

---

## 6. GC / Write Barrier ✅

### 6.1 root / scan

- `PREV` / `LOC` / `MY_ENV` は slots 内 → **root として scan**。env オブジェクト
  (`MY_ENV` が指す)は moving GC が forward。`PREV`/`MY_ENV` が指す KorbEnv も同様。
- `SCAN_EDGES(KorbEnv)` は open/closed で切替:
  - **open**: `prev` のみ scan(値は slots = root 経由で scan 済み。vals は未使用)。
  - **closed**: `prev` + `vals[0..n)`。
- `loc` は **GC edge ではない**(open=slots 固定 / closed=自分の vals を指す自己ポインタ)。
  forward 対象にしない。relocation 時、**closed なら loc を新しい &vals に付け替え**、
  open なら slots 固定なので放置。

### 6.2 Write Barrier — DUMMY holder、NULL check なし ✅

own-local write は直 slots(root)なので **WB ゼロ**(fib・`each{a+=1}` 無傷)。
外側変数 write だけ WB 経路。

**holder は activation 入口で 1 回決める**(per-write 分岐しない):

```
入口: H = env_closed ? E : DUMMY    // yield / open は DUMMY、closed だけ実 E
write: ARO_STORE(c, H, node->loc + i, v)
```

- **DUMMY** = gc_flags=0(young/clean)の共有 immortal オブジェクト 1 個。WB の
  `(gc_flags & (OLD|DIRTY)) != OLD` が常に真 → early-out(remember しない)。
  root への write は scan されるので remember 不要 = 正しい。
- 入口で固定できる根拠: proc P 実行中、捕捉元 F の open/closed は不変
  (F が P の生きた祖先なら P が return するまで F は close しない)。
- **`holder == NULL` 分岐は使わない**。理由: NULL を predict-taken にすると
  heap write 側(WB が本当に要る側)で mispredict して遅い。**root は DUMMY を渡す**
  ことで WB を単一パス化(`*slot=v` → gc_flags チェック → remember)。
- copy backend は `ARO_STORE` = 素の store(WB 無し)。この最適化は generational
  backend(copy_gen 等)で効く。**M1 開発(copy)では挙動同じ**だが、規約として
  最初から「root は DUMMY、NULL 不使用」で統一しておく。
- ⚠ framework 影響: 共有 `runtime/precise_gc/gc.h` の `aro_gc_store` は今 NULL check
  を持つ(baruby_precise 由来「stack-root は holder=NULL」規約)。**NULL check 撤去は
  全 precise sample 横断の framework 変更**(baruby/ascheme の caller を DUMMY 化)。
  → 別タスク・user GO 待ち。koruby は「常に DUMMY/E を渡す(NULL 不使用)」で
  非侵襲に利得を取る(既存 gc.h のまま、NULL 分岐に入らないだけ)。

---

## 7. 値表現の追加 ✅

| 物 | タグ / 種別 | GC |
|---|---|---|
| `KorbEnv` | moving heap (KORB_OBJ_ENV) | open=prev / closed=prev+vals、loc は edge 外 |
| `KorbProc` | moving heap (KORB_OBJ_PROC) | env のみ(iseq は immortal) |
| `blk_iseq` 隠しセル | 奇数タグ immortal ポインタ `(ptr\|1)` | 奇数 = fixnum 扱いで skip |
| `blk_env` / PREV / LOC / MY_ENV | slots 内の VALUE / ポインタ | root scan(LOC は §6.1 の扱い) |
| `blk_home` | fixnum トークン | skip |

---

## 8. 非局所脱出 ✅

RESULT state を 2 つ追加。伝播(UNWRAP/CHECK)は無変更、境界だけが畳む:

| state | 発生 | 畳む場所 |
|---|---|---|
| KORB_NEXT | `next [v]` | korb_yield / Proc#call(→ NORMAL) |
| KORB_BREAK | `break [v]` | block を供給した call site(→ NORMAL、call の値) |
| KORB_RETURN(既存)| `return` | home トークンが一致する method 境界 |

- `blk_home` = `(frame_serial << K) | frame_base_slot_off` の fixnum。slots が固定
  mmap なので frame_base_slot がスコープ識別になる。serial で再帰中の同一 call を区別。
- break: block を渡す call node が自分のトークンを cell に載せ、BREAK が戻った call
  site が一致判定して畳む。return: block 定義時の enclosing method のトークン。
- 不一致のまま toplevel / Proc 境界に達したら **LocalJumpError**(escape した Proc から
  の break/return が CRuby と同じ失敗)。
- **全経路で close(§5.4)を必ず通す**ので、非局所脱出でも env の close は漏れない。
- 導入順: next(トークン不要)→ break(call site トークン)→ return-from-block / proc。

---

## 9. 完全な例 ✅

```ruby
a = 1
1.times{ b = 2
  1.times{ c = 3
    $g = proc{ x = 0; a + b + c + x }   # x=local、a/b/c=free var
  }
}
```

parse 時の解決(`$g` body から):`x→(0,xi)`, `c→(1,ci)`, `b→(2,bi)`, `a→(3,ai)`。

**escape 前**(inner block 実行中、`proc{}` 直前)— 全部 slots、heap オブジェクトゼロ:

```
slots: [ top:  a=1 ... ]
       [ outer:PREV→top  LOC→self  b=2  MY_ENV=nil ]
       [ inner:PREV→outer LOC→self c=3  MY_ENV=nil ]  ← 今ここ
```

**`proc{}` = escape** → 捕捉チェーン(inner→outer→top)に KorbEnv を open 確保:

```
heap (moving): E_top  {prev=nil,   loc→top の slots,   vals[未]}
               E_out  {prev=E_top, loc→outer の slots, vals[未]}
               E_in   {prev=E_out, loc→inner の slots, vals[未]}
       $g = KorbProc{ iseq, env=E_in }
slots: 各 frame の MY_ENV = 対応する E_*
```

**各 frame return = close**(LIFO: inner→outer→top):各 E_* の loc を slots→自分の
vals に、値をコピー。E_in.vals=[c=3]、E_out.vals=[b=2]、E_top.vals=[a=1]。

**`$g.call`**:

```
proc frame を積む。PREV = $g.env = E_in。x は proc frame の slots(own、直)
  x (0,xi): slots[off]              = 0   ← own、直 slot
  c (1,ci): E_in.loc[ci]            = 3   ← closed なので loc→vals
  b (2,bi): E_in.prev.loc[bi]       = 2   ← E_out
  a (3,ai): E_in.prev.prev.loc[ai]  = 1   ← E_top
  = 6
```

**対比**: `proc{}` を作らず `1.times{ p c }` だけなら、escape しないので **E_* は一切
生成されない**。block は PREV→親 slots を直に辿って c を読むだけ。

---

## 10. eval / binding(M2)— foreclose しないことの確認 ❓

drop-in 目標なので **今の仕様を壊さない**(eval/binding/reflection フル対応)。本設計は
これを foreclose しない:

- **whole-env が強制される**(per-variable subset は不可)。理由: eval は `alias` /
  `send` / `method(:eval)` 経由で呼べて **構文検出できない**ので、escape した env は
  「どの local も eval され得る」前提で **全 local を保存**するしかない。本設計の env は
  元々スコープの全 local を持つ(whole-env)ので OK。
- **検出は実行時(C primitive 内)**:`binding` / `eval` の C 実装が走った時に処理する。
  alias 不問(同じ C 関数に dispatch)。escape も §5.2 で実行時検出済み。
- **名前解決**: eval/binding は名前→index が要る。これは scope を導入した **immortal な
  NODE**(method なら node_def、block なら block literal NODE)に **local 名 ID 列**を
  持たせて引く(CRuby の env→iseq→local_table(ID 列)を、koruby では env→scope NODE→
  ID 列 でやる)。M1 では不要、M2 で NODE に足すだけ。**koruby に iseq は作らない**
  (AST walker、immortal は NODE)。
- **eval が新 local を作る**(`b.eval("y=1")` が binding に永続)→ eval に触れた env
  だけ実行時拡張可能な名前表。M2 の、しかも eval-touched env だけの話。

→ M1 は eval/binding 無しで実装。env 構造(whole-env)・実行時検出・名前表の席は
最初から空けてあるので、M2 で被せるだけ。

---

## 11. コスト早見 ✅

| ケース | env alloc | WB | own access |
|---|---|---|---|
| fib(捕捉なし method)| なし | なし | slots 直(1 load)|
| `each{ p a }`(捕捉 read・非escape)| **なし** | なし | a=親 slots 直(2 load: loc,val)|
| `each{ a += 1 }`(捕捉 write・非escape)| **なし** | **なし**(root)| 親 slots 直 |
| 捕捉なし block(`each{|x| p x}`)| なし | なし | x=own、直 |
| `proc{ }` / `binding`(escape)| escape したスコープに 1 個(共有)| closed 後の write だけ | proc.env 経由 |

- **env を作るのは escape した時だけ。** 非escape の block は read/write とも slots 直。
- escape しても **スコープ活性化ごとに 1 個**(iteration ごとじゃない、複数 proc で共有)。
- 非escape で死ぬ env(短命)は copying nursery で die-young = タダ。

---

## 12. v1 から消えるもの ✅

| v1 | v2 |
|---|---|
| env = stack in-place 共有 + sp A<B | env は [prev\|loc\|vals] 統一、yield は普通の top call |
| creates_proc clone + writeback | 消滅(各 iteration fresh frame、escape で別 E)|
| method_overlaps_env clone | 消滅(block は top で走る)|
| snapshot_env_if_in_frame(return 時 scan)| close(frame return で内部差し替え)1 箇所 |
| env_size 同居(curry バグ)| scope ごと独立 env + prev リンク |
| c->current_block(global 連鎖)| frame の隠しセル(per-frame)|
| escape 時の参照追跡 | referrer は安定 object を指す = 集約済み、loc 差し替えだけ |

---

## 13. マイルストーン(M1 内)🤔

GC は copy + STRESS/PURGE、AOT 常設(M0 と同じ gate)。

| 段 | 内容 | gate |
|---|---|---|
| **B0** | frame 出入りを korb_call / korb_yield の 1 経路に集約(close hook の置き場)| 既存 green 維持 |
| **B1** | capture 解析(parse)+ frame の PREV/LOC/MY_ENV セル + outer access(depth/index)| — |
| **B2** | yield + `block_given?` + next。**escape なし = env オブジェクト 0**。`def each_n` 級で駆動 | CAT=block 該当 green(STRESS+PURGE)|
| **B3** | `proc{}` / `lambda` / `&blk` / KorbProc / escape / close / Proc#call。**curry / nested-lambda 回帰テスト**(v1 env_size バグ枠)| proc_closure 系 green |
| **B4** | break / return-from-block / LocalJumpError(§8)| exception 系該当 green |
| 以後 | autosplat、numbered params(`it`/`_1`)、self/cref(class と同時)| — |

計測(gate でなく記録): 非捕捉/非escape が本当に alloc ゼロか、outer access の loc 間接
が hot loop に効くか(per-access vs entry-display §3.2)、escaped env の nursery 寿命分布。

---

## 14. 残る未決 ❓

| # | 論点 | 決め方 |
|---|---|---|
| 1 | outer access caching: per-access loc 間接 vs 入口 display(§3.2)| B2/B3 で bench |
| 2 | framework の `aro_gc_store` NULL check 撤去(横断、§6.2)| 別タスク・user GO |
| 3 | `blk_iseq` の奇数タグ vs KorbBlock heap 化 | B2 で audit/STRESS 後 |
| 4 | `blk_home` トークンの桁配分 / ensure(M1 後半)との交差 | break 実装時 |
| 5 | eval/binding 詳細・eval-added local の動的名前表(§10)| M2 |
| 6 | self / cref の block 継承 | M1 class と同時 |

---

## 付録: 却下した案と理由(設計の経緯)

到達までに潰した案。再提案を避けるため記録する。

1. **eager heap(env を常に heap)**: 性能 NG。→ 非escape は alloc しない方向へ。
2. **per-variable upvalue(Lua/clox の変数単位)**: Ruby は **eval が任意の可視 local を
   触れる**(alias で検出不能)ので escape 時 subset 保存が unsound。whole-env 強制。
   Lua が per-variable で済むのは `load` が enclosing locals を見ないから。
3. **fp(二本)/ reified frame**: own access を ep 相対(2 load)にして fib が遅くなる、
   かつ nested escape が suspended frame の ep を更新するため reified frame が要る。
   → own は直 slots を維持し、escape したものだけ heap object 化(本設計)。
4. **sp だけ + slots に直書きコピー(snapshot at escape)**: コピーで home が 2 つになり
   共有 mutation が壊れる(`i=0;f=proc{i};i=1;f.call` が 0 を返す)。→ move(参照張替)で
   解決、さらに move-at-exit(消える瞬間に close)で frame 自身の repoint を不要化。
5. **birth-in-heap(捕捉スコープは最初から heap env)**: 非escape の `each{acc}` でも
   alloc する。→ escape 時だけ materialize に(non-escape タダ)。
6. **WB の holder==NULL 分岐**: heap write 側で mispredict。→ DUMMY holder + 単一パス。

核心の転換点: **「v2 は moving GC」**。CRuby (non-moving) が手管理してた env 移動・
referrer 追従を GC に丸投げでき、手で要るのは close 1 ステップだけになった。
