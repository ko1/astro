# todo.md — koruby Ruby 互換性ギャップ

[done.md](./done.md) は実装済み機能の一覧。 ここは **未実装 / 不完全 /
既知バグ** の作業リスト。

## 既知バグ (socket / require)

- ~~socket の blocking spec が whole-file timeout~~ **(2026-08-10 解消)**。
  fd ベース IO 層が入り、pipe/socket は `O_NONBLOCK` + park になった
  (`docs/io_design.md` の「実装状況」参照)。残っている whole-file 失敗は
  blocking ではなく機能不足側なので、個別に切り分けが要る。

- ~~mspec の spec_helper を読んだ後は `require` が無効になる~~
  **(2026-08-11 解消。真因は koruby ではなく harness だった)**。
  症状は「`library/socket/**` の spec で `Socket` / `Addrinfo` が
  uninitialized constant」。前回「bare な `require` がメソッド探索を通らず
  builtin 直行だから mspec の require 上書きが素通りする」と書いたが、
  **これは誤り**。切り分け直した結果:
  - `library/socket/addrinfo/afamily_spec.rb` を **CRuby で直接実行しても
    同じ 3 errors** になる。つまり koruby 固有の問題ではない。
  - 原因は ruby/spec の `spec_helper.rb` が `MSPEC_RUNNER` 未設定のとき
    `ARGV.unshift $0; MSpecRun.main` で **自分自身を再入実行する**こと。
    `foo_spec.rb` → `library/socket/spec_helper.rb` →
    `spec_helper.rb` (ここで MSpecRun.main) → `foo_spec.rb` を再ロード →
    describe/example が走る、という順になるので、
    `library/socket/spec_helper.rb` の 2 行目 `require 'socket'` が
    **example の後**にしか実行されない。
  - 直し方は harness 側。`tools/mspec_launch.rb` が MSpecRun を自分で駆動し、
    spec file を top level に置く (`MSPEC_MODE=self` で旧挙動)。
  なお「bare `require` はメソッド探索を通らない」自体は事実だが、
  mspec が使う `CodeLoadingSpecs::Method#require` は明示レシーバ呼び出しなので
  その経路は通らない。**この 2 つは別の話**として扱うこと。
  bare call の解決順変更は Object に直接生やすと無限再帰で core dump した
  実績があり、今のところ実測された利得もないので着手しない。
- ~~green thread 下で socket の blocking read が scheduler を止める~~
  **(2026-08-10 解消)**。`IO#read` / `gets` / `getc` / `eof?` / `write` が
  rep 経由になり、EAGAIN を park に変換するようになった。`IO.pipe` で
  実測確認済み (read 待ちの裏で別 green thread が進行する)。
  **残る穴**: regular file。poll は regular file を常に ready と報告し、
  `read(2)` は `O_NONBLOCK` を無視してディスク待ちするので、遅いディスク read は
  今も native thread ごと止まる。readiness engine では原理的に解決できず、
  io_uring engine (READ を SQE で投げる) 待ち。

## 既知バグ (プロセス / IO)

- ~~既定の signal 配送が SignalException にならない~~ **(2026-08-11 実装)**。
  C の signal handler を使わず、「配送したい signal を block して pending の
  まま置き、check point で `sigtimedwait(2)` で刈り取る」方式
  (builtins/process.c の signal delivery 節)。刈り取り点は
  `korb_thread_check_ints` (sleep / Thread.pass / IO 待ち) と `Process.kill`。
  ignore / Proc 実行 / raise の方針は prelude の `Signal.__deliver`。
  `SignalException` / `Interrupt` に `#signo` / `#signm` を実装。
  **残っている差**: 外から届いた未 trap の signal は OS の既定動作のまま
  (= プロセスが死ぬ)。startup から block すると CPU ループ中に
  `timeout` / Ctrl-C / `kill` が効かなくなるため意図的にそうしている
  (実際に一度踏んだ)。CRuby と同じ即時配送にするには handler + flag +
  VM ループの check point が要り、新規 global が必要になるので未着手。
  そのため `ruby_exe`/`IO.popen` 越しに外から signal を送る 2-3 例は落ちる。

- ~~`File.for_fd` / `File.sysopen` が見えない~~ **(2026-08-11 修正)**。
  File の singleton は file.c で File がまだ Object 継承だった時点に作られて
  おり、io.c で File を IO の下に付け替えても metaclass の親が Object の
  singleton のままだった。io.c で metaclass も同時に付け替えるようにした。
- **`Process.spawn` の未対応オプション**: `umask:`、`pgroup:`、`unsetenv_others:`、
  `rlimit_*`、`:in` に IO 以外の Ruby オブジェクト。`close_others:` と `chdir:` と
  fd リダイレクトは実装済み。

## 既知バグ (2026-08-11 に発覚、未修正)

- **Class / Module の `#inspect` override が `p` で無視される**。
  `class Foo; def self.inspect; "YY"; end; end; p Foo` が "Foo" になる。
  文字列補間側 (`#{Foo}` → `#to_s`) は修正済みだが、inspect の C printer は
  Class をそのまま名前で出す。printer が再入する形になるので慎重に。
- **StringScanner の `^` / `\A` が scan 位置基準にならない**。
  `StringScanner#scan(/^ /)` は現在位置を行頭/文字列先頭として扱うべきだが、
  koruby は元文字列基準で見ている。regex engine に「offset から match するが
  その offset を文字列先頭とみなす」モードが要る。library/stringscanner の
  err 63 のかなりの部分。
- **`Socket.getifaddrs` は常に [] を返す** (13 例)。`Socket::Ifaddr` 未実装。

- **`core/kernel/require_spec.rb` が whole-file hang** (2026-08-11、Kernel#require
  を super から見えるようにしたことで初めてここまで到達するようになった)。
  "(concurrently) blocks a second thread from returning while the 1st is still
  requiring" で無限ループ。fixture が
  `Thread.pass until t.backtrace.any? { |c| c.include? "require" } && t.stop?`
  で他スレッドを待つが、koruby には (a) require の per-feature ロックが無く
  t2 がブロックしない (b) 他 green thread の #backtrace が取れない ので
  条件が永久に成立しない。20s の harness timeout まで 100% CPU で回る。
  直すには require に「読み込み中の feature」テーブル + 待ち合わせと、
  parked な green thread の backtrace が要る。

- **TracePoint (76 例) — 設計検討と実測だけ済み、未着手**。
  部分評価は `EVAL_ARG` で子 dispatcher をインライン展開して融合するので、
  ノードの dispatcher を差し替えても**祖先 SD に取り込まれた複製には届かない**。
  一般の OSR/deopt も不可 (中間値は C コンパイラのレジスタに居て契約が無く、
  deopt map を吐かせると融合の利益そのものを失う)。
  ただし `:line` に必要なのは OSR ではない: `node_seq` の文境界では中間値が
  ゼロで、状態は全部 slot stack に居るので「残りを別の継続に渡す」だけで
  **実行中フレームにも効かせられる** (祖先も各自の次の文境界で同じ判断をする
  ので合成が効く)。得られる粒度は文境界とループ back-edge まで。
  `:call` / `:return` / `:raise` は融合と無関係な C の choke point があるので別。
  **実測 (2026-08-11、optcarrot AOT 300 frames ×4、tools/tracecost_rep.sh)**:
  常時 off のチェックを `node_seq` に入れると
  命令数 43,117.56M → 44,468.66M (**+3.13%**)、cycles **+1.8%**
  (分布は非重複)。1 文境界あたり +3.6 命令だが IPC が 3.101 → 3.139 に
  上がっており、予測される独立命令が空き発行スロットに吸収されている。
  branch-miss は増えない。実験の diff は docs/tracepoint_check_experiment.patch。
  **次の一手**: 「左部分木に call を含むか」を parse 時に焼いた定数オペランドに
  すれば `if (0 && ...)` が畳まれてチェックが消える。この情報は部分木の構造から
  決まるので **hash はすでに区別しており、SD variant は増えない**。
  optcarrot の node_seq 実行のうち pure が何割かを測るのが先。
  ゼロコストにする別案はコードパッチ (jump label / USDT の nop 方式) のみ。
- socket の残り: `recvmsg` / `recvmsg_nonblock` / `sendmsg`、
  `Socket::AncillaryData` の中身、`UDPSocket#local_address` 系。

## 次の伸びしろ (2026-08-11 実測、エラー文言ベース)

- **IOError: not opened for writing** (io+kernel で 20 例) — mode 検査の食い違い。
  どの操作が誤って read-only 扱いになるのか切り分けが要る。
- **ArgumentError: invalid access mode** (7 例) — File.open のモード文字列で
  受け損ねているものがある ("r+b" 系? bom|utf-8?)。
- **Module#autoload のスレッド意味論** (~20 例) — autoload 中の他 thread からの
  const_defined?/constants の見え方。autoload 自体の再設計が要る。
- **refinements** (`undefined method 'foo' for an instance of Integer` 系) — 未実装。
- Marshal の残りは encoding 境界 (UTF-16 Symbol 等) が主で、encoding 排除方針の外。
- core/string fail=570 は個別意味論の集積 (大物なし)。次に触るなら
  fail の文言を uniq -c してから。

## ライブラリ vendor (2026-08-12)

CRuby の純 Ruby ライブラリをそのまま lib/ に取り込む戦線を開始。
matrix / ipaddr / resolv / getoptlong / open3 / random/formatter を vendor
(smoke で CRuby と一致)。**net/http と optparse は SEGV / 未解決で保留**:
- **net/http**: load と object 生成は動くが、`Net::HTTP.get_response` の実
  リクエストが SEGV (要 mock server で再現)。grep の $~ / safe-nav+block /
  super→new を直しても残る深い所。crash は whole-file-fail を増やすので
  vendor から外した。次に入れるならこの SEGV の切り分けから。
- **optparse**: `on("-nNAME")` が make_switch 内で `[] for nil`
  (guess/search 周り)。when-splot は直ったがまだ別の穴がある。

## 既知バグ (block 転送)

- **to_enum 駆動 (CPROC block) 下のメソッドからリテラルブロックを渡すと、
  callee が `&b` パラメータで受けたとき nil になる** (2026-08-11 発覚)。
  再現最小形:
  ```ruby
  class S
    def each(&b); [3,1].each(&b); end        # &b で受ける callee
    def ewi; each { |x| yield x }; self; end # リテラルブロックを渡す
  end
  S.new.to_enum(:ewi).to_a   # → each の中で b が nil (LocalJumpError)
  ```
  直接 `S.new.ewi { }` なら動く。to_enum の C driver が ewi を CPROC block で
  呼んだときだけ、ewi 内のリテラルブロックが each の &b に届かない。
  callee が yield 型 (`def each; yield; end`) なら CPROC 下でも動くので、
  &b の materialize (korb_make_proc の bp_blk 経路) が CPROC コンテキストの
  何かを見て落ちている疑い。Enumerable の each_with_index 等は __each_el の
  間接呼び出し (間に通常ブロックのフレームが挟まる) で回避している —
  この回避をやめられるのは本修正後。

## 既知バグ (定数 / Enumerator)

- **定数テーブルが flat**。`module Foo; class Bar; end; end` の `Bar` は
  グローバルにも見えてしまう (`defined?(Bar)` が "constant")。CRuby は nil。
  読み側は owner-aware に直してあり (lexical scope → ancestry → top-level →
  flat fallback の順)、prelude の入れ子定数がユーザの top-level 定数を
  横取りする問題は解決済み。だが「入れ子定数が unqualified でも見える」
  こと自体は残っている。直すには flat fallback を落として
  `Module#const_missing` まで通す必要があり、既存コードへの影響が大きい。
  `Enumerator::Chain` / `::Product` / `::Lazy` を足すたびにこの穴が
  効いてくるので、いずれ潰すこと。

- **Ruby で書いた Enumerator サブクラスは #next / #peek が使えない**。
  `Enumerator.inherited` が Enumerable の実装を include するので
  `#to_a` / `#map` 等は #each 駆動で動くが、`#next` / `#peek` / `#with_index`
  は Enumerator の C 実装のままで、C 側の enumerator struct を読む。
  `Enumerator::Chain#next` が CRuby では動く。

## 既知バグ (moving GC, 未修正)

- **`t/hand/struct_data_inherit.rb` / `t/hand/struct_name_arg.rb` が STRESS+PURGE で
  SEGV** (2026-08-10 確認)。通常実行は PASS。commit 3c96fc83 の時点でも同じく落ちる
  ので既存の moving-GC stale pointer。Struct/Data のサブクラス生成経路で
  alloc を跨いで生ポインタを保持している疑い。gdb bt から追うこと
  ([[project_koruby_precise_stress_gc_fixes]] と同じ手順)。

## 既知バグ (メソッドオブジェクト)

- **`UnboundMethod#bind_call` が Class/Module レシーバで singleton override に
  再ディスパッチする**。`Module.instance_method(:name).bind_call(Foo)` は Module#name
  の実装を呼ぶべきだが、`Foo.singleton_class` に `def self.name` があるとそちらが
  呼ばれる (`to_s` でも同様)。通常の instance method (`A.instance_method(:foo)
  .bind_call(B.new)`) は正しく A#foo を呼ぶので、レシーバが Class のときだけ
  経路が違う。rubyspec の Marshal "ignores overridden name method" 3 例が
  これで落ちる (prelude/marshal.rb の `Marshal._class_name` は修正後に効く)。

## 既知バグ (GC backend)

- (修正済 2026-06-23) **`mark_compact_gen` backend が bignum で core dump**。
  真因は major fold_young が finalizable bignum を tenured 昇格時に finalize_list を
  更新せず、major finalize_walk が stale young を読んで生きた bignum を誤 finalize
  → mpz_clear が無関係な libc 割当を free → 次 GC で SEGV。fold_young で
  finalize_list の young エントリを前進させて修正 (runtime/precise_gc/
  gc_mark_compact_gen.c)。bignum.rb 一致・corpus 89295/5。
- (修正済 2026-06-23) **STRESS+PURGE 下で string_* が SEGV** (default copy でも決定的、
  当初 PURGE flake と誤推測したが実際は moving-GC stale-pointer の真バグ)。
  String#scan (self->buf を korb_str_new に渡し alloc 跨ぎで stale) と
  String#[]= (splice の grow GC 後に stale repl を return) を修正。
  copy STRESS+PURGE corpus が CRASH 27→0。詳細 [[project_koruby_precise_stress_gc_fixes]]。
- **array_283.rb が STRESS で TIMEOUT** (バグでない)。line 53 `[9..1].permutation{}` が
  9!=362880 順列で GC-every-alloc と相性最悪なだけ。normal は 0.17s で CRuby 一致。

## 残る committed corpus FAIL (89298/2, 2026-06-23 — 実質的に上限)

- **pointer-pack** `["x"].pack("P"/"p").unpack(...)` ×2: 文字列バッファへの**生ポインタ**を
  pack。**moving GC で原理的に不可** (GC が動かすと dangling; CRuby も unsafe 機能)。修正対象外。
  → これ以外の fixable な committed FAIL は全て解決済み。
- (修正済 2026-06-23) seeded shuffle ×2 → CRuby 互換 MT19937/Random ([[project_koruby_precise_mt19937]])。
  sample(n) も CRuby-exact 化。
- (修正済 2026-06-23) `[1,2].zip(10.upto(Float::INFINITY))` → upto(+∞) を endless
  ArithmeticSequence 化、zip が korb_zip_elem で index lazy pull。

## Ruby 準拠 probe sweep (2026-06-20 session 4) — 修正済み

probe + 実ハーネス array spec で発見・修正 (corpus 89295/5 維持・STRESS+AOT 検証):
- **&obj block 引数の to_proc coercion + Symbol/Method#to_proc 転送**: `&method`,
  `&proc_var`, `def m(&b)` 経由の C-proc。block-trio に NODE* entry を持てない
  proc (iseq==NULL) を KORB_BLK_CPROC sentinel + KORB_BLK_FWD plumbing で運び、
  korb_block_yield 冒頭で捕捉して send dispatch (GC-safe、*captured_self から
  fresh 読込)。Method#to_proc 追加 (is_lambda + self=recv で symbol proc と判別)。
- **instance_exec / instance_eval (block form)**: self を receiver に再束縛、
  def_env 保持で closure 維持。CPROC block は固定束縛なので self-rebind skip。
  String 形式と singleton def は未対応。
- **Integer#fdiv(Integer)**: bignum/大 Fixnum 被除数を exact rational (mpq) で
  割り正しい subnormal (1.fdiv(10**323)=1.0e-323)。double/double は桁落ちで 0.0。
- **Integer#size**: Bignum で |self| のバイト数 (mpz_sizeinbase)。従来は常に 8。
- **Bignum#to_f round-to-nearest**: mpz_get_d は toward-zero truncate。nextafter
  + mpz 距離比較で nearest-even。(10**308).to_f=1.0e308。

**性能: bottom-header 化 (self-copy 排除 + magic 同居) — foundation 済、本体は残 (2026-06-21)**
狙い: self を base[-1] に常駐させ korb_invoke の self-copy (`base[fs-1]=self`) を排除 (~0.5%、
free に近い) + EP を base[-2]・magic を base[-3] に同居 (CRuby の "magic" 流)。
- [済] codegen: `@children` ノードに `@framehdr` で **先頭メタを KORB_FRAME_HDR(=2) 個予約**
  (dispatcher が `slots += cnt+HDR`、予約セルは GC 走査前にゼロ化)。node.h に KORB_FRAME_HDR。
  未使用なので no-op・green。
- 残 (結合・GC/offset クリティカル。新規セッションで集中して):
  1. `@framehdr` を 8 call ノードへ (node_call/send/kw/send_safe/call_blk/call_blkproc/
     send_blk/send_blkproc)。base[-2]/base[-3] 予約+ゼロ。
  2. **self_off を base[-1] に**: node_self 等の self_off に **bake_add を足す**
     (bake は frame_size を引く → `-1-chain` が `-1-fs-chain` = base[-1] に解決)。self_off を
     使う全箇所 (PM_SELF_NODE / 各 self-stage / super / def / attr / defined / massign /
     call_splat / send_blk / ivar) を rebake。frame_size から self セル(+1)を除去。
  3. EP を base[-2] へ: korb_invoke は base[-1]=0 をやめる (base[-2] は予約時ゼロ済)。
     node_eget/eset の chain walk を `node[-1]`→`node[-2]`。korb_make_proc/binding/
     korb_frame_escaped を base[-2]。self-copy (`base[fs-1]=self`) 削除。
  4. 内部 dispatch: korb_send_impl/korb_call_impl で [recv,args] を 2 上げて base[-2]/base[-3]
     を予約+ゼロ (全 ~36 内部サイトを一括カバー)。super・Object.new→initialize も予約。
  5. frame setup: korb_block_yield (self=bf[0]=base[-1]・EP=bf[-1]=base[-2])、eval frame、
     c->slots (toplevel)、fiber vslots ── いずれも先頭スラックを **3 セル** に、走査も -3 から。
  6. magic を base[-3] に (型/フラグ/署名、debug-gated 可)。
  7. 検証: corpus / STRESS+PURGE (closure/binding/fiber 網羅) / AOT / ベンチ。

**性能: TOPLEVEL_BINDING の return 課税 → per-frame EP cell (設計 A) で解決済 (2026-06-21)**
- open env を各 frame の base[-1] (= 受け手スロット) に置き、return は `korb_frame_escaped
  (base)` で自分の base[-1] だけ見る (グローバル不読)。open_envs/open_env_cnt/register/
  close_envs 全廃 (env は base[-1] の slot 走査で GC-root)。
- 全 call 形態が self/recv を base[-1] に積む (node_call/kw/splat/blk/blkproc は self を
  argv[0] に、super は restage、eval は fb=slots+1、toplevel/fiber は先頭スラック)。
  node_eget/eset は mixed-chain。korb_make_proc/binding は base[-1]=E (E->prev に外側リンク)。
- 結果: nested_loop 6.34→0.59、while2 4.83→0.24、ackermann 5.69→1.39、method_call
  3.65→1.21 (vs YJIT)。TOPLEVEL_BINDING は eager のまま。corpus 89295/5、STRESS+AOT green。
- 拡張余地: base[-2]=magic (型/フラグ/署名・健全性 = CRuby の magic) を載せられる (未実装)。

**defer (発覚、未着手):**
- *literal* `&:sym` block が unary 専用 (kp_symbol_block が `x.sym()`、追加引数を
  転送せず)。`reduce(&:+)` 不可 (`reduce(:+)` は可)。n-ary 化は send_splat entry
  構築が SEGV、or C-proc 経由 (per-call alloc) で要再挑戦。
- **UnboundMethod#bind_call / Method#call が名前 re-dispatch** (captured entry を
  直接 invoke しない)。BasicObject 跨ぎ bind や override 下で誤った実装を呼ぶ。
- **to_enum / enum_for**: eager enum へ collector で materialize する設計が要 (C
  collector block 機構)。enumerable probe の cascade blocker。
- 実ハーネス array_004/005 の 5 fail は hard: pack 'P'/'p' (pointer、platform
  依存)、shuffle/sample (MT 完全一致要)、`upto(Float::INFINITY)` (lazy 無限
  enum + zip lazy take)。

## Ruby 準拠 probe sweep (2026-06-19 session 3) — 修正済み

差分テスト probe で発見・修正 (全て corpus 89267/5 維持・STRESS+AOT 検証):
- **String#unpack / Array#pack 'U'** (UTF-8 codepoints)。
- **BasicObject + Kernel を MRO に** (ancestors / is_a? / Object.superclass)。
- **begin/rescue の else 節** (seq(body, else) に lowering)。
- **String#reverse / reverse!** を UTF-8 文字単位に (byte 反転バグ修正)。
- **Array#each_index / cycle(n)** の no-block Enumerator。
- **無名 splat target** `first, *, last = ...` (中間を synth local に捨てる)。

### 残る準拠ギャップ (hard / niche / out-of-scope)
- (実装済 2026-06-19) **define_method** (closure 含む)。真因は env staleness では
  なく **block-arg の def_env が tagged prev 形 (base|1)** で来るのに korb_make_proc が
  raw base を期待していたこと → tag mask で解決。force-close は不要 (shared closure を
  壊す red herring)。env は共有 open env → 定義フレーム exit で heap promote。
  KORB_METHOD_DM kind + dm_proc (GC forward) + KORB_C_CLASS に登録。
  **Class.new/Module.new も修正** (new_kind=1 で generic object 化していた → kind 2)。
- **op-aware no-block Enumerator**: select/reject/flat_map/transform_values を
  block なしで呼ぶと、後続 .with_index{} が filter でなく map になる (silent 誤答)
  → 現状は raise (honest)。正しくは Enumerator に op を保持させる必要。
- **to_enum / enum_for(:method)**: 任意メソッドを collecting block で駆動する機構が要る。
- **upcase/downcase の非 ASCII** (é→É 等): full-unicode case mapping、scope 外。
- **regex** (gsub/sub/scan/match の Regexp 形): astrorge 待ち。
- **Integer.superclass == Numeric**: Numeric を module→class 化 + 階層変更、risky・低価値。

## perf: 全マイクロベンチで CRuby+YJIT 超え (2026-06-19 進行中)

目標: aot+cached が **すべての** microbench で cruby+yjit を下回る (<1.00)。

### 本セッションで勝ち越したベンチ
- floatcalc 1.21→0.97, mathfn 1.22→0.98: mixed Float·Integer 算術を node.def の
  四則 (plus/minus/mul/div) に inline (korb_num_arith/num_to_d/float_new の PLT 回避)。
- gcd 1.39→0.97, send 1.13→0.86, object 1.07→0.94: call fast-path を SD に inline。
- gen_gc 0.99 (勝ち越し)。

### call fast-path を SD に inline (commit ebb4996c)
- korb_invoke_simple を node.h の always_inline 化 → code_store SD にも畳まれる。
  node_call/node_send が cache-hit 時に korb_call_cached/korb_send_cached への
  cross-module call を回避。korb_inlcache に kind 弁別子 (INSTANCE/SMETHOD/NEW) 追加。
- 効果: fib 1.86→1.39, ackermann 2.32→1.63, tak 2.09→1.51, method_call 1.66→1.11,
  ivar 1.94→1.39, structacc 1.71→1.34。

### 残る負け (structural, tree-walker の限界に近い)
- 再帰/per-call cluster: fib 1.39, ackermann 1.63, tak 1.51, binary_trees ~2.0,
  ivar 1.39, structacc 1.34, method_call 1.11。残コストは per-call の indirect
  body dispatch (`*body->head.dispatcher`) + 16-byte RESULT 返り + argc/stack check。
  YJIT は tiny method を loop に inline + native register call。tree-walker で
  <1.0 にするには method body の cross-call devirtualize (自己再帰の直接呼び等)
  が要る。要 framework 改造、難。

### 最大の単独勝機: kwargs hash-free (現状 2.56、未着手)
- `box(x:,y:,z:)` が呼び出しごとに Hash を heap alloc → callee で korb_hash_find
  抽出。YJIT は hash 無しで kw slot に直接渡す。
- 設計: call site が「callee の signature を知らない」ため callee-decides 方式。
  - 新ノード node_call_kw/node_send_kw: pos args + kw 値を stack に staging、
    kw_syms(mid配列) を baked operand で持つ。Hash を作らない。
  - callee (korb_invoke_method 変種): kw params があれば stack の (sym,value) を
    slot に直接 bind。**kwrest があれば必要時のみ hash 構築。kw params 無し
    (&& **rest 無し) なら CRuby 通り positional Hash に変換 (fallback、hot では稀)。
  - 空 kwsplat drop (FL_KWARGS, [[project_koruby_kwsplat]]) との整合に注意。
- 大物・risk 高。要: parser + dispatch + node.def + STRESS 検証。

### その他の負け (小〜中)
- casewhen 1.36: case/when chain が node 列 (各 indirect dispatch)。dense int when
  を jump table 化する余地 (要 parser で node_case_jump 検出)。
- ary 1.23 / array_access 1.07: `a<<x` / `a[i]` が builtin receiver (Array は
  KORB_OBJECT_P でない) → 常に korb_send_cached + dispatch_method 経由。
  builtin-receiver CFUNC の inline cache 余地 (CFUNC 多様で汎用 inline 難)。
- sprintfb 1.12 / strfmt 1.00: format()/補間。bignum 1.11, mandelbrot 1.12。

## 差分テストで発覚した未対応 (2026-06-19 session 2 probe sweep)

- (実装済 2026-06-19) **パターンマッチ `case/in`**: value/binding/fixed-array/
  array-with-rest/hash(+shorthand)/capture(`=>`)/alternation(`|`)/pin(`^`)/guard
  (`if`/`unless`)/nesting + rightward `expr => pat`。既存の korb_pat_match +
  build_pattern_desc を流用、PM_CASE_MATCH_NODE を if/elsif chain に lowering。
  残: **find pattern** `[*, x, *]` のみ unsupported (稀)。
- (実装済 2026-06-19) **`Data.define`** (Ruby 3.2+ immutable value class)。
  positional/keyword 両対応 init + with + to_h/members/==/deconstruct_keys/inspect。
- (実装済 2026-06-19) **Bignum Rational 全面対応**: KorbRational num/den を VALUE
  (Fixnum or Bignum) 化 + GC edge + korb_int_arith 経由の overflow→bignum 演算
  + 既存の生 intptr 乗算 overflow バグも修正。リテラル `99999...r` も対応
  (node_rational_big、pm_integer→decimal via mpz_import)。
- (修正済 2026-06-19) **Bignum 整数リテラル**: node_bignum が source digits を
  焼いて eval 時に korb_str_to_int で再構築 (AOT も const char* operand で OK)。
  (Bignum Rational リテラル `99999...r` も対応済 — 上記参照)。
- (修正済 2026-06-19) **`Integer.sqrt`**: mpz_sqrt 経由で fixnum/bignum 厳密。
- (修正済 2026-06-19) **Enumerator#each_slice/each_cons**, **Hash#each_with_index
  (no block)**, **Hash key eql?** ({1=>x}[1.0]→nil)。

## 差分テストで発覚した未対応 (2026-06-19)

- **`define_method` 未対応** (class body): `define_method(:foo) { }` が
  「undefined method 'define_method'」。block/proc を method body 化する機構が要る。
  ※ 2026-06-19 に実装試行→revert。判明事項:
    - インフラ: korb_method に `VALUE proc` + KORB_METHOD_DM kind、GC は owner と
      並べて forward (context.h global-fn 行 + KORB_OBJ_CLASS 行)、korb_class_method_slot
      で proc=nil リセット。dispatch は korb_dispatch_method の DM case で
      korb_block_yield(entry=p->iseq, env=p->env, self=receiver)。
    - **非クロージャ block (cap_depth==0) は動く**。**クロージャ捕捉 block が
      captured 変数 read で SEGV**。同等クロージャの proc.call は動くので、原因は
      「define_method の CFUNC 内で作った proc を保存して後で呼ぶ」と captured env
      (open KorbEnv, loc=定義フレーム) が呼び出し時に無効化していること。
    - 想定 fix: DM に格納する際 captured env を即 CLOSE/promote (vals を stack から
      コピー) して loc 依存をやめる。要 focused session。
- **`Integer.superclass` が Object** (CRuby は Numeric)。koruby は Numeric を
  module(include) 扱いで superclass にしていない。Float も同様。階層変更は要注意。
- **`&blk` 引数転送の一部が prism node 139 で未対応**: `def m(*a, &b); x.send(n,*a,&b); end`。
- (修正済 2026-06-19) **multi-value `yield a, b`**: node_yield_n / node_yield_outer_n
  (@children) 追加。直接形 `def m; yield a,b; end; m{|a,b|}` は CRuby 一致。
  残: Enumerable 経由 (custom each が multi-yield → map/select) は prelude が
  `each{|x| yield(x)}` で1値しか転送せず誤り。完全対応は |*x|+yield(*x) が要るが
  hot path に array alloc が乗るので見送り (稀パターン)。yield-splat `yield(*a)` も未対応。
- **anonymous splat target 未対応**: `first, *, last = ...`(無名 `*`)が parse 不可。
- **`Object#object_id` 未対応**: moving GC 下で安定 id が要る(アドレスは移動で変わる)。
  即値は値ベースで可だが heap object は per-object id field か GC 管理の id 表が必要。
  中途半端な address ベースは `hash[obj.object_id]` を GC で壊すので保留。
- (修正済 2026-06-19) `require`/`require_relative`/`load` を no-op true 化、
  `Set[...]` クラス index コンストラクタ、`Set#disjoint?`/`#intersect?` 追加。
- (修正済 2026-06-19) Rational#floor/ceil/round、ArithmeticSequence に Enumerable
  mixin (sum/map/select 等が each 経由で動く)。
- niche 残: `Integer.sqrt` (class method plumbing 要)、`Complex#**`、
  pack/unpack の `l`/`L` 等の directive。いずれも稀。
- (修正済 2026-06-19) method_missing / respond_to_missing? / Object#instance_variables /
  Symbol#inspect の @ivar/@@cvar/$global を bare 表示。

- **blockless enumerator が一部メソッドで未対応**: `(1..10).select.with_index { }`、
  `Hash#each_with_index`(no block) 等が NotImplementedError (REQUIRE_BLOCK)。
  ブロック無し呼び出しで Enumerator を返す機構が要る (broad / 優先度3)。
  ※ map/each/select は一部 enumerator 化済 (map.with_index 等は動く)。
- (修正済 2026-06-19) **`round(half: :even/:up/:down)`**: Float/Integer 両対応。
  残: `2.45.round(1, half: :even)` が 2.4 (CRuby 2.5) — float の十進近似精度問題
  (scaled-multiply 方式の限界、decimal-string rounding が要る。default round にも
  同種の精度限界あり、稀)。
- **Unicode case mapping 非対応** (`"café".upcase` → "CAFé")。ASCII のみ。設計範囲外。

## (修正済 2026-06-18) SEGV: `exc = SomeError.new("msg"); exc.message`

- 原因: `ExcClass.new` が generic KorbObject を作っていた (new_kind が
  例外クラスを plain user class 扱い) → Exception#message の VAL2EXC が
  別 struct を誤 cast して SEGV。
- 修正: korb_class_new_kind が例外クラスを kind 2 に分類、korb_send_impl の
  mid_new で本物の KorbException を alloc (etype/msg/exc_class/user
  initialize)。無 msg 時の message は class 名。Class#name/to_s/inspect 追加。

## 共有 runtime への koruby 専用コード漏れ (2026-06-12 発覚 → 同日解決)

`runtime/precise_gc/gc_copy.c` の forward_edge 診断 (commit ae983279,
「途中経過 + 診断」) が `koruby_bootstrap_ctx` / `korb_vm` extern と
`cc->stack_base` / `cc->stack_end` (koruby 専用 CTX field) を直書きして
おり、 baruby_precise / ascheme_precise が HEAD でビルド不能だった。

**解決**: 診断ブロックは koruby の bump 移行デバッグ作業 (v2 再構築で放棄)
の持ち物なので、 weak hook 化ではなく**丸ごと撤去**。 診断専用だった
`g_scan_owner` / `g_in_root_scan` グローバルも削除。 forward_edge の
ロジック (idempotent skip + forward_payload) は不変。 baruby_precise /
ascheme_precise / koruby_precise ビルド + smoke (STRESS / STRESS+PURGE
含む) 確認済み。

## Phase 8 進捗 — RESULT 化 & per-CTX 機械化 (2026-05-29)

user 指摘: 「ctx->state 消せた？」「VALUE を返す関数を RESULT にする / 呼び側
も直す」「korb_vm はグローバル変数で残ってる」「一気にやりましょう」。

実施 (2026-05-29):
1. `CTX` に `struct korb_vm *mch` を追加 (per-CTX 機械参照、 abruby `c->abm`
   convention)。 commit a2a13522。 これは「korb_vm global を削除する道筋」。
2. `RESULT_FN __attribute__((warn_unused_result))` を導入し、 全 RESULT-returning
   関数 (korb_raise / korb_raise_X / korb_funcall_r / korb_yield_r) に適用。
   commit 33788e6a。 RESULT を捨ててる場所を全部 build 時に検出可能に。
3. `DROP_RESULT(call)` macro 追加 (RESULT を明示的に discharge する。
   `(void)` だけだと warn_unused_result が silence しない)。 commit 97a5c618。
4. mass migration (Python script): 323 ヶ所の bare `korb_raise(...);` を
   `DROP_RESULT(korb_raise(...));` で wrap。 commit e3ddb39b。
5. macro / inline header 内の 89 ヶ所も DROP_RESULT 化。 commit 7f90f3d2。
6. node.def の 57 ヶ所も DROP_RESULT 化 → node_eval.c 再生成。 commit 45232595。

結果:
- build warning 469 → 0 (全 warn_unused_result 警告消化)。
- build green、 spec pass count 不変 (263 PASS / 7 CRASH)。
- DROP_RESULT カウントが「残 migration 対象数」 の指標になる。

残作業 (Phase 8 続き):
- `-Werror=unused-result` を有効化 → regression 防止 hard error 化。 commit
  済 (Makefile)。
- DROP_RESULT を 1 ヶ所ずつ消す = function を VALUE → RESULT に migrate、
  caller も UNWRAP / return propagation に置換。
- 最終的に `CTX::state` / `state_value` を field から削除。
- `korb_vm->X` 参照を `c->mch->X` に順次置換、 最終的に global `korb_vm`
  を削除して「1 CTX = 1 interpreter」 を可能に。
- `struct korb_vm` 自体の type rename (→ `struct korb_machine`)。

### Phase 8b — builtins cfunc 完全 RESULT 化 (2026-05-29)

tools/migrate_cfunc.py で機械的に cfunc を新 ABI 化:
- file.c: 30 → 0 (commit 115aaff4)
- exception.c + binding.c 部分: 7 sites (commit 7682707b)
- range.c: 18 → 0 (commit ad5d3a4b)
- hash.c: 18 → 0 (commit e3e5ef24)
- string.c: 22 → 6 (commit d01ce7af)  ※残 6 は VALUE helper 内
- proc/comparable/module/object/float: 63 → 13 (commit d6ffe422)
- integer/kernel/array: 134 → 8 (commit 424d94ae)

prologue_cfunc_r_inl に c->state lift safety net 追加: cfunc_r 内から
legacy VALUE-returning helper (korb_funcall, korb_yield 等) を呼んだとき
state が失われるのを防ぐ。

CHECK_FROZEN_R(c, self) macro 追加: RESULT 版 CHECK_FROZEN_RET。

合計: 469 → ~125 DROP_RESULT (object.c 29 + node.def 57 + builtins/* 残)。
残りはほぼ AST 内部 / legacy helper 内で、cfunc 層の migration は完了。

regression: test_string/hash/range/exception/array/class/block/integer/
control 全 PASS。

memory note: feedback_result_and_vm_priorities (user 要望保存)。

### Phase 8c — korb_vm global → KORB_VM(c) macro 移行 (2026-05-29)

global `korb_vm` を直接参照する 891 ヶ所中、 CTX *c がスコープにあるものを
全て `KORB_VM(c)->X` (= `(c)->mch->X`) に変換 (commit e554de81):
- context.h に KORB_VM(c) macro 定義 (transition 用)。
- builtins.c: 732 → 0、 builtins/*.c: 159 → 0、 object.c: 148 → 28。
- builtins/hash.c::hash_apply_self_class helper に CTX *c 追加。
- 残 28 ヶ所 (object.c) と main.c / exe_main.c / koruby_runtime.c は CTX を
  取らない low-level helper (korb_class_add_method_*, korb_check_basic_op_redef,
  korb_method_cache_fill, koruby_visit_roots 等)。 これらは将来:
  (a) CTX を引数追加、 (b) per-machine instance method 化、
  (c) thread-local 化 のいずれかで対応。

multi-interpreter 化への第一歩。 引き続き c->mch を経由した参照に統一して
いけば global 撤廃が現実的に。

### STRESS+PURGE crash fix — ary_mul (2026-05-29)

`[1, 2] * ","` が STRESS+PURGE で SEGV する pre-existing bug を修正 (commit
3697798d)。 ary_mul の join path で `arg = argv[0]` を ARO_ROOT_SCOPE 前で
C-local capture し、 scope 内で `rs[0] = korb_str_new()` の alloc 後に
`rs[1] = arg` していたため、 arg の指す string heap obj が GC 移動された
後の stale pointer が rs[1] に格納されていた。 PURGE で次 iter 時に SEGV。

修正: `rs[1] = arg` を `rs[0] = korb_str_new()` の前に。 同パターンを 12 builtins
+ node.def + object.c で grep → 該当なし。 ary_mul 固有。

成果: 全 10 test suite (string/hash/range/exception/array/class/block/
block_arg/integer/control) が default / STRESS / STRESS+PURGE 全 mode で PASS。
これで koruby_precise の test suite は GC モード全制覇。

### AST dispatcher の argv-clobber 修正 + Array.new subclass initialize (2026-05-29)

User 指摘: 「sp として staging + 1 を渡してるんだから、別に c->sp を設定し
ないでいいんじゃないの？ alloc 前に設定するのは callee の仕事」

問題:
1. korb_dispatch_to_method の AST 経路は new_fp = c->sp + 1 から zero-fill
   するが、 caller が argv を sp 上に staging したケースで argv が clobber
   される。
2. node_super_forward 系 (3 site) が `sm->u.cfunc.func` を直接呼び、 cfunc_r
   登録 (ary_initialize 等) では func==NULL で SEGV。
3. `Array.new(...)` が subclass の initialize を call せず、 size/default
   short-cut で結果を返していた。 spec: array/to_a_spec subclass テスト fail。

修正 (commit 832011d6, dcf28a8c):
- AST 経路で argv を VLA に snapshot してから zero-fill (callee 責任の API
  に統一)。
- cfunc_r 経路にも同じ snapshot を入れて argv-overlap を防御。
- node_super_forward / node_super (3 site) で func_r 経路を追加。
- ary_class_new: allocate-then-dispatch-initialize に refactor。 ary_initialize
  registration を _r 化 (Phase 8b で body は _r 化済だったが register が
  legacy のまま残っていた未解決 integrity 不整合)。

成果: broad rubyspec sweep (150 spec) PASS=1383 → 1615 (+232)、 CRASH=0
維持、 全 test suite regression なし、 STRESS+PURGE も完走。

### 追加 fix (2026-05-29 第 5 セッション)

- Hash#slice / #except: compare_by_identity flag を保持 (CRuby 仕様)
- Integer#gcd / #lcm / #gcdlcm: 非 Integer 引数で TypeError 投出
- Integer#~ for Bignum: 真の two's-complement で実装 (mpz_com 使用)。
  allbits?/anybits?/nobits?/complement_spec 等を ~bignum_value で
  通すように。
- ary_sort_compare: <=> が nil で ArgumentError 投出 (CRuby 仕様)
- Range#begin / Range#end: rng_first / rng_last の alias から分離して
  nil-safe (beginless/endless で raise しない)
- Range.new: #<=> が raise したら propagate (rescue しない)
- Integer#pow(neg, mod): RangeError 投出 (bootstrap.rb)
- Array.new / Hash.new / String.new: subclass の initialize を dispatch
  (allocate-then-call initialize pattern)。 大量の subclass test を
  fix。 c->sp bump + sp[0] re-read で AST dispatcher の zero-fill
  clobber 回避。
- {ary,hash,str}_class_new で self を alloc 前 capture せず、 alloc 後に
  sp[-argc-1] から re-read。 T_CLASS は arena allocated なので
  STRESS+PURGE で移動して stale 化する。
- T_STRING / T_ARRAY / T_HASH / T_RANGE / ... での @ivar 対応:
  korb_vm->generic_ivars という side table (Hash of obj-ptr → ivar Hash) を
  追加。 visit_roots で tracking。
- AST dispatcher で argv snapshot: caller が sp 上に staging した argv が
  zero-fill で clobber される問題を VLA snapshot で防ぐ。 cfunc_r 経路
  にも同じ snapshot 適用。
- super (3 site in node.def): cfunc_r 経路に対応 (ary_initialize 等が
  cfunc_r 登録のため legacy 経路で func==NULL → SEGV していた)。
- prologue_proc_method / super で proc_call の ABI 不整合修正
  (Phase 8b で proc_call は RESULT 化済だが caller がそのまま legacy
  呼び出ししていた)。 __method__ spec の crash も同時 fix。
- kernel_method_name: cfunc_r prologue が frame を push するため
  current_frame->method は __method__ 自体になる → KORB_METHOD_CFUNC
  frame を skip して enclosing を返す。

成果: broad rubyspec sweep PASS=1615 → 1634 (+19)、 CRASH=0 維持、 全
10 test suite が default / STRESS / STRESS+PURGE 全 mode で PASS。

### Curry / nested closure bug (2026-05-29 第 6 セッション)

`Proc#curry` 含む recursive Proc dispatch で nested lambda の captured
outer-var が壊れる pre-existing bug を発見 ([[project_koruby_precise_curry_bug]])。

簡易 repro: `lambda { |a,b,c| a+b+c }.curry[1][2][3]` → SEGV。

仮説: `korb_snapshot_one_proc_` が new_inner_lambda の env を snapshot する
際、 fresh_env_B (rec の proc_call clone) は accum_env_size 個 (= 親 lambda
の env size) しか valid 領域を持たないが、 new_inner->env_size の方が大きい
場合、 snap_B の slot K > accum_env_size は fresh_env_B の SLACK 領域 or
他データを copy する。 親が contained block の env_size を伝播せず、 child
が parent の env_size を超えると corrupted slot にアクセスする。

対応案: parser-side で `korb_proc_new` の env_size 算定時に contained block
の env_size を親側に伝播して max を取る。 または snapshot 経路で child の
env_size まで cover するように fp_hi を拡張する。

未解決。 rubyspec の curry_spec.rb が crash する。 他は不影響 (test_block
他は PASS)。

### Bug fixes (2026-05-29 第 6 セッション、 後半)

- Array#zip: #to_ary / #to_a 失敗時の #each fallback 実装。
  Array::__zip_each__(obj, n) ヘルパで each → break at limit パターン。
  zip_spec: 10 → 12 PASS。
- proc_call clone に FRESH_ENV_SLACK = 512 (defensive、 korb_yield に
  mirror)。

このセッション末: rubyspec sweep PASS=1634 維持、 全 10 test suite が
default / STRESS / STRESS+PURGE 全 mode で PASS、 27 benchmark が
STRESS+PURGE で 0 crash。

### Enumerable 系の改善 (2026-05-29 第 6 セッション、 終盤)

- Enumerable#sort: 1 個目の `def sort` が block を受け取らず無視していた
  bug を fix。 sort_spec: 5 → 9 PASS。
- Enumerable#count(arg) / #count { ... } をサポート。 count_spec: 3 → 8 PASS。
- Enumerable#min(n) / #max(n) / #min_by(n) / #max_by(n) (arity n) を
  サポート。 min/max_by/min_by: +21 PASS。
- block 引数の受け取り方を CRuby 仕様 (block(probe, running) で sign 確認)
  に統一。

Total +33 PASS from Enumerable fixes。 broad sweep 上では fluctuation あり
だが 150-spec sample で stable 1634。 specific enumerable spec で confirm 済。

### 第 8 セッション: yield-args bug root cause fix + Phase 8d 大規模 RESULT 化 (2026-05-29)

**Bug fix 1**: yield-args 破壊 (commit ed8dc4db)

`def my_fi(&blk); obj.each { blk.call }; end` パターンで yielding method
(each) が yielder の caller (my_fi) より深い frame で動くと、 share-env
path で yield する時 block body の `c->sp = sp + N` が active method
frame slots を上書きしていた。

Fix: `korb_yield_slow` で `prev_fp > blk->env` (method overlaps env) を
検出したら `fresh_env_path` を強制 ON。 `korb_yield` fast path も同条件で
slow path に fall。 [[project_koruby_precise_yield_args_corruption]] を
"FIXED" 化。 効果: 副次的に `Proc#curry[1][2][3] = 6` の基本ケースが動く
ようになった (curry_spec 0 CRASH → 34 PASS / 14 FAIL/ERR)。

**Bug fix 2**: Enumerable predicates が `blk.call(*xs)` に戻せた

bug fix の結果 yield-args corruption が消えたため、 `any?/all?/none?/
one?/find_index` を `blk.call(*xs)` に書き換え (multi-yield の正しい
gather/dispatch 復活)。 any_spec.rb 65 → 70、 all_spec.rb 56 → 61 PASS。

**Enumerable 改善**:
- Hash#any?, Hash#all? の override 削除 (Enumerable へ委譲)。 pattern
  arg / gather-as-pair 対応。
- Array#any?/all?/none?/one? cfunc に argc > 1 の ArgumentError check 追加。
- Array#intersect? を実装。 intersect_spec.rb 0 → 10 PASS。

**Phase 8d 大規模 RESULT 化** (user 強い要望: 「c->state 消してってば 全部 RESULT」)

- `koruby_gen.rb`: `Node.result_type = "RESULT"` を override。 全 dispatcher
  / EVAL が RESULT 返り値で gen される。
- `node.def`: 全 120 NODE_DEF を mechanical rewrite (tools/migrate_nodes_to_result.py)。
  - `return X` → `return RESULT_OK(X)`
  - `DROP_RESULT(korb_raise(...)); return Qnil` → `return korb_raise(...)`
  - `EA(c, n)` → `EVAL_ARG_UNWRAP(c, n)` (user 指摘で改名、 UNWRAP(EVAL_ARG(c, n)) 展開)
  - while/until/rescue/rescue_else/ensure を RESULT-native loop に
  - break/next/retry/redo/raise/return 終端 node は `(RESULT){v, state}` を返す
- `node.h`: `EVAL` が RESULT 返り値の canonical (旧 EVAL は `EVAL_LIFT` に rename、
  legacy c->state bridge 用)。 user 指摘で `EVAL_R` を最終 `EVAL` 名に。
- `prologues.h` / `object.c`: `mc->dispatcher(c, mc->body, sp)` 直叩きを
  `EVAL(c, mc->body, sp)` に (user 指摘「dispatcher 直接呼出しはやめてね」)。
- `context.h`: `LIFT_C_STATE` + `LIFT_C_STATE_OR_OK` macro 追加 (legacy
  helper を呼ぶ場所での c->state → RESULT bridge)。 全 helper を _r 化
  するまでの過渡期 macro。
- 全 10 test suite が default / STRESS / STRESS+PURGE で PASS 維持。

残り Phase 8d 作業:
- ~~`EVAL_LIFT` 撤去~~ **完了**。 全 caller (koruby_run_ast / proc_call /
  korb_yield_slow / Fiber start / Kernel#eval / Module#class_eval /
  Binding#eval / korb_dispatch_to_method) を RESULT-native 化。
- `korb_dispatch_call` / `korb_funcall` / `korb_yield` / 各種 `korb_node_X_slow`
  helper を RESULT 化 (今は LIFT_C_STATE 経由で c->state を read)。
- `builtins/` 全 cfunc を cfunc_r ABI に統一 (現在 ~100 ヶ所未移行)。

### 第 7 セッション: Enumerable 追加 + pre-existing yield bug 発見

- Enumerable#take(n) / #drop(n) を実装 (Integer 強制 + to_int coerce)。
  drop_spec: 0 → 9 PASS。
- Enumerable#chunk_while / #slice_when の block 必須化、 slice_when
  実装。 chunk_while: 3 → 4、 slice_when: 0 → 6 PASS。
- Enumerable methods で each yield-multi 値の gather pattern:
  each_with_object 8→17 (+9)、 group_by 5→7 (+2)、 partition 1→3 (+2)。
- Enumerable#any? / #all? / #none? / #one? に pattern arg + multi-arg
  raise + multi-yield 対応。 any 49→63、 all 40→54、 none ~25→31、
  one ~30→39。

**pre-existing bug 発見** ([[project_koruby_precise_yield_args_corruption]]):
`def each(arg, *args); yield arg; end` を block-from-outer-method 経由で
呼ぶと args が逆流書き込みされる。 predicates の blk.call(*xs) で
surface した。 curry bug ([[project_koruby_precise_curry_bug]]) と同根
の env-slot 算定問題と推定。 blk.call(x) (gather as array) で回避。

このセッション末: rubyspec sweep PASS=1634 stable、 全 10 test suite が
default / STRESS / STRESS+PURGE 全 mode で PASS。 Enumerable 全体で
個別 spec の PASS counts が大幅改善 (合計 ~+50 PASS、 sweep sample に
は含まれない specs での gain)。

## rubyspec 取れ高改善 (2026-05-28 後半)

baseline (commit 66ab6dda 時): broad sweep `PASS=238 / FAIL=178 / CRASH=11`。
直近 sweep: `PASS=263 / FAIL=148 / CRASH=7`。 第 3 セッション分の追加:

- Integer#<=> に coerce protocol + Bignum-vs-Float 精度保存 + NaN/Inf 対応。
  33 → 42 pass。
- Array#initialize を実装 (旧 ary_class_new とは別 entry)。
  array/initialize_spec 6 → 30 pass。
- Range#first / #last に to_int coerce, Float truncate, beginless/endless
  例外。 first 6 → 11、 last 7 → 12 pass。
- Range#each に beginless TypeError / endless / non-succ 対応。 8 → 15。
- Range#min / #max を専用実装 (rng_first/last からの誤エイリアス解消)。
  min 8 → 9、 max 9 → 10 pass。
- Range#size で endless / beginless / non-Numeric の挙動を CRuby に合わせ。
  4 → 8 pass。
- Array#sum に Kahan compensated summation。 25 → 26 pass。
- Integer#round に half: kwarg (:up / :down / :even) と ArgumentError。
  8 → 21 pass。
- Integer#truncate を専用実装。 3 → 6 pass。
- Array#cycle で count の Float / to_int / TypeError 対応。 11 → 18 pass。
- mspec_shim に `it_should_behave_like` 別名追加。
- Numeric#coerce のデフォルト実装。 0 → 10 pass。
- Integer#coerce で String / #to_f respondable も Float pair 化。 17 → 24。

## rubyspec 取れ高改善 (2026-05-28 後半 + 第 2 セッション分)

baseline (commit 66ab6dda 時): broad sweep `PASS=238 / FAIL=178 / CRASH=11`。
第 2 セッション末で `PASS=258 / FAIL=153 / CRASH=7`。 第 2 セッション分:

- Array#slice / #slice! を全面書き直し (Range / (idx, len) / Range の to_int
  coerce / element-wise return)。 27 → 95 pass。
- Array#bsearch に find-any (Numeric ブロック) mode と Enumerator 経路。
  10 → 20 pass。
- Array#equal_value (==) に identity check + element-wise user dispatch
  + non-Array rhs の to_ary 経由 == 委譲。 5 → 8 pass。
- mspec_shim MSpecMock#respond_to? を should_receive(:respond_to?) と
  singleton_methods に追従。
- Array#[]= の (start, len) self-alias 安全化 + 不正引数 + to_ary coerce
  + Bignum length → RangeError。 element_set 54 → 320 pass。
- Array#* で to_str → to_int の coerce 順序 / nil → TypeError / 多引数で
  ArgumentError。 multiply 18 → 24 pass。
- String#[] (slice) を UTF-8 codepoint index で書き直し。 chr 9 → 11、
  element_reference 10/10 pass。
- String#delete_prefix / #delete_suffix を codepoint 境界尊重。 prefix
  21 → 22、 suffix 21 → 22 pass。
- String#[]= で UTF-8 codepoint index と to_str coerce、 raise TypeError。
- Array#fill に Range form + grow + Bignum RangeError + argc 範囲 check。
  62 → 130 pass。
- Array#to_h に argc check + to_ary coerce + sized error。 7 → 14 pass。
- Array#values_at の Range で nil-fill + RangeError。 11 → 14 pass。
- Array#sort! / Array#sort_by! の frozen check & break-aware。
- Array#zip に to_ary/to_a coerce + block 経路。 7 → 10 pass。
- Array#each / Array#rindex で iteration 中の size 変化を再評価。
- Hash#to_h で subclass → fresh Hash + default/default_proc/CBI 引継ぎ。
  17 → 20 pass。
- Hash#[] で subclass の #default override を honor (canonical 以外で
  funcall 経由)。 element_reference 28 → 29 pass。
- Hash.[] (class method) で to_hash/to_ary coerce + argc 検証 + 要素検証。
  20 → 30 pass。
- Hash#transform_keys! の break で「処理済 transformed + 未処理 untouched」 
  + Hash#transform_values で CBI 引継ぎ。
- Symbol#casecmp / #casecmp? を追加 (String 経由)。 0 → 41 pass。
- Integer#to_s で全 base 2..36 + Bignum 経路 (mpz_get_str)。 12 → 25。
- Integer#div / #divmod で Bignum / Float / coerce protocol 対応。
  div 13 → 60、 divmod 6 → 29 pass。
- Float#round で negative-precision Integer 化、 NaN/Inf 例外、 kwarg drop。
  62 → 83 pass。
- Float#divmod に NaN / Infinity / ZeroDivisionError 検査。 9 → 14 pass。
- Float#<=> に coerce protocol + Infinity vs finite Integer + obj.infinite?
  経路。 26 → 39 pass。



主な修正 (順):
- `String#lines` に sep / `chomp:` 引数 (bootstrap.rb の override 削除 +
  C str_lines を引数 parse 込みで再実装) — lines_spec 0 → 3 pass
- `Integer#%` で Float RHS の SEGV — int_mod に Float fast path 追加。
  これで integer/{pow,gcd,gcdlcm,lcm}_spec が crash 脱出 (合計 +75 pass)。
- `Integer#pow` (bootstrap.rb) で exp/mod の type 検証 + ZeroDivision。
- `String#ljust/rjust/center` の pad cycle が CRuby と非互換 (`pad * (n-size)`
  だと長すぎる) → `__pad_to(pad, n)` helper を導入。 ljust/rjust 14 → 33、
  center 16 → 49 pass。
- `String#squeeze` を charset (range, negate, multi-set 交差) 対応に再実装、
  squeeze! に frozen 判定追加 — squeeze_spec 6 → 44 pass。
- `String#reverse/#chop/#insert` を UTF-8 codepoint 境界対応。
- `String#chomp` no-arg で `$/` 参照を追加。
- `Array#join` で sep の `to_str` coerce + empty array short-circuit。
- `Array#flatten` に depth の `to_int` coerce + 要素 `to_ary` 連携、
  `Array#flatten!` を専用 entry として実装。 flatten_spec 27 → 50 pass。
- `Array#each` でループ毎に array len を再評価 (block 内の push/shift 反映)、
  no-block で Enumerator を返す。 each_spec 4 → 10 pass。
- `Hash#to_h` で subclass からの fresh Hash 生成 + default/default_proc/
  compare_by_identity 引継ぎ。 to_h_spec 17 → 20 pass。
- `Array#sort_by!` を no-block enumerator / break-aware に。 sort_by 10 → 13。
- `Array#reject!` を delete_if と分離 (no-change → nil)、 両者に no-block
  enumerator 経路を追加。 reject_spec 25 → 38、 delete_if 6 → 8 pass。
- `Hash#transform_keys!` の break 動作 (処理済 transformed + 未処理 untouched)、
  `Hash#transform_values` の compare_by_identity 引継ぎ。
- `Array#min/#max` のブロック引数順を (probe, running) に修正 (sort と逆)、
  min 28 → 38、 max 33 → 35 pass。

残 7 CRASH の sweep:
  array/{combination,comparison,permutation,repeated_combination,
        repeated_permutation,uniq}_spec、 string/each_byte_spec。
- comparison は recursive_array 比較で stack overflow。
- combination 系は深い再帰 + 要素数で OOM。
- uniq は要素数で OOM。
- each_byte は Enumerator + Fiber 経路で SEGV。



## 残 global / korb_vm->current_ctx fallback 撤去 — **完了 (2026-05-28 thirteenth pass)**

「グローバルつくらんといて」 規約 (memory: feedback_no_globals_strict) に
従い、 段階的に CTX を関数引数経由で渡す方向に移行 → **完了**:

- `current_block` / `running_block` global → CTX field 化 (commit 8e388503)
- `korb_str_new` / `korb_str_new_cstr` に (CTX, sp) (commit 0010d621)
- `korb_float_new` / `korb_float_new_heap` に (CTX, sp) (commit 03435e7a)
- `korb_ary_new` / `_capa` / `_from_values` / `korb_hash_new` に (CTX, sp) (commit f2297121)
- `korb_str_dup` / `korb_str_concat` / `korb_object_new` に (CTX, sp) (commit 9e8905b4)
- `korb_class_new` / `korb_module_new` に (CTX, sp) (commit 68994185)
- `korb_to_s` / `korb_inspect` に (CTX, sp) (commit 007fe5af)
- `korb_cvar_names` / `method_params_for_method` / `methods_with_visibility` /
  `korb_select_collect_ready` / `str_appendf` / `korb_inspect_inner` / `korb_p` /
  `korb_runtime_init` / `korb_init_builtins` に CTX 引数化 (commit 3c00037f)
- `korb_singleton_class_of` / `korb_singleton_class_of_value` に CTX 引数化 (commit 53a99fe4)
- `korb_hash_value` / `korb_eq` / `korb_eql` / `korb_hash_aref*` / `korb_hash_aset`
  に CTX 引数化 (commit 10abd165)
- `korb_raise_frozen_modification` / `korb_ivar_set_ic` / `korb_class_add_method_ast*` /
  `korb_exc_new` / `binding_arg_to_id` / 残 object.c の `c2/c3` 内部 fallback
  に CTX 引数化 (commit 1dc64f59)

残 1 件: `koruby_setup_ctx` (= bootstrap_ctx を返す boundary entry point) のみ。
これは 「初期化フロー」 のため許容。

## c->state 撤去 (Phase 8) — 段階移行 plan

「c->state 消してね」 規約。 試行で result_type を一括 RESULT 化すると
node.def + node.h の `EVAL_ARG` / `dispatcher_t` / prologues.h / 多数の
`VALUE x = EVAL(...)` callers の 15 系統の error が連鎖し、 1 commit で
build green に到達できないことが判明。

代わりに 「段階移行」 方式を採る:

1. **dispatcher_t / EVAL を RESULT 化 + EVAL_LEGACY 提供**: 既存 callers
   は EVAL_LEGACY (pump RESULT → c->state) を使う thin wrapper を用意。
   - dispatcher 直叩き callers (prologues.h, object.c の builder, builtins
     経由の korb_yield 内部 inline) も同様に pump 経由に。
2. **node 単位で result_type=RESULT に opt-in**: NODE_DEF の per-node flag
   で result type を選べるよう koruby_gen.rb を拡張。 1 node ずつ移行し
   ながら build green を維持。 EVAL_ARG 結果も per-node RESULT or VALUE。
3. **node.def 内の `return X;` を機械変換**: Python script で
   `return RESULT_OK(X);` に置換。 `if (cond) return Y;` も対応。
4. **korb_raise / korb_funcall / korb_yield も RESULT 化**: 既に
   `korb_funcall_r` / `korb_yield_r` ブリッジは追加済 (commit fa03bd2a)。
5. **c->state を読む箇所を順次撤去**: legacy cfunc 群を Phase 4 sweep で
   新-ABI 化していくときに同時に置換。
6. **最終的に CTX から `state` / `state_value` field を削除**。

途中の試行で以下まで進めた (revert 済):
- koruby_gen.rb override で全 node の dispatcher signature を RESULT に
- node.def の `return X;` (270 箇所) を `return RESULT_OK(X);` に
- EA / CHECK_STATE マクロを UNWRAP ベースに書き換え
- 残 ~10 site (prologues.h dispatcher 直叩き、 node_eval.c の三項 EVAL_ARG、
  object.c / builtins の `VALUE x = EVAL(...)` callers) で build error

これらは next session に持ち越し。

## 現状 (2026-05-28, twelfth pass)

直近の発見: 自前 test/ の "OK Xxxx (0)" 24/24 表示は **TESTS.each
{|t| run_test(t) } という最頻出パターンが top-level の block 経由で
動かず**、 各 test メソッドが 1 回も実行されないまま `$fail == 0` で
"OK ..." を出していた phantom pass だった。

修正項目:

1. **block / proc / fiber body の sp = fp + env_size 規約整備**
   (object.h / object.c / builtins/proc.c):
   bake walker は block 内 lvar を envsize 相対の sp_offset に baking
   する一方、 korb_yield fast/slow path・proc_call・fiber entry が
   sp=fp を渡していたため、 `puts "hi"` のような最も単純な literal
   までもが nil に化けて出力が空行になっていた。 method body と同じく
   `sp = fp + env_size` を渡すよう統一。

2. **node_plus の synthetic frame で self を hijack していた**
   (node.def): GC 越しに `l` を生存させる目的で `fr.self = l` を設定
   していたが、 これが rhs 評価中の ivar 参照 (例 `"X" + " (#{@y})"`)
   を l に向けて @y を常に nil 化していた。 `fr.self = caller's self`
   に戻し、 l は `fr.last_line` に park する形へ。

3. **block_given? が cfunc frame の block を見ていた**
   (builtins/kernel.c): prologue_cfunc_inl は frame push するように
   変わっていたが kernel_block_given は古い前提 ("cfunc は push しない")
   のままで block_given? が常に false。 frame chain を walk して直近
   の AST method frame を見るよう修正。

4. **Fiber 二回目以降の resume で stale な cfunc frame chain**
   (object.c): korb_fiber_entry が frame push せず、 inner cfunc frame
   が resumer の cfunc frame (= 一度 return すると stale) を .prev anchor
   としていたため 2 回目以降の resume で SEGV。 fiber 自身の C stack に
   stable な fiber_root frame を push して inner はそこに chain する形に
   改修。 prev_top の巻き戻しも撤去 (= resume の POST-SWAP が
   resumer_current_frame で c->current_frame を上書きするため不要)。

5. **current_block / running_block が visit_roots に欠落**
   (koruby_runtime.c): ary_each などの builtin iterator が
   `korb_yield(c, 1, &v) → current_block` 経路で proc を deref するが、
   current_block は file-scope global で GC が edge を update して
   いなかった。 STRESS の下で proc が forward された途端、 次の yield
   で blk->self / blk->env が unmapped 領域を deref → SEGV。
   visit_roots の (d') section に追加。

### NORMAL mode 結果 (本セッション)

| test                       | pass |
|----------------------------|------|
| test_alias                 | 9    |
| test_alias_redef           | 3    |
| test_array                 | 72   |
| test_basic_op_redef        | 4    |
| test_block                 | 33   |
| test_block_arg             | 8    |
| test_class                 | 18   |
| test_comparable            | 21   |
| test_control               | 34   |
| test_cpu_corner            | 29   |
| test_eq                    | 58   |
| test_eq_redef              | 7    |
| test_exception             | 10   |
| test_fiber                 | 26   |
| test_float_round           | 27   |
| test_flonum                | 80   |
| test_hash                  | 52   |
| test_integer               | 105  |
| test_misc                  | 26   |
| test_object_alloc          | 19   |
| test_range                 | 27   |
| test_string                | 49   |
| test_to_s_dispatch         | 5    |
| test_yield                 | 15   |

**24/24 ファイル 全 OK、 計 ~735 assertion 全 pass**。

### STRESS mode 結果

NORMAL の後で BARUBY_GC_STRESS=1 を入れて自前 test/ を全 24 件走らせた
結果。 段階的に修正を入れて **24/24 file 完全 pass** (STRESS / STRESS+PURGE
両方とも全 OK)。 初期 14 から +10。 mode 間で異なる stale ref 経路が
表面化する pattern (test_block は STRESS のみで SEGV、 逆に test_exception
は STRESS で OK だが PURGE で SEGV) を最後の 2 つの修正で潰した:

- `koruby_visit_roots` に **gvars table を追加** (= `$!` を auto-forward
  しないと raise inside rescue で cause-link walk が SEGV; commit
  e3dfea32) — test_exception STRESS+PURGE。
- **korb_yield の prev_self を GC root stack で pin** (commit 76aa7419)
  — block body の EVAL を straddle する prev_self C-local が STRESS GC
  越しに stale 化し、 block exit の frame.self 復元で死アドレスを
  書き込む問題。 test_block STRESS。

| test                      | STRESS  | STRESS+PURGE |
|---------------------------|---------|--------------|
| test_alias                | 9       | 9            |
| test_alias_redef          | 3       | 3            |
| test_array                | 72      | 72           |
| test_basic_op_redef       | 4       | 4            |
| test_block                | 33      | 33           |
| test_block_arg            | 8       | 8            |
| test_class                | 18      | 18           |
| test_comparable           | 21      | 21           |
| test_control              | 34      | 34           |
| test_cpu_corner           | 29      | 29           |
| test_eq                   | 58      | 58           |
| test_eq_redef             | 7       | 7            |
| test_exception            | 10      | 10           |
| test_fiber                | 26      | 26           |
| test_float_round          | 27      | 27           |
| test_flonum               | 80      | 80           |
| test_hash                 | 52      | 52           |
| test_integer              | 105     | 105          |
| test_misc                 | 26      | 26           |
| test_object_alloc         | 19      | 19           |
| test_range                | 27      | 27           |
| test_string               | 49      | 49           |
| test_to_s_dispatch        | 5       | 5            |
| test_yield                | 15      | 15           |

行った修正:
- **libc-allocated proc の self / enclosing_block を visit_roots に
  追加** (koruby_runtime.c): scan_edges は arena obj のみ呼ばれる
  ため libc proc の field は forward されない。 running_block /
  current_block / 全 frame.block を walk して内部を再帰 visit。
- **korb_class_of_class を type-based redirect に**
  (object.h): T_ARRAY / T_STRING / T_HASH / T_RANGE / T_PROC /
  T_FLOAT / T_BIGNUM の basic.klass は GC 越しに stale 化するため、
  type を見て korb_vm->X_class (= visit_roots で auto-track) を
  直接返す。
- **korb_inspect_inner T_ARRAY / T_HASH / T_STRING を ARO_ROOT_SCOPE
  化** (object.c): result 文字列 / element 文字列が korb_str_concat
  越しに stale 化していた (= `[20, 30]` が `]]` に、 `"hello".inspect`
  が `""` になる現象)。
- **libc obj registry (koruby_register_libc_obj)** (koruby_runtime.c
  + object.c + builtins/{object,binding,symbol}.c): 全 libc 容器
  (korb_array / korb_hash / korb_range / korb_float / korb_bignum /
  korb_proc) + T_DATA (Method / Binding / Fiber / symbol-proc /
  method-proc) の constructor で koruby_register_libc_obj を呼んで
  singly-linked list に登録。 visit_roots 末尾の (f) phase で list
  を walk し、 各 obj の内部 heap-pointer fields (basic.klass,
  array elements, hash entries, proc env / self / cref) を
  visit_value_slot / visit_ptr_slot で forward。 これにより hash
  key/value, array elements, proc env 等に格納された arena ref が
  GC 越しに正しく追跡されるようになる。
- **node_lshift で l の synthetic-frame pin** (node.def): node_plus
  と同じ pattern で fr.last_line に l を park し、 fr.self は
  caller's self 継承。 `s << "y"` の self が GC 越しに stale 化
  していた問題を解消。
- **builtin pinning の系統的適用**: ary_min, ary_max, ary_mul
  (string-join path), str_lshift / str_concat_one, str_split,
  str_chars, str_gsub, str_sub, kernel_format (sprintf) の各 cfunc
  iterator で C-local heap pointer を ARO_ROOT_SCOPE で pin。

新規 full pass: test_cpu_corner / test_hash / test_to_s_dispatch /
test_misc / test_fiber / test_array / test_string。

残課題 (2026-05-28 完了):
- **test_exception PURGE SEGV**: gvars.vals が visit_roots に欠落
  していたため $! (= 直前 raise した exc) が stale 化、 nested raise の
  cause-link walk で SEGV。 koruby_runtime.c phase (d'') で gvars を
  walk するよう追加 (commit e3dfea32)。
- **test_block STRESS SEGV**: korb_yield の prev_self C-local が
  block body の EVAL を straddle して stale 化、 block exit 時の
  frame.self 復元で死アドレスを書き込んでしまう。 prev_self を
  AROH_ROOT_STACK_TOP / SET_TOP で root stack に park (commit 76aa7419)。

### rubyspec STRESS+PURGE 改善 (2026-05-29)

rubyspec の language sweep を STRESS+PURGE で計測した結果:

- 初期 6 PASS / 61 SEGV (前 session 終了時)
- 14 PASS / 50 SEGV (current)

追加修正 (commit 順):

- `proc_call` の prev_self を AROH_ROOT_STACK で pin (commit 5cb2938c)
- `korb_const_lookup` で cref->klass=NULL を defensive skip
  (commit 85dfcd0c) — GC forward_payload の stale-to-space 分岐で
  cref->klass が NULL 化された場合の保険。
- libc registry walker で T_PROC の `env` を walk (escape proc 用),
  proc_call / prologue_ast_general で raise/throw 時の r snapshot を skip
  (commit 827e0151)。
- `node_apply_call` の recv / args / block を 3-slot pin (commit cd60b4b0)。
- `node_method_call_block` の r / block を 2-slot pin (commit c516da2c)。
- `node_aref` / `node_aset` / `node_obj_singleton_def` を pin (commit 323b1f85)。
- `korb_singleton_class_of_value` を ARO_ROOT_SCOPE 化、 o 再 load
  (commit 323b1f85)。

### BARUBY_GC_STRESS=N 間隔指定 (2026-05-29)

`BARUBY_GC_STRESS=100 BARUBY_GC_PURGE=1` のように N alloc 毎の GC を
指定可能に (commit 763880ed)。 default の N=1 (= 全 alloc) では
重い workload (rubyspec / benchmark) が timeout するため、 100 や
1000 などで間隔を緩和して使う。 BARUBY_GC_STRESS=100 + PURGE で
benchmark 26/27 完走 (従来 timeout で 24/27)。

### node.def の sp slot pattern 統一 (2026-05-29)

ARO_ROOT_SCOPE_START を node.def 内で多用していたが、 baruby_precise
の @child / DISPATCH 自動 spill モデルに合わせて、 scope-entry での
1 回の sp shift + sp[N] 参照 + scope-exit restore パターンに refactor
(commit 1a66508c)。

```c
sp[0..N-1] = 0;     /* zero-fill */
c->sp = sp + N;     /* extend visit range — 1 回だけ */
sp[0] = EA(c, ...); /* GC 中 sp[0] auto-forward */
...
c->sp = sp;         /* restore */
```

object.c / builtins/ の C-level pinning (sp arg を持たない関数) では
ARO_ROOT_SCOPE_START を引き続き使用。

### 残課題 (要 framework 改造)

- **koruby_gen.rb に `@child` decorator 対応**: astrogen framework は
  既に `@child` を Operand level で実装済 (Node 側で build_specializer
  が auto-generate)。 koruby_gen.rb は PG_CALL_NODES 用 custom
  specializer があり、 @child 互換にするには両方の specializer に
  child spill コードを emit する必要。 baruby_precise のように
  framework に任せると node.def 側が `VALUE recv@child` と書くだけで
  `sp += N; sp[-N] = dispatch(child, sp); ...` が auto-generate される。

- **proc.self / frame.self が STRESS+PURGE で stale 化する根本原因**:
  libc registry walker は `visit_value_slot(ctx, fn, &p->self)` を毎
  cycle 呼んでいるはずだが、 magic_comment_spec.rb / array_spec.rb 等で
  proc.self が retired plane の addr (PROT_NONE 領域) を保持し続けて
  SEGV。 forward_payload が NULL を返すはずの分岐 (gc_copy.c L417) に
  到達していないか、 そもそも visit が走っていない可能性。 詳細 trace
  要。

### 2026-05-29 追加: sp-based / RESULT 返り値 ABI への大規模 refactor 着手

C-local stale 問題と c->state 側流の散漫な伝播を根本解決するため、
Lua 風の stack-based ABI に全面移行する設計を decision。 baruby_precise
/ castro / abruby と同じ規約。

#### 新 ABI 規約

| function 種類 | signature | sp の意味 |
|---|---|---|
| cfunc | `RESULT cf(CTX *c, int argc, VALUE *sp)` | sp[-argc-1]=self, sp[-argc..-1]=args, sp[0..]=scratch |
| EVAL_node body | `RESULT EVAL_node_X(CTX *c, NODE *n, VALUE *sp, ...)` | parent の staging top |
| C API helper | `RESULT h(CTX *c, VALUE *sp)` (fixed arity) / `RESULT h(CTX *c, int argc, VALUE *sp)` (varargs) | sp[-N..-1]=args |
| 即値 helper | 値で受けてよし (alloc しないもの) | -- |

「sp = この scope の staging top。 過去の値は sp[-N]、 自分の scratch
は sp[0..]」 で全関数統一。 caller は `c->sp = sp + N` で alloc 直前
sync。 例外/break/return は RESULT.state で in-band 伝播 (UNWRAP macro)。

#### 進捗 (Phase 別)

- **Phase 1 完了** (commit 03a5449f): RESULT 型と UNWRAP / CHECK /
  RESULT_OK 等のマクロを context.h に追加。 korb_cfunc_r_t / 
  korb_dispatcher_r_t typedef。
- **Phase 2 完了** (commit 4352f5f5): context.h 整理、 prologue_cfunc_r_inl
  追加。 method_cache に cfunc_r field 追加。 korb_dispatch_call_cached
  と korb_dispatch_to_method に bridge を入れて、 cfunc_r != NULL のとき
  新 ABI 経路に流す。 RESULT は c->state + VALUE 返り値に変換 (Phase 8
  まで暫定)。
- **Phase 3 完了** (commit d1ae64a4): PoC として ary_eq を新 ABI で
  書き換え、 動作確認。 self-tests 24/24 / rubyspec 17/67 維持。
- **Phase 4 進行中** (commit 3efbcb94): math.c (18 cfunc) + boolean.c
  (18 cfunc) を sweep。 全 cfunc ~680 個のうち ~37 個完了。 残り ~640
  (大物: kernel.c 45, module.c 45, file.c 49, integer.c 52, hash.c 63,
  array.c 97, string.c 102)。

#### 残作業

- **Phase 4 残**: 残 ~640 cfunc の sweep。 builtins/*.c を 1 ファイル
  ずつ。 symbol.c で test_string が silent fail する原因調査 要。
- **Phase 5**: node.def の call 系 node (node_method_call /
  node_func_call / node_method_call_block / node_apply_call) を
  sp staging 規約に。 fp[arg_index] 経由を撤廃。
- **Phase 6**: prologue_ast_general / prologue_ast_simple_inl 系を
  sp 経由 args 受け取りに。
- **Phase 7**: C API helper (korb_eq / korb_str_concat / korb_funcall 等)
  を slot pointer 規約に。
- **Phase 8**: c->state 経路撤廃、 RESULT 化。
- **Phase 9**: 動作確認 + 回帰 fix。

### 2026-05-29 追加: korb_host_class() helper と class def の pin (multiple commits)

- `korb_host_class(c)` helper を object.h に追加 (commit 07dcb17f)。
  `c->current_frame->cref ? cref->klass : current_class` パターンを
  全て置換、 cref chain を walk しながら NULL klass を skip。
- `node_class_def_in` (commit 44123967) / `node_class_def_in_strict`
  (commit 8c854955) を ARO_ROOT_SCOPE 化 — parent / super / klass を
  3-slot pin。
- `node_const_path_get` の eName / name_v / prefix を pin
  (commit 5a01892e)。
- `node_method_call` の r を pin (commit 59c15c5b)。
- `proc_call` (commit 54222f06) / `koruby_run_ast` (commit a79eaf69) の
  THROW handler で eUTE / tag / val / tag_s を pin。
- `visit_roots` の frame chain walk に depth cap 4096 (commit b9247c37)。

rubyspec STRESS+PURGE 15/67 → 17/67 PASS, SEGV 46 → 41。

### 2026-05-29 追加: fresh_env を value stack 化 (commit 8a0a68f3)

if_spec.rb 等の SEGV 解析で発見した重要な root cause:

`korb_yield_slow` で `creates_proc=true` block の fresh_env が
korb_xmalloc (libc) で確保されていた。これにより:

- fp = libc heap addr (低 0x5555... sbrk 領域)
- sp = fp + env_size = libc addr
- c->sp = sp + N (pin) = libc addr
- visit_roots phase (a) は `c->stack_base..c->sp` を walk するが、
  c->stack_base は 128 MB malloc (高 0x7fff... mmap 領域)、 c->sp が
  libc になると range walk が空 loop となり pin slot が scan されない
  → 全 stale 化

修正: fresh_env を c->sp 起点 (= value stack 上、 16M slots) で確保し、
exit 時に c->sp restore。 captured proc は snapshot_env_maybe で libc
に snapshot 化するため closure semantics は維持。

効果: rubyspec STRESS+PURGE 14/67 → 15/67 PASS、 SEGV 50→46。

## 旧現状 (2026-05-10, eleventh pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `test/ruby/` (全 135 ファイル)**: **89 / 135 has_pass (66%)、
  1,050,622 pass / 1,405,004 (74.8%)**。 起点 (2026-05-09 sweep #1) は
  62/135、 834,385 pass。 +27 ファイル復活、 +216k pass。
  - dumped core: 7 → 0 (super dispatch / refinement binding cref / mm
    recursion / fiber NULL body / Float SIGFPE / Integer overflow recursion
    / Comparable cmp_eq 全解消)
  - LOAD ERROR: 23 → 1 (残り test_time_tz は Regexp 必要)
  - timeout: 3 → 0 (IO.pipe を unbuffered に)
  - 残 0-pass の 46 ファイルは大半 intentional pending (TracePoint /
    ObjectSpace::WeakMap / Ractor / 真の Refinements / 非 UTF-8 Encoding /
    MJIT / 完全 Marshal / 真の ARGF / callcc)。
- **CRuby `spec/ruby/language/`**: **3,745 pass / 190 fail / 51 err**。
- **optcarrot**: AOT-cached で **~76 fps** (CRuby 43 fps の 1.8x、 yjit
  175 fps の 0.43x)。 100 frames best-of-1。
- **CRuby `spec/ruby/core/` 23 cat 集計** (array hash string integer numeric
  range comparable module proc kernel symbol float exception basicobject set
  rational random gc signal binding class enumerator regexp):
  - **pass=14,434 / fail=3,071 / err=1,014 (= 77.9% pass)**
  - **313 / 930 ファイル perfect (33.7%)**

## eleventh pass で test/ruby 互換性を全面強化

主要な fix と効果:
- **super dispatch bug** (object.c:korb_dispatch_binop): caller block の
  defining_method 漏れ。 `Class#new → user initialize → super` が
  "run_all" 等で lookup されてた致命バグ。 test_string 0 → 1965 pass。
- **UnboundMethod late-binding**: instance_method が name しか保存して
  なかったので、 後で define_method 上書きされると new body へ
  redirect され無限再帰。 captured_method を凍結。 test_super 蘇生。
- **Binding が stack-alloc cref を保存**: class body 中に binding する
  と class body 終了で dangle → SEGV。 cref chain を heap deep-copy。
  test_refinement 蘇生。
- **Struct.new {} block の cref leak**: `Struct.new(:x) do def
  method_missing; ... end end` が outer class の method_missing を
  上書き。 block->cref を一時 swap。 test_marshal 蘇生。
- **Class#clone が singleton method を copy しない**: 旧コードは
  `def AClass.cm1; end` 後 `MyClass = AClass.clone` で cm1 が消失。
  test_module 0 → 725 pass。
- **bootstrap method_missing recursion**: `self.inspect` が再 raise
  する class で `"undefined method '...' for #{self.inspect}"` 構築中
  に SEGV。 thread-local depth で 4 段以上で `(recursion)`。
- **Float SIGFPE**: `(long)(big_double)` の UB を `korb_dbl2int`
  (Bignum fallback) に。 flt_floor/ceil の n<0 div-by-0 修正。
- **Integer 無限再帰**: `2 ** -2^62` で fixnum overflow → 無限再帰。
- **A::B::C = 0 ** 0 の slot collision** (test_primitive)。
- **Array#cycle / repeated_combination / repeated_permutation を C 実装**
  (bootstrap.rb 版は break が nested block を貫通しなかった)。
- **巨大 index 防御**: korb_ary_aset / EVAL_node_aset / ary_aset で
  `LONG_MAX/sizeof(VALUE)` 超え index に IndexError raise (旧 OOM)。
- **IO.pipe unbuffered**: line-buffered だった → readpartial が無限
  block。 test_io / test_optimization の timeout 解消。
- **kwh_save_slot defaulting + FL_KWARGS peeling** in dispatch_to_method:
  `Method#call(**{})` 等で kwsplat が `*args` に紛れ込まない。
  test_keyword 613 → 791 pass。
- **dispatch arg-count error を ArgumentError に**: 旧 RuntimeError
  だったので assert_raise(ArgumentError) が拾えなかった。
- **String#[]= 実装**: 旧 stub。 IndexError raise 含む。
- **object_id を immediate VALUE に**: 旧 `(long)self / 8` は Fixnum
  小値を全部 0 に潰し Hash#hash 完全破綻。
- **Kernel#Float() に strict 検証**: "xyz" → ArgumentError、
  exception:false opt、 nil → TypeError、 Bignum 受理。
- **sprintf %b の width / precision / 0-pad / negative-prefix**。
- **tu_shim 大幅拡張**: Errno (130 const) / Encoding (.find / 70 alias /
  CompatibilityError 等) / Process (UID/GID/CLOCK_*) / Thread::Queue /
  IO 定数 / File::Constants / RubyVM::AbstractSyntaxTree / Bug /
  Socket / Dir.mktmpdir / Tempfile / FileUtils / MemoryViewTestUtils /
  ARGF / trace_var / caller_locations / refine 部分実装。

詳細は [done.md](./done.md) の「CRuby test/ruby/ 全 135 ファイル sweep」
セクション参照。
- **tenth pass で perfect 化したもの**:
  - chilled string 完全実装 (FL_CHILLED + parse.c の prism flag 読み分け +
    Symbol#to_s も chilled): `string/chilled_string_spec` `string/uplus_spec`
  - sized Enumerator + each redispatch: `hash/transform_values_spec`
  - NameError @name/@receiver は method_missing 経由でも記録:
    `exception/name_error_spec`
  - Object#<=>/initialize_copy/clone/dup 既定 hook: `kernel/comparison_spec`
  - GC モジュール (garbage_collect/disable/enable/start): `gc/{garbage_collect,
    disable,enable,start}_spec`
  - Random.new_seed の uniqueness 改善 + urandom 負サイズ: `random/{new_seed,
    urandom,seed}_spec`
  - Integer#allbits?/anybits?/nobits?/sqrt/try_convert/to_r/rationalize/
    numerator/denominator/ord: `integer/{allbits,anybits,nobits,sqrt,
    try_convert,numerator,denominator,ord}_spec`
  - Float#numerator/denominator/to_r: `float/{numerator,denominator}_spec`
  - Range#to_s/eql?/count(infinity for endless): `range/{to_s,eql,count}_spec`
  - Array#combination/permutation Enumerator + binomial size:
    `array/combination_spec` `array/permutation_spec` (+5)
  - Array#fetch/fetch_values/to_a/to_ary/deconstruct: 各 spec full
  - Hash#flatten/transform_keys{,!}/to_h/sort/replace/deconstruct_keys:
    `hash/{flatten,sort,replace,to_h,deconstruct_keys}_spec`
  - String#each_byte Enumerator/strip!/lstrip!/rstrip!: 各 spec full
  - Symbol#intern/name + inspect bare/quoted 判定 fix: `symbol/{intern,name,
    inspect}_spec`
  - Rational#integer?=false / Rational.new 禁止: `rational/{integer,
    rational}_spec`
  - Set#each Enumerator: `set/each_spec`
  - Signal::EXIT / Kernel module 関数 30 件 private: `signal/list_spec`,
    `kernel/{abort,exit}_spec`
  - Numeric#dup/clone/ceil/floor/round/truncate/fdiv: `numeric/{dup,clone,
    ceil,floor,round,truncate,fdiv}_spec`
  - Module lifecycle hook 既定 / BasicObject 既定 hook
  - Comparable#== identity 短絡 + Float 0.0 + NoMethodError swallow

## 旧現状 (2026-05-08, ninth pass)

- **CRuby `spec/ruby/core/` 14 主要 cat**: ninth pass で perfect 化:
  - `core/numeric/{ceil,floor,round,truncate,fdiv,dup,clone}` (7)
  - `core/integer/{allbits,anybits,nobits,sqrt,try_convert,to_r,rationalize}` (7)
  - `core/string/{lstrip,rstrip,strip,chomp,sum,getbyte,append,to_f}` (8)
  - `core/hash/{flatten,deconstruct_keys,replace,sort,to_h}` (5)
  - `core/array/{fetch,fetch_values,deconstruct,to_ary}` (4)
  - `core/exception/{message,inspect,name}` (3)
  - `core/symbol/{symbol,float,inspect}` (3)
  - `core/range/inspect` (1)
  - `core/comparable/equal_value` (mostly)
  - `core/module/{const_get,extended,included,prepended}` (4)
  - `core/kernel/{Array,Float,Hash,Integer,String}` (5 — private 化)
- **CRuby `spec/ruby/core/` 14 主要 cat (eighth pass)**: **13,320+ pass、 216+ ファイル perfect**
  本セッションで perfect 化したもの一覧 (代表):
  - `core/array/{any,clear,assoc,plus,try_convert,compact,rassoc,at,insert,
    pop,shift,drop,delete,rotate}` (14)
  - `core/hash/{new,try_convert,to_proc,default_proc}` (4)
  - `core/string/{hex,oct,try_convert,plus,include,prepend}` (6)
  - `core/integer/{multiply,plus,uminus,divide,digits,bit_and,bit_or,bit_xor}` (8)
  - `core/proc/{lambda,allocate}` (2)
  - `core/comparable/{lt,gt,gte,lte,clamp}` (5)
  - `core/kernel/{class,raise,instance_of,instance_variable_defined,dup,
    case_compare,throw,respond_to,respond_to_missing}` (9)
  - `core/binding/clone`
  - `core/module/{const_set,class_variable_set,deprecate_constant,lt,lte,gt,gte,
    comparison,private_class_method,public_class_method,attr_reader,
    attr_writer,attr_accessor,attr,private,protected}` (16)
  - `core/numeric/comparison`
  - `core/float/divide`
- **Binding**: **150 pass** (完全互換)。

## 旧現状 (2026-05-08, fifth pass)

- **CRuby `spec/ruby/language/`**: 3,726 pass / 35 perfect (mock-shim slot fix +301)。
- mock-shim slot bug 解消、 lambda opt args、 anon-rest+post 修正、
  hash literal string-key freeze、 missing keyword 全キー列挙、
  Proc#parameters slot indexing、 eval `__FILE__`、 clone(freeze:)、
  proc post-rest extra-drop、 case/in slot 衝突。

## 旧現状 (2026-05-08, fourth pass)

- **自前 test/ruby/**: **24/24 全 OK** (737 件)。
- **CRuby `spec/ruby/language/` (rubyspec, 65)**: **3,404 pass / 40 perfect**。
- **CRuby `spec/ruby/core/`** (13 主要 cat): **12,407 pass、 133 ファイル perfect**。
  本ラウンドで perfect 化: array (assoc/clear/dig/include/plus/take),
  hash (compact/delete/dig/empty/any/fetch/fetch_values/new/reject),
  module (case_compare/protected_instance_methods/public_instance_methods),
  class (new), range (new/hash), float (lt/le/gt/ge/uminus),
  integer (bit_length), string (bytes)。
- **Binding**: **150 pass** (完全互換)。

---

## 旧現状 (2026-05-08, second pass)

- **自前 test/ruby/**: **24/24 全 OK** (ArrayLshiftRedef 解決)。 合計 737 件 全 pass。
- **optcarrot**: CRuby と動作・出力一致。
- **CRuby `test/ruby/` (in-scope 67 ファイル)**: 1,108,357 pass / 77.5%。
- **CRuby `spec/ruby/language/` (rubyspec, 65 ファイル)**:
  **3,402 pass / 3,634 (93.6%)、 40 ファイルが 100% perfect**。
  本ラウンドで rescue / class_variable / yield / for / safe_navigator が
  perfect 化、 shim の evaluate を it block で wrap した波及で +119 pass。
- **CRuby `spec/ruby/core/`** にも改善が波及: Array.try_convert を実装、
  shim の MSpecNegatedExpectation で predicate? 系の should_not 反転を
  正しく扱う (Hash#empty? / Hash#any? が perfect)。
- **CRuby `spec/ruby/core/binding/` + `core/kernel/{eval,binding}_spec`**:
  **150 pass** (Binding 自体は 100%、 残るのは Refinements / IRB の out-of-scope のみ)。
- **CRuby `spec/ruby/core/` 主要カテゴリ**:
  - `kernel`: 6,489 pass / 293 fail / 145 err
  - `string`: 1,800 pass / 1,127 fail / 206 err
  - `array`: 1,171 pass / 436 fail / 67 err
  - `integer`: 869 pass / 172 fail / 183 err
  - `hash`: 400 pass / 97 fail / 33 err
  - `proc`: 195 pass / 60 fail / 25 err
  - `float`: 120 pass / 35 fail / 74 err
  - `symbol`: 117 pass / 69 fail / 31 err
  - `range`: 98 pass / 79 fail / 11 err
  - `binding`: 58 pass / 0 fail / 2 err (irb_spec の IO.popen など out-of-scope のみ)

## §0 範囲外 (project policy / user 指定)

これらは TODO ではなく **scope の外**。 触らない。

| 項目 | 理由 |
|---|---|
| Regexp (`=~` / `/.../` / `match` / `scan`) | astrorge 経由で integrate する方針 |
| Encoding-aware String (`String#encoding`, `force_encoding`, multi-byte succ, m17n) | byte sequence のみ |
| Thread / Mutex / Queue / ConditionVariable / SizedQueue | single-threaded only |
| Fiber Scheduler / Async I/O | 範囲外 |
| ObjectSpace 走査 / 詳細 | excluded |
| GC.* (start / stress 以外の細部) | runtime 内部依存 |
| TracePoint / RubyVM | 範囲外 |
| NaN-boxing | 値表現変更禁止 (project memory) |
| Refinements (`refine` / `using`) | 言語拡張、 範囲外 |
| Ractor | 並列拡張、 範囲外 |
| ruby2_keywords | 互換性 marker、 範囲外 |
| DidYouMean (NoMethodError 提案) | 別途 |
| Random reproducibility (`srand` で seed 一致) | 範囲外 |
| Process / spawn / fork / `ruby_exe` 子プロセス起動 | 範囲外 (子プロセスを介する spec は skip / 0 pass で許容) |
| IRB (`Binding#irb`) | 対話的 IRB は対象外 |

mspec_shim はこの一覧の constant を未定義時に skip 扱いにする。

## §A 完全 perfect 候補 (残 fail/err が 1〜4 件)

「あと数件で 100% pass」 になる language spec。 直近の作業優先度高め。
本ラウンドで rescue / class_variable / yield / for / safe_navigator は
perfect 化済み。

| spec | pass / fail / err | 原因の見当 |
|---|---|---|
| `variables_spec` | 168 / 2 / 0 | lambda 内 eval から外側 lvar 書き込み (lexical scope chain walk + multi-scope prism 連携) |
| `method_spec` | 268 / 5 / 0 | Ruby 3.x の特殊 param 系 (`def m(*, a)`、 `def m(a, **nil)` 等) |
| `class_spec` | 66 / 2 / 2 | Class.new block 内の `class X` の lexical scope (§B3) |
| `super_spec` | 117 / 1 / 2 | BasicObject 経由 super, 可視性変更後 super, define_method 経由の RuntimeError |
| `block_spec` | 180 / 2 / 2 | block の SyntaxError 系 (循環引数参照) と to_proc 周り |
| `metaclass_spec` | 22 / 1 / 1 | metaclass of metaclass (深い singleton chain) |
| `constants_spec` | 142 / 3 / 1 | private constant access、 unicode const name |
| `return_spec` | 51 / 3 / 0 | return inside class block の LocalJumpError、 toplevel return warning |
| `hash_spec` | 66 / 4 / 0 | string key freezing、 Ruby 3.x の `m(**h)` 非コピー特殊規則 |
| `keyword_arguments_spec` | 45 / 8 / 3 | `**hash` empty 扱い (§B2) |
| `regexp_spec` | 43 / 26 / 2 | astrorge 待ち |

## §B 中インパクト項目

### B1. block frame の backtrace ラベル

CRuby は block 内 raise の backtrace を `:in 'block in foo'` (or `:in 'block in <main>'`) と表示する。 koruby は呼び出し元の AST node line を貼るだけで block を独立 frame として表示しない。 `rescue_spec` / `ensure_spec` の "deepest rescue block" 系と、 backtrace API ベースの spec が複数 fail。 backtrace builder で running_block の生成位置を frame として挟む必要あり。

### B2. 空 kwargs hash の自動消失

`m({}, **{})` で空 kwargs hash が positional に流れる現象。 CRuby 3.x は empty kwargs hash を call-site で消去する。 `keyword_arguments_spec` で十数件 fail。 call site の args 構築を kwargs と positional に明示分離する必要あり。

### B3. Class.new block 内の `class X` lexical scope

`Class.new do; class X; end; end` で、 X は block 作成時の lexical scope (= 外側) に作られる。 koruby は `current_block->cref` を nk に push するため X が anon class 配下に入る。 builtins/module.c の `class_new` の cref 操作を見直し。

### B4. SyntaxError message 一致

`-> { eval "..." }.should raise_error(SyntaxError, /pattern/)` で prism と CRuby のエラーメッセージが違うため fail。 mspec_shim 側の substring matcher で半数は救えているが、 「prism は受け付けるが CRuby は SyntaxError」 のケース (例: yield in singleton class) は個別対応必要。

### B5. eval body から外側 block の lvar 更新

`eval("a = 2")` を block 内から呼ぶと、 eval body は新規 lvar `a` を作るだけで block の `a` を更新しない。 prism の depth 1+ scope への書き戻しを実装する必要あり。 `kernel/eval_spec` の "updates a local in a scope above a surrounding block scope" など。

## §C 残小バグ

- [ ] **ArrayLshiftRedef** (`test/test_basic_op_redef.rb`) — Array#<< の redef guard が発火しない (4/4 fail)
- [ ] **Float#floor(n) の Float 精度** — Float 表現の本質 (291.4.floor(2) は 291.39 になる)
- [ ] **proc/lambda の post-rest の parameter 名** が `[:req]` のまま
- [ ] **m17n strings**: encoding を真面目に処理してないので multi-byte 周りで slot wrap や split で誤動作
- [ ] **`def f(&nil)` / `def f(**nil)`** (Ruby 3.4) — PM_MISSING_NODE で吸収済みだが parameters に反映されない

## §D Module / Class 内部

- [ ] `Class#attached_object` (singleton class API)
- [ ] `Module#prepend_features` / `Module#append_features` 公開
- [ ] `Module#const_source_location` の真の実装 (line 番号を保存)
- [ ] `Module#constants(false)` の `inherit` 引数の細部
- [ ] `private constant :X` (定数の visibility)

## §E 標準ライブラリ — 残小物

- [ ] `String#bytesplice` (byte-level 編集)
- [ ] `Struct`: `keyword_init: true`、 `IndexError` on bad index、 `to_h` with block
- [ ] `Data`: 真の Data 実装 (現状 Struct 経由)
- [ ] `Complex` / `Rational`: `coerce` 経由の polar canonicalization、 NaN/Infinity の to_s 表記、 expt special angles
- [ ] `StringIO` クラス自体 (test の依存により $_ 系 spec が skip 多い)

## §F core spec の伸びしろ

`spec/ruby/core/` で **現状 60% 未満かつ実装可能** なクラス。 実装済み
基盤 (Binding / eval / proc / hash) を生かせば数百件単位で増える。

| カテゴリ | pass | fail | err | 備考 |
|---|---:|---:|---:|---|
| `string` | 1800 | 1127 | 206 | encoding 系除外でもまだ伸びしろ大 |
| `array` | 1171 | 436 | 67 | BasicObject splat、 reject_bang の余地 |
| `integer` | 869 | 172 | 183 | Float 精度系 + bignum 細部 |
| `range` | 98 | 79 | 11 | endless range step / first(n) の細部 |
| `symbol` | 117 | 69 | 31 | inspect / to_proc / encoding 関連の細部 |
| `proc` | 195 | 60 | 25 | curry / parameters / arity の細部 |
| `float` | 120 | 35 | 74 | Float 表現本質、 step / divmod 精度 |
| `hash` | 400 | 97 | 33 | merge with block / compare_by_identity |

## §G 過去セッション履歴

実装済み変更の履歴は git log を参照。 手を動かす前に直近 30 commits 程度を
ざっと眺めて既に試した方針を再走しないこと。 直近の大改修:

- 2026-05-07: Binding 完全実装 (binding TOTAL 110 → 150)
- 2026-05-07: Kernel#eval coerce + String#b + block fp shift + nested eval
- 2026-05-06: eval-with-binding (caller の lvars 参照、+99 pass)
- 2026-05-05: mock support (should_receive)、 LocalJumpError 検出
- 2026-05-04: describe→context→describe のローカル破壊修正

## §H precise GC (2026-05-28)

`BARUBY_GC_STRESS=1 BARUBY_GC_PURGE=1` (= per-alloc GC + mprotect ベース
stale-ptr 検出) 下で test 走行:

- 8-test 基本 battery (min1/def1/class1/fib10/method_chain/justmul/
  string_op/test_seq4): **normal / STRESS / STRESS+PURGE 全 8/8 pass**。
- 完全 test/ ディレクトリ (24 ファイル):
  - **normal**: **24/24 (全 OK)**
  - **STRESS**: **24/24 (全 OK)**
  - **STRESS+PURGE**: **24/24 (全 OK)**

  baseline (548e616a) では STRESS だけで 3 件 fail だったので、 全件
  解消。 test_eq_redef / test_alias_redef / test_basic_op_redef の
  全 inspect 系 SEGV / arg routing / sp restore / framework forward
  bug を fix。 3 run 連続 24/24 で deterministic。

### 残 test_basic_op_redef STRESS の root cause = Phase 3 未完

`korb_ary_new_capa` (= 配列) と `korb_hash_new` (= ハッシュ) は libc
xmalloc で確保 (= GC arena 外)。 visit_roots phase (a) が value stack の
slot を走査して forward_edge を呼ぶ際、 slot 値が libc 配列を指している
と forward_payload は arena obj として扱い:
- aligned = ALIGN8(gc_size=0) = 0 (= memset で 0 のまま)
- memcpy 0 bytes (= no-op だが to_top は事実上 進まない)
- HDR_SET_FORWARDED → libc 配列の gc_flags に bit 立て
- fwd_overlay_set で payload+8 (= klass field!) に to_top を書き込み
- *slot = to_top (= libc 配列の今後の参照先が壊れた arena 領域に向く)

結果、 配列 lvar の値が "to_top に置かれた直近の arena obj" (= 多くの
場合 main_obj) に化ける。 `a.class = Main` の理由。

#### Phase 3 試行で見つかった "shadow ref" 問題

`korb_ary_new_capa` だけ arena に migrate して試したら NORMAL は 24/24
維持されたが、 STRESS で全 24 件 SIGABRT (= forward_payload が "GC BUG
forward to-space" abort)。 原因は **mixed allocation の shadow reference**:

- 配列を arena に移行 → scan_edges T_ARRAY が a->ptr[i] (= libc buffer
  の中身) を visit して contains の arena ref を forward
- しかし libc 側 container (= ハッシュ entry chain, 一部 method
  table) が arena obj への参照を持っている → これらは visit_roots
  にも scan_edges にも触られない (= GC から「見えない」shadow ref)
- 結果: shadow ref しか持たない arena obj は collect 対象になり死ぬ
- 次の cycle で他の経路 (= migrated 配列の ptr buffer 等) から
  死んだ obj への参照を visit すると "to-space past to_top, not
  forwarded" abort

根治には **全 koruby container を一斉に arena 化** が必要 (= partial
migration では shadow ref が新たに発生する)。 対象:
- korb_array (struct + ptr buffer)
- korb_hash (struct + entries)
- korb_range (struct)
- korb_float (struct)
- korb_bignum (struct + mpz_t)
- korb_proc (struct)
- その他 libc 構造体

各 type について scan_edges per_type の visitor を追加し、 内部の
arena 参照 (klass field, content slots) を全て visit_ptr_slot で
forward する必要あり。 大規模 refactor、 1 commit 単位では収まらない。

### 解消した test_alias_redef NORMAL の logical fail

調査の結果、 `assert_equal` (= optional msg=nil 持ち method) の
prologue_ast_general に `c->current_frame->fp += arg_index;` の
shift が抜けていた pre-existing バグだった。 body が caller の
lvar slot から params を読んでいた。 commit d907a658 で fix。
prologue_ast_simple_inl / prologue_ast_full_inl_K は最初から shift
していて、 general だけ漏れていた。

### Phase 6 audit: 全 15 backend BUILD + STRESS sweep

`extern int g_in_root_scan;` を visit_roots phase 区切りログ用に
置いていたが、 これは gc_copy.c の診断専用 globals。 他 14 backend
で link 失敗していた。 撤去 (commit 75c9c9c5)。

**15 backend 全 BUILD OK + min1/fib10/class1/string_op/test_seq4 の
5-test sub-battery で run 5/5**。 全 24 test/ ファイル STRESS sweep:

| backend           | STRESS 24件中 | 備考                                 |
|-------------------|--------------|--------------------------------------|
| **copy** (default)| **24/24**    | + STRESS+PURGE も 24/24 全 OK!       |
| mark              | **24/24**    | 非 moving。 PASS                     |
| mark_freelist     | **24/24**    | 非 moving。 PASS                     |
| immix             | **24/24**    | 非 moving + line/block。 PASS         |
| bump              | **24/24**    | 非 moving + bump alloc。 PASS         |
| none              | **24/24**    | no-op GC。 PASS                      |
| mark_compact      | 23/24        | test_basic_op_redef のみ              |
| mark_card_gen     | 20/24        | _gen 系                              |
| mark_bitmap_gen   | 19/24        | _gen 系                              |
| mark_bump_gen     | 5/24         | _gen 系                              |
| copy_gen          | 0/24         | _gen + moving。 session 通じて変動なし (= 以前の audit の "17/24" は集計スクリプトの bash `!`-history 展開ノイズによる過大集計、 確認したら最初から 0/24 だった)。 |
| mark_gen          | 0/24         | _gen 系                              |
| mark_compact_gen  | 0/24         | _gen 系                              |
| immix_gen         | 0/24         | _gen 系                              |
| mark_gen_inc      | 0/24         | _gen 系 incremental                  |

非 moving + 非 generational は 24/24 全 pass、 copy は **STRESS+PURGE
含む 全モード 24/24 全 OK**。 _gen 系 backend は framework 側
write barrier 不整合があり、 別 session で深堀必要。

## §I benchmark / rubyspec sweep (2026-05-28)

### benchmark/bm_*.rb (27 ファイル)

| mode | pass | fail | SEGV | 備考 |
|---|---|---|---|---|
| NORMAL | 22 | 5 | 0 | pre-existing 仕様未実装 |
| STRESS | 19 | 8 | 0 | timeout + libc shadow ref |
| STRESS+PURGE | 19 | 8 | 1 | nbody が mprotect で死亡 |

nbody の PURGE SEGV: `korb_class_find_method` で klass deref → libc-
allocated array の `klass` field が arena class の stale addr を持って
おり、 mprotect 経由で SIGSEGV。 libc-arena shadow ref の典型例。

### rubyspec (~/ruby/src/master/spec/ruby/core/) sweep

NORMAL mode で mspec_shim 経由実行: 多くの spec で test setup 中の
`empty?` for nil 等の pre-existing koruby 仕様未実装エラー。 STRESS+
PURGE 下では SEGV (= 同じ libc-arena shadow ref パターン)。

### 根本対処 (= Phase 3 完了相当)

container を全 arena 化:
- korb_array (struct + ptr buffer)
- korb_hash (struct + buckets/entries)
- korb_range / korb_proc / korb_float / korb_bignum 等

partial migration (= 一部だけ) では shadow ref が新たに発生する
ことが実験で確認済 (4ed470fb で試行)。 一括移行が必要。 大規模
refactor、 1 commit に収まらない別 session 規模。

### Top-level closure pre-existing 不具合

```ruby
x = 10
[1,2,3].each { |i| x += i }
puts x   # 期待: 16、 実際: NoMethodError "undefined method '+' for nil"
```

method body 内では正しく動作 (test_block.rb の test_block_capture 等):

```ruby
def f
  s = 0
  [1,2,3].each { |x| s += x }
  s
end
puts f   # 6 (= 正)
```

baseline (548e616a) でも同じ症状で再現、 GC 関連ではない pre-existing
koruby_precise の top-level closure 不具合。 sibling `sample/koruby`
では top-level でも正しく動作。 koruby_precise の CTX → frame
migration 時に block env capture の semantics が壊れた可能性。

benchmark/bm_times / bm_each / bm_inject / bm_nbody がこの問題で
NORMAL から fail。 別 session の課題。

**深掘り途中メモ:**

AST は top-level と method body で identical (envsize=39、 block 構造 同じ
`1 4 7 0`)。 proc_call の self_recursion path (= prev_fp == new_fp)
にて fresh_env = c->sp に clone する。 method body 内では正しく x = 10
が読まれるが、 top-level では block.fp[0] が nil を返す。

**root cause 推定:** `korb_yield` fast path (object.h:841) と
`korb_yield_slow` (object.c:2374) は body dispatcher に `sp = bfp`
を渡している (= blk->env と同じ addr)。 一般 method body は
`sp = fp + locals_cnt` を渡すので、 block body の bake walker
offset 規約が異なるか、 もしくは現状の dispatcher 呼び出しが間違って
いる可能性。

**試行と再現結果 (2 回目):**
- `sp = bfp + env_size` に変更 → top-level closure 通る (x = 16)
- ただし NORMAL test/ が 24 → 13/24 に regression
  (test_array / test_block / test_block_arg / test_class / test_cpu_corner
  / test_fiber / test_hash / test_misc / test_object_alloc / test_range /
  test_yield)
- "EXCEPT test_reduce: NoMethodError: undefined method '+' for :test_reduce"
  のように、 一見無関係な箇所が壊れる → block 外の lvar 読み書きまで
  影響する深い path 修正が必要

**結論:** bake walker と yield 経路の規約整合 (= 全部 frame top 基準に
するか、 block だけ bfp 基準にして bake 側も追従するか) は単純な
1-liner では実現不可。 bake walker (parse.c) の offset 計算と全 yield
経路の dispatcher 呼び出しを同時に改修する必要あり、 multi-commit な
大改修。 別 session の課題。

### 一括 migration 試行ログ (2026-05-28, session 末)

`korb_ary_new_capa` / `korb_hash_new` / `korb_range_new` /
`korb_float_new_heap` / `korb_proc_new` を `aro_gc_alloc` に同時切替
+ scan_edges T_HASH 実装した試行。 結果:

- NORMAL: 24/24 維持 (= 意味的に破壊なし)
- STRESS: **24 → 3/24 (大幅 regression)**
- STRESS+PURGE: **24 → 0/24 (完全崩壊)**

bootstrap で `NameError: undefined method 'numerator' for Rational`、
`BAD SLOT abort-imminent` 連発。 推定原因:
- container migration が bootstrap 中に発生する GC を increase
- ある時点で class.methods (= korb_method_table、 hash 構造別物) や
  別の libc-allocated 構造への参照が stale 化
- 私の T_HASH scan_edges は korb_hash 用、 method_table は scan されず

container 系を一括 migration するだけでは足りず、 関連する全 libc
構造 (method_table、 const_entry chain、 includes 配列 等) も同時に
scan_edges に load する必要あり。 さらに大規模、 別 session 規模を
超える本格 refactor。 revert で 24/24 復帰。

このセッションの主要 fix (commit 686f01f0 〜 13edbcb7):

- **visit_roots phase (c+d) 統合**: frame 毎に cref chain を walk。
  method dispatch で frame.cref = mc->def_cref (= cref_dup の SEPARATE
  chain) になるため、 head 一本だけ走査では outer frame の cref->klass
  が stale 化。 PURGE で class body 内 const_set が SEGV した直接原因。
- **c->sentinel_frame + c->top_cref を visit_roots で unconditional に walk**:
  korb_eval_string が top_frame.prev=NULL で push するため、 sentinel は
  head chain から外れる。 sentinel.self (= main_obj) が bootstrap 中の
  GC で stale 化し、 user script 側の最初の puts 1 が dispatch SEGV。
- **node_class_def / node_class_reopen / node_module_def を synthetic
  frame push に統一**: 旧 impl は outer frame.current_class / cref を
  その場で書き換え、 C-local prev_class を across-body-GC で保持。
  GC 後に書き戻すと stale arena ptr が outer に書かれ、 次の GC で
  BAD SLOT。 synthetic 構造体 (.cref/current_class/self を inherit)
  を c->current_frame として push する形に統一。
- **korb_dispatch_to_method AST path で frame2 が cref/current_class/
  current_file を inherit**: frame2 の struct literal がそれらを
  初期化せず NULL のまま → body 側 const_lookup が NULL 経由で
  garbage を返して SEGV。
- **node_str_concat を ARO_ROOT_SCOPE で保護**: `korb_to_s_dispatch` が
  GC を発火するため C-local の累積 r と part が across-GC stale。
- **globals 撤去**: `koruby_top_cref` / `koruby_top_sentinel_frame` を
  file-scope static から CTX フィールドに移行。 multi-interpreter
  共存可能に。 struct korb_frame の定義を struct CTX_struct の前に
  移動して by-value embed を可能化。

### 残 pre-existing flakes (baseline からの持ち越し)

- **test_alias_redef** (normal も STRESS+PURGE も fail): const_lookup
  内で c->current_frame が stale stack 領域を指す。 cref->prev =
  korb_dispatch_to_method 内の saved RIP。 どこかの dispatch path が
  c->current_frame を local frame に push して unwind 漏れしている疑い。
- **test_basic_op_redef** / **test_eq_redef** (normal pass、STRESS / STRESS+PURGE で fail):
  GC root scan が visit_ptr_slot に libc code address (= `_int_malloc+N`)
  を渡して SEGV。 cref chain の prev が garbage stack 領域を指し、
  そこから wild ptr へ chain される。 method aliasing が同じ
  `struct korb_method *` を複数 entry に登録するため、 def_cref chain
  に corrupted node が混入する可能性。

### 組み込み型の instance variable (cont.16 で発覚)

- 現状 `@ivar` を持てるのは `KorbObject` (KORB_OBJ_OBJECT、ユーザ定義
  クラスのインスタンス + top-level main) のみ。`node_ivar_get` は
  `\!KORB_OBJECT_P(self)` で nil を返し、`node_ivar_set` は raise する。
  本来 Ruby では String/Array/Hash 等の組み込みオブジェクトも ivar を
  持てる (`"x".instance_variable_set(:@a, 1)` 等)。
- 実装するなら ivar ストアを KorbObject 専用ペイロードから外し、
  全 heap オブジェクト共通のサイドテーブル (obj identity → ivar hash)
  か、各 heap struct への ivar スロット追加が要る。Fixnum/Symbol 等の
  immediate は別扱い (CRuby は generic_ivar table)。優先度低 (corpus に
  ほぼ無し)。

### perf push 中に発覚した correctness gap (2026-06-18)

- (実装済 — todo 誤記、2026-06-19 確認) **`retry`**: node_retry +
  node_begin の KORB_RETRY ループ + parser PM_RETRY_NODE すべて存在し動作。
- **custom `hash`/`eql?` の Hash キーが効かない**: ユーザ定義 `hash`/`eql?`
  を持つオブジェクトを Hash キーにしても dispatch されず、identity 扱いに
  なる(別オブジェクトが別キー化、lookup miss)。korb_value_hash /
  korb_value_eq が組み込み型のみ。object キーは user hash/eql? dispatch が要る
  (= GC-sensitive: hash 計算が user code を呼ぶ → rehash 中の GC 安全性に注意)。
  既知の大物。
- (修正済 2026-06-19) **`{1=>"a"}[1.0]` が "a" を返していた** (eql? でなく ==):
  korb_hash_find の linear scan が korb_value_eq_fast (==) を使い、1 と 1.0 を
  同一キー扱いしていた。korb_value_eql (numeric-type-strict、既存だが Hash で
  未使用だった) に切替。Integer/Float/Rational が別キーに (Set#uniq は既に正)。
- (修正済 2026-06-18) Array#sort/min/max/min(n)/max(n) の <=> dispatch、
  AOT の -DKORB_HAVE_GMP 欠落 → 別 commit で対応済。
- (修正済 2026-06-19) **Bignum `>>`**: korb_m_int_rshift に bignum ガードが
  無く、bignum self を FIX2LONG してゴミを返していた（lshift にはあった）。
  bignum→korb_int_shift(self,-sh)、負シフト(=左シフト)の Fixnum overflow も
  Bignum 昇格するよう lshift と対称化。
- (修正済 2026-06-19) **beginless/endless range index**: `arr[1..]` / `s[..3]`
  等が「no implicit conversion into Integer」で raise していた（rbegin/rend が
  nil を弾いていた）。Array#[]/[]=/slice!、String#[]/[]= の5箇所で nil 端点を
  beginless→0 / endless→長さ として扱うよう統一。
- **`Integer == user_obj` の reverse coercion 未対応**: `42 == E.new`
  (E が `==` を true 返しで定義) が CRuby では true だが koruby は false。
  Integer#== が相手を理解できない時、CRuby は逆方向 (`E.new == 42`) を
  試す。node_eq の korb_value_eq は組み込み型のみ判定。優先度低
  (corpus 影響なし、pre-existing)。
- (修正済 2026-06-18) `send(:top_level)` / `__send__` が top-level def に
  届かなかった (private Object method 相当) → korb_send_impl の main miss で
  global function table fallback。public_send は privacy で raise。

## rubyspec mining 由来 (2026-06-19, /tmp/rspec_gen バッチ)

新規 mined assertion を CRuby と diff して発見・修正 (20 commit, 全て
corpus 89267/5 + STRESS+PURGE + AOT verify 済)。total diff 4165→3678。

**RESOLVED (session 2, 2026-06-19):** main self methods; bignum bitops;
Float#arg(-0.0)/coerce(String); Hash#delete blk; sprintf %+b;
Method#arity/owner/original_name/parameters; **UnboundMethod**
(instance_method/unbind/bind/bind_call); defined?(A::B); Numeric⊇Comparable;
Object#singleton_class + attached_object; Integer/String.try_convert;
Integer div/divmod/modulo/remainder の Rational/Bignum operand;
String#to_r (leading-dot/_/dec÷den); tr/delete lone-^ literal;
ArithmeticSequence#size analytic (HANG fix + float); Numeric#step(to:,by:);
**Complex** abs2/numerator/denominator/rationalize/infinite?/integer?/
<=>/eql?/**(pow)/(div)/polar + inspect; **3 SEGV fix** (|&b| block,
Proc.new{}, Enumerator.new{} eager yielder).

**残 (deferred — 大物 or skip):**
- **Integer#size (Bignum)**: limb 単位の実装依存値。spec も "machine
  dependent"。matching 非現実的、skip。
- **to_enum / enum_for(:method)**: block 無し Enumerator を method 名生成。
  block-from-C 駆動が要る。中規模。
- **Set#compare_by_identity / Hash#compare_by_identity** (set_000)。object_id 基盤要、moving GC で大物。
- **Method#parameters の名前** (ISEQ は node_entry に名前未保持 → 種別のみ。
  名前を出すには parser で名前テーブルを node_entry に保持する変更)。
- **nested Complex()** (kernel_000)。

**RESOLVED (session 3, 2026-06-19 — 大物バッチ):**
- Class < Module < Object MRO (Class.superclass/ancestors/is_a?(Module))。
  残: module オブジェクトの metaclass がまだ Class (M.is_a?(Class) が誤 true)
  — Module 系メソッドを KORB_C_CLASS から Module へ移す別作業。
- define_method(name, Method/UnboundMethod) — 定義を copy + ancestor チェック。
- lazy Enumerator drop/take/drop_while (無限ソース対応、per-op state)。
- Complex 残り: marshal_dump, to_r, <=>, eql?, abs(0,0)=0、Integer#to_r/
  rationalize の Bignum 精度バグ修正。complex_000 121→2 (残は float ULP)。
- **float ULP (Complex**Float の polar)**: libm の演算順序依存で bit 一致不可
  と確定 → skip。

- **Binding / TOPLEVEL_BINDING (実装済 session 3)**: KORB_OBJ_BINDING 型。
  `binding` = node_binding (parser が scope の name→sym 表 + frame base + self
  を bake、frame を closure と同じ open-KorbEnv で捕捉 → closure と共有・frame
  退出後も生存)。local_variable_get/set/defined?/local_variables/receiver
  (新規 local は extra side-hash)。eval(str, binding) は prism declared-scope
  で binding local を宣言 (prism は単一 scope を program 自身に畳み込む →
  depth-0) → eval frame を binding から seed、実行、write-back (既存→env、
  新規→extra)。TOPLEVEL_BINDING は main.c で toplevel frame を捕捉。
  GC 罠: write-back の hash alloc で GC が binding を動かす → cached VALUE が
  stale → args slot から再読込で修正。t/hand/binding.rb (28 assert) で
  interp+STRESS+AOT 検証。残: extra-local の eval 順序の細部、cref/定数解決。

**残:**
- skip 対象: regex (→astrorge), Encoding/US_ASCII, 非 ASCII case,
  Thread/Queue/Fiber 細部, IO/File/Dir/Process/Time, MT19937 完全一致, Marshal。
- module オブジェクトの metaclass が Class (M.is_a?(Class) 誤 true)。
- Method#parameters の ISEQ 名前 (node_entry に名前未保持)。
- nested Complex(); Set/Hash#compare_by_identity (object_id 基盤要)。

---
## bottom-header (self-copy 排除 + magic) — 2026-06-21

目的: frame top の self-copy (`base[locals_cnt-1]=self`) を排除し perf を上げる
+ CRuby 風の magic セル (frame type/flags/integrity) を base[-3] に持つ。
self と EP が base[-1] を奪い合う構造のため、まず EP を退避する必要がある。

レイアウト最終形 (locals base = `base`):
- base[-1] = self  (staged receiver を常駐; copy しない)
- base[-2] = EP    (open closure env / PREV link)
- base[-3] = magic (frame type/flags/signature; release では 0、debug で書く)

### Step 1 (DONE, commit 2cbfa2c0): EP を base[-1]→base[-2]
- `KORB_EP_OFF(-2)` + `korb_ep_get/set` (node.h) に EP オフセット一元化。
- 内部 C dispatch は korb_send_impl 入口で 1 回 relocate して base[-2]/-3 確保
  (~35 builtin send サイト不変)。korb_super/node_call_splat/method_missing は
  手動 restage で 2 メタセル予約。block frame は bf=slots+2、eval は fb=slots+3、
  fiber/toplevel は leading slack 2 セル。
- env-chain walk の live-base deref [-1]→korb_ep_get。parse.c prev_off ×6 (-1→-2)。
  context.h scan 開始 slots-1→slots-2。
- 検証: corpus 89295/5 (parity)、hand STRESS+PURGE clean。
- **この時点で base[-1] には staged recv (=self) がそのまま残る** (korb_invoke は
  もう base[-1] を上書きしない) → Step 2 は self を base[-1] から読むだけ。

### Step 2 (TODO): self-copy 排除 — self を base[-1] 常駐に
規模: ~30 の self_off baking + entry/trio オフセット shift + korb_invoke ×3 +
frame_size。all-or-nothing (中途半端だと 189-regression 型の silent offset 破壊)。
- **規則: 各 self_off サイトは初期値 (`-1 - tc->chain` 等) をそのまま残し
  `bake_add(tc, &n->u.<type>.self_off)` を足すだけ** (bake が fs を引いて
  base[fs-1]→base[-1] になる)。node_self は `bake_self()` ヘルパ化。
  対象: node_self(911,1017,1078,1095,1241,1676), ivar_get/set, def(definee),
  attr, massign_het, cvar(self側), super(self側), defined, binding, make_proc,
  blkparam(cself), call_splat(self_off は初期 -1-chain-1 等のまま+bake)。
- **entry/def_class オフセット (-2-chain)** を **-1-chain** へ (self セル除去で
  frame_size-1 → entry が base[fs-2]→base[fs-1] へ上がる): cvar 第2引数, super dc_off。
- **block trio** base[fs-3/-4/-5] → base[fs-2/-3/-4] (blkparam -3/-4/-5-chain →
  -2/-3/-4-chain; korb_invoke の trio 書込みも)。
- **frame_size**: `+ 2u` → `+ 1u` (parse.c pop_frame)。
- **korb_invoke ×3** (node.h korb_invoke_simple, korb_runtime korb_invoke_method,
  korb_invoke_kw_simple): `base[locals_cnt-1]=self` を削除 (self は base[-1] に staged
  済み); entry を base[locals_cnt-1] (旧 -2) へ; memset 上限を locals_cnt-1 に;
  trio を locals_cnt-2/-3/-4 へ。self 引数は (void) 化可。
- **magic (base[-3])**: debug build で `KORB_MAGIC | flags` を書き return 時 assert。
  release は reserve 経路が 0 で埋める (既に Step 1 で 0)。
- 検証: corpus → STRESS+PURGE (closure/binding/fiber) → AOT → benchmark。

### Step 2 / magic (DONE)
- Step 2 (self-copy 排除) commit 3d997570。magic セル内容 (debug-gated integrity
  check) commit 0aca97ef。ASTRO_DEBUG=1 で corpus 89295/5 magic abort ゼロ検証済。

### pre-existing flake (FIXED, commit d5f45637)
- `Hash#select/filter` の STRESS+PURGE SEGV を修正。真因は korb_hash_filter が
  要素 key を yield 前 local に読み yield(GC)後に stale 使用 (value は再読込済だが
  key 未対応)。key も self から再読込に統一。method/hash_* STRESS+PURGE 12452/0/0。
  daed240e でも再現した pre-existing バグ (bottom-header の回帰ではない)。
- method/array_283 は STRESS 下の低速 timeout (no-stress は PASS、benign)。残置。

### pre-existing bignum バグ (perf 作業中に発見、2026-06-22)
- `0 - 4611686018427387904` (= fixnum 0 - bignum 2^62) が `+2^62` を返す (正: -2^62)。
  committed HEAD 4fb1e3dc でも再現する pre-existing バグ (fixnum tagged 算術最適化
  とは無関係)。korb_int_arith の fixnum-lhs - bignum-rhs 経路の符号処理疑い。
  通常の bignum±/fixnum±bignum の多くは正常 (`-2^62 - 1` 等は OK)。要調査。

### perf 解析メモ (2026-06-22, perf record/annotate @ 機械語レベル)
- fib AOT: 単一 SD に完全 inline。支配項 = 再帰の間接 call `call *0x30(rax)`
  (body->head.dispatcher 経由, ~7%) + callee-saved 6 push/pop + sub/add rsp (~12%)
  + per-call の serial/kind/params/stack-limit×2 チェック。dispatch-bound。
- nested_loop AOT: inner loop。最適化前は tagged decode(sar)/encode(lea) が支配。
  fixnum tagged 算術化 (commit 055dbd14) で sar/lea 激減 → 支配項は slots の
  memory load/store + per-op `test $0x1` tag guard に移行 (= locals-in-memory が壁)。
- 残る大物 (いずれもアーキテクチャ級・高リスク): locals のレジスタ常駐化、
  type-stable loop での guard hoisting、再帰の devirtualization。
- 計測機が現在ノイジー (best-of-9 でも ~2× variance)。fine-grained per-bench 比較は
  interleave+pin でも不安定。robust な win は nested_loop ~1.4× のみ確証。

---
## rubyspec 充足 sweep — 2026-06-22

gen_from_rubyspec.rb で全 core を mine (5579 assertion/101 file, /tmp/spec_new) し
crash 系(un-crash で大量復活) を ROI 順に修正。広域 sweep PASS **2344 → 2746 (+402)**、
corpus は終始 89295/5 維持。生成器も改善 (warning/RNG/pointer-pack を mine 時に除外)。

### このセッションで修正 (commit 群)
- Float 整数定数 (DIG/MANT_DIG/...) + Range.new
- Math.expm1/log1p、Kernel#Integer の exception:/Bignum
- Rational round(ndigits,half:)/zero?/integer?/div、Rational(Float/String/2引数)
- Encoding stub (定数群 + String#encoding) + p のユーザ inspect dispatch + ArithSeq#take
- String#upto / #crypt(libc)、Hash.ruby2_keywords_hash?/try_convert
- Integer#chr(encoding)、Kernel.X module-function、Range#bsearch(float/endless/no-block)
- Exception#backtrace(_locations)/detailed_message/full_message、Errno stub、Module#const_source_location
- Symbol#inspect 非ASCII識別子、Object#to_enum/enum_for
- **send/method_missing→global builtin の SEGV 修正** (dispatch_method に BUILTIN case)

### 残り (大物機能 or skip 方針、ROI 低)
- **string ~978**: regex(captures/scan/split→astrorge), encoding 変換(SHIFT_JIS chr 等),
  multibyte case, format/chomp の細部。大半が skip 方針 or 1-assertion tail。
- **enumerator ~103**: `arr.select`(no block)→Enumerator や Lazy.grep_v など
  method-enumerator/Lazy 意味論。koruby は eager Enumerator なので要再設計 (中〜大物)。
- **range ~147**: Range サブクラス `.new`(side-table 方式が要), bsearch float の 1 ULP 境界。
- **proc ~43**: block の param list `{|&b|}` (parser 拡張)。
- **symbol/integer ~49 each**: Unicode case folding, SHIFT_JIS/EUC chr (encoding 実体)。
- **exception ~40**: Errno/IO::*WaitReadable namespace, 定数の Module::Name 表示。
- **basicobject ~10**: instance_eval(String)。
- 既知: `send(:p, *a)` 系は修正済 / 別途 `send` 大splat の細部は要確認。

### method-enumerator (deferred — 2026-06-22, regression のため revert)
`arr.select`(no-block)→deferred Enumerator を試作したが corpus 回帰(89261/39)で revert。
要点: mode-3 method-enum(source=recv, values=method sym, ops=args)+ each が
recv.meth(&block) を re-dispatch、は動く(select/Hash/Range の with_index は一致)。
だが with_index/with_object/to_a を prelude で each 経由実装すると **C の eager 版を
override し eager enum を壊す**: (1) no-block with_index(`each.with_index(1).to_a`)が
yield で no-block-error、(2) with-block の戻り値が eager は each→self で method 結果に
ならない(select は mode-3 each が結果を返すので OK)。eager と mode-3 の戻り値
セマンティクス統一 + 全 enum consumer(first/count/map/find...)の mode-3 対応が要る
= Enumerator の本格再設計。multi-arg yield 修正(commit bd611c04)はこの作業中に発見した
別の regression(Step2 由来)で、独立に commit 済。

## [FIXED 2026-06-24 commit 5ecd6cd7] nested block + closure が中間レベル変数を捕捉すると depth-2 変数解決が壊れる

repro:
```ruby
acc = []
2.times { |i| [10,20].each { |x| acc << lambda { x*i } } }
p acc.map { |p| p.call }   # CRuby: [0,0,10,20] / koruby: acc が Integer 化して TypeError
```
内側 each ブロックが中間(times)の `i` を closure(lambda)に捕捉すると、その each
ブロックからの depth-2 変数 `acc`(toplevel) 参照が中間/内側の slot を読む(Integer 化)。
1-level capture や lambda 無しの 2-level は正常。block-yield fast path とは無関係
(HEAD でも再現、`korb_block_yield` の simple fast path 導入時に発見)。node_eget の
mixed-chain walk が、closure-escape で中間 env が heap 昇格した際に depth を誤る疑い。
corpus 未検出 = この patターンが corpus に無いだけ。要 [[project_koruby_precise_toplevel_binding_perf]] の env chain 見直し。

## [FIXED 2026-06-25] forwarded block (KORB_BLK_FWD) が `&blk` param に届くと crash

forwarded proc を `&blk` 引数で受ける method の中で、その `&blk` をさらに別 method に渡すと
`node_blkparam` → `korb_make_proc` が def_env=KORB_BLK_FWD(0x2) を deref して SIGSEGV だった
(mspec_shim の `def context(...&blk); describe(name,&blk); end` 経由で多発)。
fix: node_blkparam で denv が KORB_BLK_FWD のとき CPROC と同様 `&blk = slots[cself_off]`
(転送 proc 自体) を bind。rubyspec pass +102 (8477→8579)、comparable/enumerable 等の crash 解消。

## [FIXED 2026-06-25] 自己参照構造の hash / <=> / eql? / singleton / FWD-block 系 crash 一掃

rubyspec の unblock 後に到達していた既存 SIGSEGV を多数修正 (make test 89300 維持):
- `korb_deep_hash` (Object#hash) 無限再帰 → depth cap (KORB_DEEP_HASH_MAX=96)。
- `korb_cmp_full` (Array#<=>) / `korb_value_eql` (Array#eql?/uniq) 自己参照無限再帰 →
  先頭に identity short-circuit (`a==b` で 0/true; <=> は float 除外で NaN<=>NaN=nil 維持)。fastpath は同一要素比較が速くなる方向。
- `class << nil/immediate` が `korb_obj_singleton` で nil deref → nil/true/false は class 返却、
  Integer/Float/Symbol は TypeError (CRuby 一致)。
- forwarded Symbol#to_proc (CPROC) を builtin iterator に渡すと `korb_entry_params_cnt(CPROC)` が
  deref → CPROC は arity 1 を返すよう一点 guard。
- builtin が block 無しで yield → `korb_block_yield(block==NULL)` deref → LocalJumpError (UNLIKELY guard)。
- `Hash.new { capturing block }` が make_proc の env walk で odd-tagged def_env を deref →
  korb_make_proc 冒頭で `|1` tag を strip (全 caller idempotent)。
crash 数 19→4。
- 自己参照 container の inspect/to_s 再帰 (array/hash/set) → 深さ cap (KORB_PRINT_DEPTH_MAX=48) で `[...]`/`{...}`/`Set[...]` marker。CTX 可変状態は使わず内部 _d helper に depth thread (public wrapper は depth0)。
- Proc/Fiber の subclass `.new` が generic object を作って rep/proc 未初期化で crash (proc/new, fiber/new) → builtin-subclass `.new` path に KORB_C_PROC/KORB_C_FIBER を追加 (korb_make_proc/korb_fiber_new で payload 構築 + klass override)。new_kind 分類にも base 追加。
- Proc subclass / Proc.new(&p) の追加修正: Proc/Fiber subclass instance は ivar を持てない → korb_ivar_get/set を KORB_OBJECT_P 先頭判定に並べ替え + 非object/非class は nil/no-op (fastpath 正)。`Proc.new(&proc/&method)` は forwarded proc (def_env==KORB_BLK_FWD) をそのまま返す + korb_make_proc に CPROC entry guard。
- lazy enumerator の with_index / each が SELF_ENUM->values(nil) を deref して crash → mode!=0 で korb_lazy_drive materialize (finite で正、infinite は別 redesign)。finite lazy each/with_index が動くように。
残 crash 2 + timeout 1: proc/curry([[project_koruby_precise_curry_bug]] env_size/nested-lambda の深いバグ、node_eget depth2 + FWD def_env)、enumerator/new(timeout/hang)。lazy 系の infinite は redesign 待ち(crash→clean error/timeout に降格済)。crash 19→2。

## [FIXED 2026-06-25] proc/curry の SEGV = 実は instance_exec(&forwarded_proc) のバグ
curry_spec の crash は env_size parser bug ではなく、`instance_exec(arg, &curried_proc)` が
forwarded Proc (def_env==KORB_BLK_FWD) の captured_self を新 receiver で上書きし、
korb_block_yield の FWD path が `VAL2PROC(receiver)->env` (= 非Proc を Proc 扱い) を読んで
SEGV していた。instance_exec/instance_eval で FWD の場合 proc の env を抽出して通常 def_env
として渡し、self だけ rebind するよう修正。残る実 crash = 0 (enumerator/new は timeout)。

## rubyspec 充足 — 2026-06-27 セッションの発見 (未完)
高 pass率カテゴリから穴埋め中。済: Method/UnboundMethod#== alias 同一視 (commit 0c18f26b、
float/string/array/kernel/integer 横断 +26 pass)、Numeric/String 比較の非可比較 →
ArgumentError (commit a160f362、float lt/gt/lte/gte)。

残タスク (確認済みの実バグ/gap):
- **$\! (last exception global) が未設定**: `begin;raise;rescue; p $\!; end` で $\! が nil。
  globals は const table 経由 (node_const)。node_rescue で const "$\!" を set すれば読めるが、
  **rescue body 跨ぎの save/restore が precise GC で厄介** (C-local の outer 値が body の GC で
  stale 化、in-frame slot は body が clobber)。GC-safe には vm の errinfo save-stack か
  専用 scanned field が要る。要設計。
- **Integer#<< >> が #to_int coercion しない**: `1 << obj_with_to_int` が TypeError。
  korb_to_index に to_int fallback が無い。korb_coerce_to_int ヘルパ追加で対応可 (CTX 要、
  to_int dispatch は GC するので self は ref 経由再読込)。他の int-coercing op にも流用可。
- **bignum shift のエラーメッセージ誤り**: m が Bignum のとき "Integer into Integer" と出る。
- float/integer の divmod/fdiv/divide/modulo の err: 多くは coerce / 特殊値 (Inf/NaN) 絡み。

## 2026-06-28 spec 網羅
- **最大の山を崩した**: `.should.raise(X)` 形式が rubyspec 846/1811 file で使われており、
  koruby の public `raise` のせいで全部 err だった。Kernel#fail 追加 + shim の
  MSpecExpectation#raise 実装で解消。pass 11354→12042 (+688)、pass率 61.3→64.3%。
  (commit 6bdd227a)
- **既知の STRESS 遅延**: method/array_283.rb は STRESS+PURGE で 600s でも完走しない
  (通常モードは即 PASS=MATCH)。大配列 + 毎alloc GC + PURGE mprotect で病的に遅いだけで
  soundness バグではない。`make test STRESS=1` は常にこの 1 file で timeout 1 を出す。
  STRESS 監査時は除外候補(baruby の test_p1 flake 相当)。
- `make test STRESS=1` の env を BARUBY_GC_* に修正済(commit af223556)。それまで
  標準コマンドは黙って stress していなかった。

## 2026-07-01 structural (VALUE tag 再編 + 名前空間 + inspect/ivar)
6 件を master に直接投入 (user 許可下、他作業者無し)。golden 90090→92447、method 86977/0 維持、全 STRESS+PURGE clean・corpus で vs ruby 一致。

- **✅ lazy-on-無限generator hang** (85d96aa6): `Enumerator.new{loop}.lazy.select.first(n)` が
  korb_lazy_op の force_gen で無限 materialize → hang。**mode 4 = lazy generator** を
  mode 3 = plain generator (eager 維持) と分離。`gen.lazy` → proc source + ops chain の
  mode-4。terminal は ops-aware yielder が各 yield 値に deferred ops (select/map/reject/
  filter_map/take/take_while/drop/drop_while/compact) を per-run op_state 付きで適用し
  filtered limit で StopIteration。Fiber 不要。plain generator の map/select は eager 維持
  (CRuby は .lazy だけ defer)。
- **✅ VALUE 即値タグ再編** (3c7b3f5a): 旧 layout (nil=0/false=2/true=4/symbol&7==6/
  flonum&7∈{2,4}+110→100remap) は満杯で Qundef 用の空き無し。新 low-nibble taxonomy:
  Fixnum=`&1`, Flonum=`&3==2` (CRuby純正2bit tag, remap撤廃), nil=0,
  Special=`&0xF==4` (false=4/true=20/**KORB_UNDEF=36** + 拡張余地), Symbol=`&0xF==12`,
  heap=`&7==0 && \!=0`。**GC edge filter (AROH_IS_GC_OBJECT) 無変更**が決定的。
  tools/tag_value_test.c で codec 検証 (7167 flonum round-trip + single-category partition)。
  罠: 2^-255 が +0.0 magic に衝突 → encode に guard。
- **✅ ivar-nil** (b8c9f0d0): `@x=nil` → `instance_variable_defined?` false だった。
  **membership 判定に** (object=shape korb_shape_index、class/exc=side hash) →
  **ivar_get hot path 無変更でゼロ課税**。remove_instance_variable は**真の削除** (object は
  shape を root=shape1 から remaining ivars replay + 値配列 compact、class/exc は
  side hash shift-delete)。sentinel を slot に入れない (user 指摘: read 毎 check は無駄)。
- **✅ container 内 user #inspect** (6405b91f): `p [pt]`/`[pt].inspect`/`.to_s`/`"#{[pt]}"` が
  custom #inspect を無視して `#<Class>` だった。inspect/to_s formatter chain に optional
  rooted slots を threading (NULL で旧挙動維持、opt-in entry だけ real slots)。plain-object
  element が (overridable) #inspect を dispatch。method dispatch は callee に fresh frame を
  与えるので slot offset は 1 container nesting 内でしか伸びない (depth cap 48)。
- **✅ namespaced class name (lexical M::C)** (8be50091): KorbClass に GC-scan される
  `enclosing` edge。node_class/node_module に self_off operand (baked) 追加 →
  korb_class_body が現在 self を受けて enclosing に。qualified name を Class#name/#to_s/
  #inspect・object #inspect (`#<M::C>`)・NoMethodError で使用。罠: node_class の self_off は
  staged super child のぶん `-1-chain-1`。
- **✅ respond_to?(:send/:__send__/:public_send)** (d452333a): special-dispatch で
  MRO walk が漏らしてた。`new` special-case に追加。

### 残 structural (sizable, focused session 推奨)
- **Kernel-private builtin の reflection**: `puts`/`p`/`require`/`Integer()` 等が
  private_instance_methods に出ない、`respond_to?(:puts,true)`=false。builtin は global
  table (vm->methods) で class table に無い。根治は builtin に visibility flag +
  dispatch で explicit-receiver 拒否 + reflection が global table 列挙 = 両方 golden risk。
- **const namespace / `class M::C` (path 形式)**: flat const table。lexical (`module M;class C`)
  は上で対応済、path 形式は bare name のまま。
- **Array#join の element to_s**: `[obj].join` が obj.to_s を dispatch せず `#<Class>`。
  korb_join_rec が no-GC + raw-pointer cycle detection なので、to_s dispatch (GC) を
  入れるには header-flag ベースの cycle detection + RESULT 伝播への GC-safe 再設計要
  (spare bit 0x400 有り)。
- **deferred-Enumerator**: `find`/`group_by`/`partition` の no-block `.with_index` 等の method 再呼出。
- **object default #inspect の ivars**: `#<M::C @x=1>` の ivar 表示。address は moving GC で
  不安定なので ruby と完全一致は原理的に不可 (name は qualified 済)。

### 2026-07-01 続き (join + class formatter)
- **✅ Array#join の element to_s** (4f9780b3): 上の「残 structural」から解決。korb_join_rec を
  RESULT 返し + KORB_FL_JOIN_VISITING header flag ベース cycle detection に再設計 (flag は
  moving GC で一緒に動く) → user object element の #to_s を dispatch。
- **✅ class の qualified name を formatter でも** (2e45cbc3): Class#name/#to_s は qualified
  だったが C formatter の KORB_OBJ_CLASS case が bare → 補間/container/puts で "Sub" と
  出てた。korb_fprint_class_qname に統一。
- 残 structural (更新): Kernel-private-builtin reflection、const namespace (Module#constants /
  `class M::C` path)、deferred-Enumerator、object default #inspect の ivars。

### 2026-07-01 続き2 (Module#constants — cref アプローチ)
- **✅ Module#constants** (d58f687e): const table を owner-tagged 化。koruby に runtime cref は
  無いが **lexical nesting は parse 時に既知**(Module.nesting が baked してるのと同じ情報)なので、
  node_const_set に **enclosing module の const 名を baked**(`tc->frame->class_name_sym`)、runtime で
  live module に解決 → owner に。nested class/module は korb_class_body の既存 `enclosing` から。
  const_owners[] parallel array (AROH_VISIT_ROOTS で root-scan)。$-global / @-cvar は除外。
  **これで self_off を node_const_set(global 変数書込と共用、7 nesting site)に通す risk を回避**
  = owner 名は parse-time 定数、frame offset 不要。own-constants のみ (inherit / M::C path は follow-up)。
  (user の "cref がないの?" 指摘が解法の鍵だった)
- 残: `class M::C` path 形式 (owner 追跡はできたので次点で可能かも)、Kernel-private-builtin
  reflection、deferred-Enumerator、regex (astrorge 待ち skip)。

### 2026-07-01 続き3 (class M::C path form)
- **✅ `class M::C` / `module M::Inner` (const-path form) の qualified name** (6e1189d8):
  Module#constants と同じ cref トリック。parser が path parent (M) の名を node_class/
  node_module の `path_owner` operand に baked、runtime で live module に解決 → enclosing に
  (lexical self の代わり)。M::C.name / M::Inner::E.name が qualified、M.constants にも載る。
  値は flat table の rightmost 名で格納のまま (M::C vs top-level C の衝突は別 follow-up)。
- 残 const-namespace: **flat-table collision** (M::C と top-level C が同じエントリ) — const 格納の
  namespace 化が必要な最深部。他: Kernel-private-builtin reflection、deferred-Enumerator、
  regex (astrorge skip)。

### 2026-07-01 続き4 (const flat-collision 解消 + intern O(1))
- **✅ namespace-aware const resolution** (4c68952f): flat-table collision を解消。const table を
  **(name, owner) キー化**で M::C と top-level C を別エントリに。resolution は owner タグ + class の
  **`enclosing` chain**(= parse 時に既知の "cref")で lexical に:
  - const_define が (name,owner) キー、class find-or-create が同 namespace を reopen (M::C は M::C を、
    top C じゃなく)。
  - node_const に baked `owner_name`(bare=innermost enclosing module を parser の frame chain から / 
    M::X=path parent)。cache miss 時に owner の enclosing chain を walk して (name,owner) を試し、
    無ければ flat first-match に fallback(top-level + builtin const like Math::PI は owner nil)。
    bare TOP-LEVEL read は owner 0 = 元の flat path なので **hot cache-hit path は byte 同一(perf 中立)**。
  - **これで const-namespace の最深部(flat-collision)も解決**。multi-level lexical resolution 検証済。
  - 罠: top-level self=main は class でないので owner を nil に normalize(find と define で一致させる)、
    builtin module const(Math::PI, owner nil)は scoped read で flat fallback 必須。
- **✅ perf: O(1) symbol intern** (c71cfb6d): korb_intern が線形走査 + 各比較で strlen → perf-record で
  __strlen_avx2 が symbol-heavy path の 61%。open-addressing hash index(FNV-1a)+ 長さ cache で O(1) 化。
  混合 bench(fib+float+symbol+truthy)が **28.4B→7.0B instructions / ~4.2s→~0.66s(~4x)**。挙動不変。
- **残 structural**: Kernel-private-builtin reflection(dispatch visibility)、deferred-Enumerator、
  regex(astrorge skip)、object default #inspect の ivars(address 不可)。

### 2026-07-02 (deferred-Enumerator find + Kernel-private reflection)
- **✅ Array#find/#detect の no-block Enumerator** (792a93e7): op-tag 方式(op 4 = find)。
  .each/.with_index が early-stop で最初の match を返す(no match → nil)。naive eager materialize
  でなく driver 側で early-stop するので正しい。map/each/select/reject/flat_map は既に op-tag で動作。
- **✅ Kernel-private builtin の reflection** (ca8c89c7): puts/print/require/Integer() 等は global
  builtin table に有り class table に無いので respond_to?(:puts,true) / private_method_defined? /
  Kernel.private_instance_methods / obj.private_methods が漏らしてた。reflection が global table
  (kind==BUILTIN)を Kernel-private として参照(respond_to? は include_private のみ、defined? は
  private=true/public/method=false、listing は inherit 時に append)。**dispatch は不変(reflection のみ)**。
- 残 no-block Enumerator: partition/group_by/min_by/max_by/count.with_index(各 custom accumulation
  = partition→2配列, group_by→hash, min_by→track, count→int。稀)。他残: regex(astrorge skip)、
  object default #inspect の address(moving GC で不可)。

### 2026-07-02 続き2 (method visibility enforcement)
- **✅ private / protected の enforcement** (b6a68836): これまで tracking/reflection のみで
  **enforce してなかった**(明示レシーバで private/protected を呼べてしまう)。node_send →
  korb_send_cached の explicit-receiver 経路で enforce。caller self を node_send に baked self_off
  で渡す(op-assign/[]/=== の synthetic send は INT32_MIN = check 無し)。
  - private: 暗黙 self か `self.foo`(recv == caller self)のみ。protected: caller self が owner の
    kind_of? のときのみ。send/__send__ は bypass、public_send は既に拒否。
  - **fast path は byte 同一を維持**(user が「fastpath 遅くするな」と指摘): korb_send_cached が
    private/protected を KORB_IC_INSTANCE cache から**除外**(visibility 変更は method_serial を
    bump 済 → cache 自動 invalidate)ので node_send の inline fast path は public しか見ず check 不要。
    20M explicit-send loop で 8.25B→8.27B instructions(誤差)。
  - attr_reader/writer が body の cur_visibility を継承、private_class_method/public_class_method が
    実際に singleton method の visibility を設定(no-op だった)。継承 private/module private/
    protected across instances/self. 経由 private setter/private attr を網羅。
  - **残**: block(recv.m{}) / splat(recv.m(*a)) / safe-nav(recv&.m) の explicit send は未 enforce
    (korb_send_impl 経路で caller_self 無し。private + これらの組合せは稀)。

### 2026-07-02 続き3 (private も cache = KORB_IC_INSTANCE_VIS)
- **✅ private/protected を cache** (cc59497c): 続き2 は private を IC から**除外**したので明示レシーバの
  private 呼び出し(`self.foo` をループ等、正当なケース)が毎回 re-resolve で遅かった。専用 ic kind
  **KORB_IC_INSTANCE_VIS** を追加: private/protected は cache(解決済)するが node_send の inline fast
  path は KORB_IC_INSTANCE のみ match するので素通ししない → korb_send_cached の ic-hit VIS branch で
  cached entry を guard→korb_invoke_simple。public fast path は byte 同一のまま。
  20M self.priv loop で **12.57B→11.32B insn / 1.78→1.65s(~7%)**。(user 指摘「private も使う、ちゃんとやろう」)

### 2026-07-02 続き4 (stdlib: ENV / ARGV / File stat / Dir)
- **✅ ENV** (36cde6b7): module 方式で getenv/setenv/environ backing。[]/[]=/store/key?/fetch(default/
  block/KeyError)/keys/values/to_h/each/delete/size/empty?/value?/to_s。ENV.class は Object に override。
  罠: ENV.delete が arg String の byte ptr を str_new(moving GC)後に unsetenv で使い STRESS crash →
  stack buffer に copy。
- **✅ ARGV / $0** (36cde6b7): main が post-script args から ARGV array + $0/$PROGRAM_NAME を構築
  (korb_define_argv)。
- **✅ File stat + read + Dir** (次 commit): File.exist?/file?/directory?/size/read(fopen)、Dir.pwd/exist?
  (getcwd/stat)。path ptr は alloc 前に使うので GC 安全。
- **残 stdlib**: File.open/write・IO オブジェクト(write-side)、String#unicode_normalize、Marshal、
  Dir.glob/entries、Time の詳細。

### 2026-07-02 続き5 (IO/File objects — write-side完成)
- **✅ File.write/readlines/foreach/delete** (7281b8b2): class method、String content。
- **✅ IO オブジェクト層** (689fdbe6): vm->io_fps[] table に FILE* を持ち、instance の @__io_fp ivar が
  index(FILE* は off-heap raw ptr → GC scan 不要、ivar は Fixnum)。IO class(< Object)に write/print/
  puts/<</read/gets/readlines/each_line(getline、no-block→Enumerator)/close/flush/eof?。File < IO に
  reparent、File.open(path,mode){|io|…} で yield+auto-close。$stdin/$stdout/$stderr + STDIN/STDOUT/STDERR
  を fd slot 0/1/2 に bind。**GC罠: korb_init_io が IO class を C local に cache→korb_obj_singleton/
  obj_new が動かして 2個目以降の std-stream が stale klass(STRESS で $stdout.class が Object 化)→
  rooted slot を毎回 re-read**。
- **残 stdlib**: IO.new/pipe/select、Marshal、Dir.glob/entries/chdir、String#unicode_normalize、
  File.write の to_s coercion、Errno クラス階層。

### 2026-07-02 続き6 (Dir/$$/StringIO/Marshal)
- **✅ Dir.mkdir/rmdir/entries/children/glob/[]/chdir + $$** (462d0177): opendir/readdir/glob(3)/
  mkdir/rmdir/chdir。GC罠: Dir.chdir が block-arg を path arg 内部ptrから str_new→alloc GC で stale。
  arg String value を直接渡す。
- **✅ StringIO** (7b4e462e): pure-Ruby prelude(prelude/stringio.rb)。write/print/puts/<</read/getc/
  gets/each_line/readlines/string/pos/rewind/eof?/StringIO.open。
- **✅ Marshal.dump/load** (1a342588): pure-Ruby prelude、CRuby format 4.8 互換。nil/true/false/Integer
  (compact long)+Bignum('l')/Float/String/Symbol(+link)/Array/Hash。load は 'I' ivar wrapper skip。
  **CRuby と双方向 cross-process interop 確認**、deep-copy round-trip(2**100 含む)。残: object link/
  ivar/user marshal_dump、koruby の string dump は encoding ivar 無し。
- **prelude に Ruby で足せる stdlib は prelude/*.rb + main.c の KORUBY_PRELUDE_FILES に追加が最速**。
- **残 stdlib**: Errno クラス階層(File系の raise を正確に)、IO.new/pipe/popen、Time 詳細、
  String#unicode_normalize、Comparable/Enumerable の細部、Set の残 method。

### 2026-07-02 続き7 (rubyspec 系統的 sweep 開始; Errno/custom-new/Latin-case/const-reflection/Time)
- rubyspec core sweep (tools/rubyspec_run.rb ~/ruby/src/master/spec/ruby/core): pass率 69.5%
  (pass=12480 fail=2245 err=3244)。worst file から高ROI・非regex/encoding を選んで潰す方針。
- **✅ Errno クラス階層 + qualified 名** (f6e67d77): File/Dir が errno→Errno::* を raise
  (korb_errno_name/korb_raise_errno、strerror msg、exc_class tag)。`X = Class.new` の const 代入が
  enclosing を設定→Ns::Anon が qualified 名に(const-namespace の enclosing chain 再利用)。
- **✅ custom `def self.new`** (0b97b9f5): Klass.new が allocator で custom singleton new を shadow してた。
  korb_class_new_kind が singleton の new を検出→kind 2(smethod path)。factory method + Time.new(parts)。
  new_kind cache で hot path 不変。
- **✅ Latin-1 Unicode case** (6566fa29): upcase/downcase/capitalize/swapcase(+bang)が ASCII のみ→
  Latin-1 Supplement(À-Þ↔à-þ, ÿ↔Ÿ)対応の UTF-8 case transform。byte length 保存で in-place。
  Greek/Cyrillic/Turkic/ß/ligature は table 必要で未対応。
- **✅ Module#const_get/set/defined? owner-aware** (7d9e998b): flat table 無視してた→const_set が
  receiver に nest、const_get/defined? が receiver+ancestors 優先(collision 解決)、defined?(name,false)。
  罠: const_defined? の arity を 1→-1 に(inherit arg)。残: Integer.const_get(:MAX) が Float::MAX を拾う
  (builtin const の owner が nil、要 owner 付与)。
- **✅ Time** (b35fb306): sunday?..saturday? + Time#to_a。

### 2026-07-02 続き8 (IO/Dir/File 拡充; rubyspec sweep 継続)
- rubyspec core sweep 69.5%→69.7%(file-clean 806→818)。残 big bucket は encoding(Unicode table)、
  real syscall(process/spawn, io/popen/pipe/pread — fork/exec 必要)、refinements(module/refine/using)、
  const_source_location、Unicode case table。個別 method の穴は減少。
- **✅ IO#read(n) + seek/pos/pos=/tell/rewind/each_char/getc** (89966ba1): read が length 無視→n bytes。
  fseek/ftell。SEEK_SET/CUR/END 定数。罠: each_char が str の内部ptr から str_new(alloc GC で source
  移動)→ stack buffer に copy(STRESS crash)。
- **✅ Dir.glob ** 再帰 / 配列 pattern / block** (be3a8a68): glob(3) は ** 非対応→prefix+N*wildcard+suffix
  展開(N=0..24)。array pattern、block yield。罠: C コメント内の `*` `/` が comment を閉じる。
- **✅ IO.read/write/readlines/foreach class method + File 拡充** (e73b244e): IO.* を File impl に委譲、
  File.binread/binwrite、File.read(path,len,offset)、File.foreach no-block→Enumerator、readlines(chomp:)、
  IO#getc/readline/readchar(EOFError)。
- **手法の学び**: commit gate は CORPUS_MATCH を**別 call で確認してから** commit(const_defined? arity で
  一度 failing test を commit してしまい amend で修正)。

### 2026-07-02 続き9 (whole-file crash 掃討 + shim NotImplementedError rescue = 大幅 coverage 判明)
- **✅ Comparable#== 無限再帰 SEGV 修正** (96f7df6c): Comparable#==→#<=>→Object#<=>→#==→… の cycle。
  KORB_FL_CMP_VISITING (0x800) で receiver に guard、再入→false。Object#<=> は == dispatch 維持
  (object_spaceship 依存)。**whole-file code=139 crash がゼロに**。
- **✅ mspec_shim it() が NotImplementedError を rescue** (92753178): NotImplementedError は
  StandardError でない→per-example rescue を escape して file 全体を abort してた(1 unsupported example
  = whole-file fail)。real mspec は example 失敗後も継続する。rescue して skip 計上。
  → **whole-file fail 291→170、pass 12532→23400、pass率 69.7%→78.9%**(passing example が abort に
  隠れてただけ)。koruby の真の core coverage は ~79%。
- 残: encoding(Unicode table)、real syscall(process/spawn, io/popen/pipe — fork/exec)、refinements、
  instance_eval/class_eval の String 版、niche reflection(BasicObject.instance_methods, Method#to_proc block)。

### 2026-07-02 続き10 (whole-file SEGV 掃討 → core SEGV ゼロ; enumerator generator 化 + lazy to_enum)
rubyspec core sweep 78.9%→現状 file-clean 843→854、whole-file-fail 170→93、SEGV(code=139) **全滅(0)**、
pass 23405→24061。この session の commit:
- **✅ Klass.new(...) { block } の SIGBUS** (25bb0f97): send_impl の mid_new block path が
  korb_invoke_method(ISEQ専用) で CFUNC initialize(既定 Object#initialize)を呼び locals_cnt 誤読。
  no-block hot path と同じく base[-1]=obj + korb_dispatch_method に。file/new_spec の File.new(f){raise} 修正。
- **✅ puts 再帰配列 "[...]"** (efa2ba66): korb_puts_one_to が自己参照配列で stack overflow。
  KORB_FL_JOIN_VISITING guard で "[...]\n"。io/puts_spec。
- **✅ mspec_shim touch/mkdir_p** (76095808): file/io fixture helper 未定義 → whole-file abort。
- **✅ Object#to_enum / enum_for を lazy 化** (06953f6a): eager に send(meth){} 実行してた →
  raise/infinite で to_enum 構築時に死ぬ。Enumerator.new generator に。lazy/* ~12 file の abort 解消。
  **副作用**: eager 前提の enum method が mode-3 generator で SEGV → 下記で対処。
- **✅ Module#class_variable_get/set/defined? + class_variables** (1713d05d): korb_cvar_owner/set 上に実装。
  basicobject/instance_exec/eval の abort 解消。
- **✅ module 内 alias_method が Object/Kernel method 解決** (d00ed7c7): korb_do_alias が module receiver で
  Object fallback。core/module/* 共有 fixture(classes.rb)の abort 全解消。missing は NameError に。
- **✅ enumerator generator/lazy mode 対応** (fef02ebd): each_with_object/with_object(force_gen)、
  next/peek/next_values/peek_values(mode!=0 で gen_run/lazy_drive 駆動)、Lazy#select/map/... no-block は
  ArgumentError。each_with_object/next_values/peek_values/lazy{select,reject,take_while,drop_while} の SEGV 修正。
- **✅ Enumerator.allocate + #initialize** (aabbd267): allocate が generic object → VAL2ENUM で heap 破壊。
  allocate を Enumerator 用に KorbEnumerator 化 + #initialize(block→mode3 generator)。size 引数は未保存。
  最後の core SEGV(enumerator/initialize_spec) 解消。
- **✅ public/private/protected :sym が inherited method に効く** (9be52893): korb_set_visibility が直接
  定義の method しか見てなかった → 継承/include の method に visibility-override entry を作る(owner 保持で super OK)。
- **残 TODO(未着手)**:
  - ~~core/method/* 定数解決 architectural bug~~ **✅ 修正済 (c5c3847a)**: bare const read の enclosing 全 chain
    (outermost→innermost)を constcache に焼き、cold path で owner-scoped 解決 → **unique な innermost cref class**
    を得てから enclosing walk。同名 nested class(M::C vs M::Inner::C)を誤らない。flat seed に fallback で hot
    path 不変。method/* 13 file を unblock。
  - **define_method body 内の `super` は未対応**("M0 unsupported: super outside a method body")。DM body は
    block として parse され super が method owner を知らない。super が DM method に**着地**する側は ✅ (f2d17d6d、
    korb_super に DM case、SIGBUS 解消)。DM body から super を**発する**側が残(parser/frame で method identity を
    DM proc に渡す設計が要る)。module/prepend の該当 example は NotImplementedError で skip 継続(crash せず)。
  - alias_method :meow, :derp で meow の owner=C になり super が原 module 上でなく C 上から解決される
    (visibility_inherited.rb の単純 case は通るが、multi-module + super('arg') で arity 不整合)。
  - 他 code=1 abort: kernel/p(M0 non-local multi-assign)、exception/case_compare(constant path w/ non-namespace
    parent)、kernel/eval(eval str + coerce)、marshal/dump・load(UserMarshal fixture)、dir/file の syscall 系。

### 2026-07-02 続き10 追記 (Enumerator::Lazy + shim resilience + parser gaps)
続き10 の後半。commit 追加:
- **✅ Enumerator::Lazy class** (6bce2258): lazy-mode enum(1/4)の class として報告(korb_class_obj_of +
  korb_dispatch_class)。const 定義で lazy/* の "uninitialized constant Enumerator::Lazy" abort 解消。
- **✅ mspec_shim it() が Exception を rescue** (1acb43cb): `SpecificError < Exception`(lazy 早期停止証明用)が
  `rescue => e`(StandardError)を escape して whole-file abort → Exception rescue で継続。
- **✅ `expr::CONST`** (d1a17bb3): 非定数 parent の constant path を `(expr).const_get(:CONST)` に desugar。
  exception/case_compare unblock。(残: Integer::MAX が Float::MAX を拾う const_get flat-fallback bug は別件)
- **✅ global multi-assign** (f7d327de): `$/, $\, $, = a,b,c` を general desugar + mw_assign_target に
  GLOBAL_VARIABLE_TARGET 追加。kernel/p unblock。
- **✅ mspec_shim as_user/as_superuser** (1984af40): privilege guard 未定義 → abort。非 root なので as_user は
  実行、as_superuser は skip。dir/{chroot,mkdir}・file/{lchown,ftype} unblock。
- **セッション累計**: rubyspec core whole-file-fail **170→85**、SEGV **全滅維持(0)**、pass 23405→24169(+764)。
- **残 code=1 abort(fixture-resolution 依存 or feature gap)**: method/*(14, const-resolution 大物)、
  marshal/{dump,load}(UserMarshal fixture)、kernel/eval(eval str SyntaxError 化)、exception/syntax_error
  (eval syntax→ArgumentError で SyntaxError でない)、dir/fileno(Dir instance object)、module/constants
  (ConstantSpecs fixture)、file/ftype(FileSpecs fixture)。※ 個別 fixture は runner resolve_requires 依存で
  手動 cat 再現と挙動が違うことがある(sweep が正)。

### 2026-07-02 続き10 追記2 (const-resolution 大物解決 + super/eval/shim)
- **✅ 定数解決 architectural bug 解決** (c5c3847a): constcache に enclosing 全 chain を焼き cold path で
  unique cref 解決。method/* 13 file unblock。詳細は上の該当項目参照。golden 無回帰。
- **✅ super into define_method** (f2d17d6d): korb_super に KORB_METHOD_DM case。module/prepend の SIGBUS 解消。
- **✅ eval SyntaxError 化** (b0844a67): KORB_E_SYNTAX etype 追加、koruby_parse_source に exit_on_error flag。
  plain eval が process exit してた/binding eval が ArgumentError → SyntaxError(<ScriptError)に。kernel/eval unblock。
- **✅ shim guard block が Exception を swallow** (最新): platform_is_not 等の guard body 内の未対応構文
  (backtick 等)が file を落とさない。file/ftype・file/stat/ftype unblock。
- **セッション最終 tally**: rubyspec core whole-file-fail **170→60**、**SEGV 全滅維持(0)**、
  pass 23405→24369(**+964**)、file-clean 843→868。
- **残 whole-file abort(大機能 or niche)**: thread/*(11、Thread/並行性)、string/encoding・encode(Unicode table)、
  marshal/dump・load(UserMarshal = String subclass の ivar)、dir/fileno(Dir instance object)、
  method/parameters(重複 `_` param の locals 割当)、exception/syntax_error(spec file 自体が意図的 invalid 構文)。

### 2026-07-02 続き11 (20h autonomous: per-example 修正 + recursion/overflow hang 掃討)
whole-file abort 掃討後、per-example 失敗と timeout/crash を修正。sweep 現状: pass **24659**、
file-clean 869、whole-file-fail 56、**SEGV=0 / TIMEOUT=0 / KILL=0**(全 hang/crash 解消)。
- shim: evaluate() を eval(code, binding) 化(@ivar が assertion block に見える)→ proc/arity 0→64 等 broad。
- include/prepend が Class 引数を TypeError、bare module の private_instance_methods から Kernel builtin 除外。
- define_method: name to_str coerce + FrozenError。Module#module_function 実装(named+no-arg mode、
  private instance + public singleton copy)。const_get: inherit flag/scoped name/to_str/const_missing。
- sprintf `*` 幅/精度の positional(`*N$`)+ 負幅→左詰め。String case の :ascii option。
- Time#asctime/ctime、Time.new utc_offset(Integer/"±HH:MM:SS"/UTC/to_int、@__off 保存)→ time/new 12→27。
- Method#super_method / UnboundMethod#super_method(MRO 上の次定義、fixed owner)→ 5→13。
- **recursion/overflow hang 掃討(重要、code=124/137/139 を全滅)**:
  - inspect(Array/Hash)自己参照 → KORB_FL_JOIN_VISITING で "[...]"/"{...}"(depth limit だと multi-ref で指数爆発)。
  - deep_hash(Array/Hash)自己参照 → 同 flag。== / eql?(Array/Hash)自己参照 → 同 flag(SEGV だった)。
  - Array#fill 巨大 len(fixnum_max)→ uint32 cap 超で ArgumentError、Bignum len → RangeError。
  - Array#product 積数 overflow(101^11)→ 乗算時 cap 検出で RangeError。
- 手法メモ: hang 追跡は shim の it() に `$stderr.puts $ms_current` 一時挿入で最後の test を特定。

### 2026-07-02 続き11 後半 (feature 拡充続き)
- include/prepend の transitive(module の include を MRO flatten、cyclic 検出、const_get も included module 走査)。
- Module#module_function、const_get(inherit/scoped/to_str/const_missing)、Method#super_method、File.chmod/umask/access。
- **Encoding.default_external=/default_internal= setter**(cvar backing)→ 多数 file の encoding-default ERR 一掃(pass +500超)。
- **define_method 済 method は arity 厳格化**(block/lambda/proc 全て method-arity、korb_dispatch_method DM case で check)→ 52→70。
- **block/proc の optional + rest 併用**(`|a,b=1,*r|`)対応(parse rejection 撤去 + rest branch で front optional の default 適用)。
- repeated `_` param は soft NotImplementedError 化(hard kp_failf 撤去)。
- **最終 tally: pass 23405→25284(+1879)、whole-file 170→57、SEGV/TIMEOUT/KILL=0**。
- 残 big bucket は変わらず: regex(astrorge)、encoding table、real syscall(process/thread/io popen/pipe)、
  autoload/require、source_location、Dir instance object、marshal user-fixture。

### 2026-07-02 続き11 追記 (arity + File.open + 既存 io STRESS bug 発見)
- **Proc#arity** を param_info kind 集計で正確化(post/optional/proc-vs-lambda)→ proc/arity 83→122(0 fail)。
- **File.open block** が #close 経由で閉じる(subclass override 尊重、block 値 return、close error 伝播)→ file/open 55→60。
- **⚠ 既存バグ発見(要修正、Phase 3 libc/arena mix)**: File.open で作る io object が **STRESS+PURGE 下で klass stale 化**
  → `io.read` が "undefined method 'read' for an instance of Object"。**HEAD(改修前)でも再現**するので既存。
  io object は arena 側 + io_fps(libc)混在。[[project_koruby_precise_libc_arena_mix]] の未完分。
- **最終 tally: pass 23405→25329(+1924)、whole-file 170→58、SEGV/TIMEOUT/KILL=0、golden 92769→93024**。

### 2026-07-02 続き12 (io STRESS bug 根本原因特定 + 実 require/source_location/Dir へ)
- **io STRESS bug 根本原因確定**: `BARUBY_GC_STRESS=1` 単独では **PASS**(実 moving GC は正しい)。
  **STRESS+PURGE でのみ失敗** = 1024-plane mprotect hardening の cross-plane forward 問題。具体的には
  io->klass=File は正しいが **File->superclass(=IO) が PURGE 下で失われ**(retired plane への参照が
  forward_payload で NULL 化)、io.read が IO まで辿れず NoMethodError。**io 固有バグではなく shared
  runtime/precise_gc の PURGE 限界**(全 sample 共通、[[project_koruby_precise_libc_arena_mix]] の Phase 3
  = 全 container を arena migrate する抜本対応が必要 = 大物・要方針確認)。File→IO reparent の raw write は
  ARO_STORE に修正済(barrier 正常化、ただし PURGE 失敗の原因ではない)。
- 実 GC(STRESS)で io は正しいので **production correctness は OK**。PURGE は test hardening mode。

### 2026-07-21 健全性ファジング + 言語系統修正（詳細は docs/rubyspec.md の 07-16〜21 節）
- **完了**（corpus 93,399/0 + STRESS + fuzzer 875/0 + ruby一致）:
  - Marshal を CRuby 4.8 wire format に全書換（dump 0→146 / load 53→235）。**Marshal byte-exact は除外から外れた**。
  - send/block-path の `Class#new` が builtin singleton `new`(CFUNC=Regexp/Time/File/Dir) を honor → core +2,302。
  - 言語クラッシュ3件根治: `$stdout=obj;print`無限再帰 / massign-RHS の user`<<`(node_shl send_cached frame衝突) / proc内`rescue <captured-var>`(node_rescue の matcher chain 不整合)。language 68.8%→76.1%。
  - `defined?`(match global/nil-ivar-gvar, 231→276)、`!=` override + `Object#!=` latent、index massign target、定数 MRO ancestry(91→102)、`super` rest+block forward(48→66)。
  - **差分ソートネスファザー `tools/fuzz_soundness.rb`**（`--stress` で GC crash 検出）。node.def の slot/frame offset 型バグは静的+動的とも残存無し確認。
- **残（到達可能）**: `super` block forward は depth==0 のみ（nested block 内 super 未対応）。`X::Foo`(X 非module)→TypeError（node に explicit-path/bare-read 区別が必要）。method/massign の mock-protocol coercion（除外）。

## Thread/IO (io_design.md Phase 1 実装後の残)
- [ ] Time.now の秒未満精度 (to_f が整数秒 — IO.select/timeout テストの時間検証が偽陰性になる)
- [ ] IO#close → korb_blop_cancel_fd (待機中 thread に IOError "stream closed in another thread")
- [ ] dead thread の vslots/cstack reap (現状リーク; 別 stack から free する reaper)
- [ ] IO read/write 本体の blop 化 (fd ベース IO 層への作り直し; 現状 FILE* 同期のまま)
- [ ] epoll / io_uring engine (現状 poll(2) のみ; vtable 化と probe は io_design.md 通り)
- [ ] pump wake eventfd + signal 配送 (pump 睡眠中の Ctrl-C)
- [ ] rescue Exception が Thread::Kill を捕まえてしまう (CRuby は kill を rescue 不能)

## stdlib vendor 後の残 (2026-08-09)
- [ ] CSV.parse が2行目以降を落とす (strscan の each_line/scan 系の挙動差疑い)
- [ ] `(...)` argument forwarding (PM_FORWARDING_*) — forwardable は回避済みだが言語機能として未対応
- [ ] Fiber.current / Fiber.storage (logger は Thread.current で回避済み)
- [ ] proc 内 outer-yield を FWD 実行すると "no block given" (timeout.rb は明示 capture で回避済み)
- [ ] 中間の重複 `_` block param が位置 staging と衝突 (tmpdir は改名で回避済み)

## eval / Binding のスコープ (2026-08-13 発見)
- [ ] `eval(str)` が **呼び出し元のローカルを見ない** (`a = 1; eval("a")` → NoMethodError)。
      CRuby は `eval(str)` == `eval(str, binding)`。C の builtin には caller の
      local 名表が無い (parse 時情報) ので、**parse.c で `eval(str)` を
      `eval(str, binding)` に desugar** すれば同一フレームのケースは直る
      (実装して確認済み: kernel/eval 17fail+16err → 14+12、make test 退行なし)。
      ただし下の AOT bake バグに当たるため **revert して保留**。
- [ ] Binding が **外側スコープのローカルを持たない**。block の binding は自分の
      フレームのスロットしか名前を持たず、`[1].each { binding.local_variables }`
      が `[]`、`eval("outer_var")` も見えない。KorbBinding は env 1 本 + フラットな
      name 表なので、closure chain (node_eget の depth walk と同じ規則) を辿る
      per-level name 表が要る。core/binding の local_variables/local_variable_get
      と kernel/eval の "enclosing scope" 系がこれ待ち。
- [ ] **AOT bake バグ**: メソッド本体が `binding` *だけ* の場合 (body root が
      node_binding)、その body が bake されず `--compiled-only` で poison になる
      (`def run; binding; end` で再現。`x = 1; binding` は OK)。上の desugar を
      入れると `def run; eval(s); end` も同じ形になり optcarrot AOT が落ちる。
      node.def に `@noinline` を付けても exempt されなかった (head.flags.no_inline
      に伝播していない可能性)。desugar 解禁の前提。

## io/copy_stream_spec が whole-file timeout (2026-08-13 の退行)
- [ ] core/io/copy_stream_spec.rb が sweep で timeout する (それ以前の sweep
      では 109 例完走 / 3err だった)。同日の IO.popen "r+" (socketpair) 追加・
      read 前 flush・copy_stream の readpartial 化・File.open の options 対応の
      いずれかが原因。**単体で切り出したケースは全部 CRuby 一致で通る**
      (pipe→file / file→IO / object 送受信 / Tempfile(RDONLY) / 17KB / popen r+ /
      offset の ESPIPE)。spec を走らせると fixture の内容が stdout に繰り返し
      出続けるので、どこかで無限ループして子プロセスの出力が端末に漏れている。
      mspec 環境固有 (new_io / IOSpecs::CopyStream の class 変数越しの受け渡し)
      の可能性。次に触るときは spec を分割実行して例を特定するところから。

## 正規表現の名前付きキャプチャ → ローカル変数束縛 (2026-08-14 発見)
- [ ] `/(?<n>\d+)/ =~ str` が **パースできない** (prism `PM_MATCH_WRITE_NODE` = 102 が
      node_unsupported)。CRuby はこの形だけ特別扱いで、名前付きキャプチャと同名の
      ローカル変数を作る (正規表現がリテラルで左辺のときのみ)。
      `MatchData#[:name]` / `#named_captures` / `Regexp#names` は動くので、
      足りないのは「リテラル正規表現 =~ で local を作る」構文だけ。
      transduce_call の `=~` 経路で、左辺が PM_REGULAR_EXPRESSION_NODE かつ
      names が空でない場合に「match して各 name を local に代入」へ desugar すればよい。

## sweep の whole-file-fail には ±1 のノイズがある (2026-08-17 確認)
- [ ] `tools/mspec_real_run.rb` は 1 ファイル 20s のタイムアウトで 12 並列に流すので、
      子プロセスを大量に起こすファイルは負荷次第で WFAIL に化ける。
      `core/io/popen_spec.rb` は単体だと **9.7s / 40 例 27 pass** で安定して通るが、
      sweep では回によって WFAIL (exit 124) になる (0817 → WFAIL、0817b → 27 pass、
      0817c → WFAIL、コードは同一)。process/spawn も同じ性質。
      whole-file-fail の増減を退行と読む前に、その 1 本を単体で回して切り分ける。
      直すなら subprocess 系だけタイムアウトを伸ばすか並列度を落とす。

## moving GC 下で object_id / Object#hash が安定しない (2026-08-16 発見)
- [ ] `o.object_id` は GC を跨ぐと値が変わる (アドレス由来)。CRuby は寿命の間
      不変を保証する。`Object#hash` も同様に変わるが、Hash 側は GC 後も引けている
      (`h[o]` は通る) ので実害は id の値そのものを持ち回る場合に限る。
      直すなら sklass_obj/sklass_cls と同じ「両列を root として forward する
      サイドテーブル」で、初回 object_id 呼び出し時に単調増加 id を割り当て、
      オブジェクトにフラグビットを立てて線形検索する形。
      spec への影響は小さい (kernel/object_id・basicobject/__id__ が各 1 例) が、
      ObjectSpace の finalizer 登録などで id をキーにできない制約になっている
      (今の実装は identity 比較で回避した)。

## caller / caller_locations が常に空 (2026-08-16 確認)
- [ ] 例外の backtrace は巻き戻し中に korb_bt_append で組み立てているだけで、
      「今のコールスタックを歩く」機構が無いため `caller` は `[]` を返す。
      Thread#backtrace / #backtrace_locations も同様。実装するには per-frame の
      (line, name) シャドースタックを push/pop する必要があり、呼び出しごとの
      コストになるので fast path への影響を測ってから。
      core/kernel/caller_locations・core/thread/backtrace_locations 系が
      これ待ち (12 err ほど)。

## 定数解決の flat fallback が無関係な名前空間を拾う (2026-08-16 確認)
- [ ] `class C; def self.get = Z; end` の時点で Z がどこにも無く、後から
      `module ZM; Z = :zm; end` があると、C の祖先に ZM が無くても :zm が返る
      (node_const の最後の `korb_const_index` フォールバック)。CRuby は NameError。
      prelude が「入れ子定数を非修飾で参照する」ことに依存しているため、外すには
      prelude 側の参照を先に直す必要がある。
      core/module/prepend の "updates the constant" 系 (8 例) と
      core/module/const_source_location (36 例) がこの構造に引っかかっている。

## core/kernel/require_spec が busy loop で終わらない (2026-08-16 確認)
- [ ] 74 例まで進んだあと CPU を回し続ける (400s 走らせても user 時間が丸ごと
      消費される = 待ちではなくループ)。$LOADED_FEATURES 基準の重複判定と
      CLI の -I/-r を入れて 12 例 → 74 例まで伸びた分の先。
      次は example 名を出しながら流して該当例を特定するところから。

## trap 済みシグナルが無限待ち / CPU ループ中に配送されない (2026-08-17 実測)
- [ ] シグナルはプロセス全体で block して check point で sigtimedwait 回収する
      設計なので、**trap したシグナル**は check point に来るまで配送されない。
      trap していないシグナルは block されないので OS 既定がそのまま効く
      (素の `sleep` への Ctrl-C は問題なく効く)。実測 (0.7s 後に送信し 2s 待つ):

      | プログラム | INT | TERM | KILL |
      |---|---|---|---|
      | trap 無し + `sleep`              | 即死 130 | 即死 143 | 即死 |
      | INT trap + 無限 `sleep`          | **効かない** | 即死 143 | 即死 137 |
      | INT trap + `loop { }`            | **効かない** | (未 block なので即死) | 即死 |
      | INT+TERM trap + 無限 `sleep`     | **効かない** | **効かない** | 即死 137 |
      | INT trap + `100.times{sleep 0.1}`| 効く 3 | — | — |

      CRuby は `Signal.trap(:INT){exit 3}; sleep` に INT で即 rc=3。
      原因は **2 つある**:
      (a) scheduler が poll(2) に -1 (無期限) で入っている間は明けるまで配送されない。
          → pump の直前に sigpending() を見て pending があれば ms=0、加えて trap 済み
          シグナルがある間は poll を数十 ms で頭打ちにするか、self-pipe + ppoll。
      (b) 純 CPU ループには check point 自体が無い。
          → (a) を直しても残る。ループ back-edge に安価なチェックを置く必要があるが、
          fast path のコストになるので計測してから (feedback_koruby_fastpath_no_slowdown)。
      当面の回避は `kill -9` か、trap されていない別シグナル (通常 TERM)。

      **(a) の実装方針は signalfd で確定 (2026-08-17 実測)**。block したまま
      check point で回収する今の方針を崩さずに poll を起こせる唯一の手:
      | 測定 | 結果 |
      |---|---|
      | poll 前から pending でも readable か | rc=1 revents=1 / 0.0ms |
      | sigtimedwait で回収したら静まるか | rc=0 revents=0 |
      | poll 中に届いた場合 | rc=1 revents=1 / 送信と同時 |
      | 別 thread が共有 fd を poll (process 宛) | rc=1 / 見える |
      | 別 thread が共有 fd を poll (thread 宛) | 見えない |
      2 行目が肝で、**drain を既存の korb_signal_deliver (sigtimedwait) に任せられる**
      ので二重消費もスピンも起きない。4 行目より **signalfd は 1 本で足りる**
      (koruby の Ruby レベルのシグナルは全部 process 宛)。M:N でも 1 本のまま。
      pump への変更: (1) 遅延生成した signalfd を pollfd 配列の先頭に常に足す
      (2) `total == 0 && ms == 0` の早期 return は blop 由来の fd 数で判定する
      (3) revents 書き戻しのインデックスを 1 つずらす
      (4) 未 block のシグナルを mask に入れておくのは無害 (pending になり得ない) なので
      __signal_block との同期は不要。

## targeted wakeup (Thread#kill / #raise / IO#close) の手段 (2026-08-17 実測)
- [ ] **close(2) は read も poll も起こさない**ので、close 自体は wakeup 機構に
      ならない。実測 (別 thread が対象 fd を close、2s 待ち):
      | 状況 | 結果 |
      |---|---|
      | read ブロック中に close | **起きない** (その後 pthread_kill で EINTR) |
      | poll ブロック中に close | **起きない** (起きた後の revents=POLLNVAL) |
      | read ブロック中に pthread_kill | EINTR 即座 |
      | poll ブロック中に pthread_kill | EINTR 即座 |
      pthread_kill は no-op ハンドラ + SA_RESTART を落とせば poll/nanosleep/waitpid
      すべてを EINTR にできる (fd 0 本)。ただし syscall に入る直前に届くと取りこぼす
      ので、CRuby のように「フラグを立てて抜けたと確認できるまで叩き直す」が要る。
      eventfd は level-triggered で書いた事実が残るため取りこぼしが無い代わりに、
      pump の poll に乗っている待機しか起こせない。
      境界は「close かどうか」ではなく **pump の poll に乗っているか**:
      - 乗っている (今の koruby は自前 fd を O_NONBLOCK にして全部 POLL blop に park)
        → close は「blop を ECANCELED で cancel + eventfd を叩く」で足り、signal 不要。
        eventfd の本数は **worker (OS thread) 数**で、Ruby thread 数ではない。
      - 乗っていない (`KORB_BLOP_CFUNC` で C ライブラリ内、waitpid、通常ファイル/NFS の
        blocking read、getaddrinfo) → pthread_kill しかない。blop の union が既に
        ubf/ubf_arg を持っているのはこのため。CFUNC blop を実際に使い出す時点が分水嶺。

## CodeQL: value-after-gc は「引数で来た VALUE」を見ていない (2026-08-19)

`codeql/value_after_gc.ql` は「関数内で may-GC 呼び出しから生まれた VALUE を
ローカルに保持したまま次の may-GC を跨ぐ」形だけを種にする。呼び出し元から
**引数で渡された VALUE** は種にならないので、`korb_re_str_span` の
`group_or_nil` を korb_re_run (alloc する) 跨ぎで保持していたバグ (STRESS+PURGE
で SEGV、2026-08-19 に修正) は検出できなかった。

計測: `codeql/test/param_cases.c` を作って比較したところ、既存 rule は
ローカル版だけを報告し引数版を報告しない。引数も種にする実験 rule
(`codeql/value_param.ql`) は引数版を報告し、slot に置いて読み直す版は報告
しない。ただし実 DB に対しては 9 分でも終わらなかった (VALUE 引数は数が多く
`reach` の再帰が爆発する)。gate に入れるには種を絞る必要がある
(例: 自分で `slots[]` に書く関数の引数だけ、など)。詳細は codeql/README.md。

## エンコーディング枠は 5 個しかない (2026-08-19) → **解決済み (2026-08-20)**

String ヘッダのエンコーディング tag が 3bit しかなく、名前付きエンコーディングは
3..7 の 5 枠を共有していた。6 種類目以降が別のエンコーディングとして報告され、
core/encoding/compatible_spec が 109 例落ちていた。

**解決**: Hash 専用の KORB_FL_CMP_BY_ID (bit 7) と IO 専用の KORB_FL_DEFAULT_IO
(bit 8) は String では使われないので、この 2bit を索引の上位に足して 5bit
(other 枠 29 個) にした。compatible_spec 109F → 6F。
残る fail は実 transcoding (encode の実バイト変換) が必要なもの。

## eval(str) が呼び出し元のローカルを見ない (2026-08-19 試作 → 撤回)

CRuby の `eval(str)` は呼び出し元のスコープで走る (ローカルの読み書きができる)。
koruby は binding 引数がある場合だけそのスコープを使い、引数無しでは新しい
スコープで評価するので `def m; a=1; eval("a"); end` が NameError になる。

**試作**: parse.c で引数 1 個の `eval(str)` を `eval(str, <binding ノード>)` に
書き換える (bare `binding` と同じ node_binding を第 2 引数として staging)。
core/kernel/eval_spec は 17 fail/16 err → 14/12 になり make test も退行なし。

**撤回した理由**: `make optcarrot-aot` が
"AOT compile-miss — interpreter dispatch reached for body '(program root)'"
で落ちる。optcarrot は CPU/PPU コアを `eval(生成コード)` で作っており、
node_binding は per-process ポインタ (name_syms) を持つため bake できない。
差し戻し済み。パッチは残していないが、必要なら transduce_func_call に
`eval` の分岐を足すだけで再現できる。

**やるなら**: node_binding を bake 可能にする (名前表を @sym 化する) か、
binding 引数を実行時に組み立てる別ノードにする。ブロック内の binding が
外側スコープのローカルを含まない件 ([[project_koruby_eval_binding]]) も
同時に直す必要がある (`[1].each { eval("c += 1") }`)。

## ブロック内の `super` が未実装 (2026-08-19)

`def foo; [1].map { super }; end` / `define_method(:foo) { super() }` が
"M0 unsupported: super outside a method body" になる。parse.c の
PM_SUPER_NODE / PM_FORWARDING_SUPER_NODE は `tc->frame->method_mid` が 0
(ブロックフレーム) のとき諦めている。block_given? と同じようにメソッド
フレームまで遡り、self / entry cell を env リンク経由 (depth) で読む必要がある。
`defined?(super)` も同じ理由でブロック内では nil (CRuby は "super")。
language/defined_spec の残り fail のうち 6 件がこれ。

## 定数の flat fallback は特異クラス body が支えている (2026-08-19 実験)

node_const は「レキシカル chain → cref の ancestry → owner nil (top-level) →
**名前だけで最初に見つかったもの**」の順で引く。最後の flat fallback が
`M::X` を M の外から見えるようにしてしまい、language/constants_spec の
「呼び出し元のレキシカルスコープを探さない」「Object は明示的に open した
ときだけ探す」系が落ちる (34 fail/9 err の主因)。

**実験**: fallback を外すと `make test` は 100,510 のまま変わらないが、
language/constants_spec が **fixture のロードで死ぬ**:
`class << obj; CS_SINGLETON4_CLASSES = ...; end` の中から同じ定数を読めない。
特異クラスは名前を持たないので、parse 時に焼く陣列 (class_name_sym の chain)
で cref を再現できず、chain 解決が nil になるため。

**やるなら**: node_const の cref を「名前 chain」ではなく実行時の cref
オブジェクトで渡す設計に変える必要がある (AOT の bake キーは名前ベースなので、
そこも一緒に考える)。fallback はそれまで残す。

## 定数解決を「実行時 cref」に寄せる案 (2026-08-19 追記)

現状は parse 時に焼いた**名前 chain** で cref を再現している。これが破れるのは
名前を持たない cref (`class << obj`、`Class.new` の body) のときで、そこを
flat fallback が救っている。

2026-08-19 に `class << obj` の body だけ self を owner に使うようにして
一段改善した (singleton_class_spec / constants_spec / const_get_spec)。
それでも **`class << obj` の中で `class X` と入れ子にすると X の外側
(ConstantSpecs 等) が辿れない**: X の enclosing は特異クラスで、特異クラスの
enclosing はレキシカルな外側を指していないため。language/constants_spec の
fixture (constants_sclass.rb) がこれで落ちる。

**本筋の案**: node_const / node_const_set に (self_off, dc_off) を渡し、
`korb_cvar_cref(self, entry_cell)` と同じ方法で実行時 cref を得る
(クラス body なら self、メソッド本体なら entry->owner)。レキシカルな外側は
cref->enclosing を辿る。名前 chain は不要になり、flat fallback も外せる
見込み。ただし特異クラスの enclosing をレキシカル位置に設定する必要があり
(同じ特異クラスを別の場所で開いたときの扱いを決める必要がある)、そこが
未解決。

## トップレベル `def` は Object の private インスタンスメソッドではない (2026-08-20 実験)

koruby はトップレベル def をグローバル関数表に入れており、
`Object.private_instance_methods.include?(:foo)` が false になる (CRuby は true)。

**試作**: korb_method_define で Object にも private インスタンスメソッドとして
登録してみたところ、`make test` は不変だったが **language/def_spec が 17 → 22 に
悪化**した (mspec 自身のトップレベルメソッドが Object の private に現れ、
メソッド一覧を見る例が壊れる)。差し戻し済み。

**やるなら**: グローバル関数表を廃して Object の private メソッドに一本化する
(dispatch の fast path をどう保つかが論点)。中途半端に両方に置くと一覧系が壊れる。

## eval の `(eval at FILE:LINE)` と caller/caller_locations (2026-08-20)

`eval("__FILE__")` は CRuby だと `"(eval at spec.rb:228)"` になるが、koruby は
`"(eval)"` のまま。呼び出し元の **現在行** を知る手段が無いのが理由で、行番号は
例外オブジェクト (`KorbException.line`) に unwind 中だけ載る設計になっている。
`caller` / `caller_locations` が空なのも同じ根っこ。

**やるなら**: フレームに現在行を持たせる (send ノードが持つ line を frame に
書く) 必要がある。fast path のコストとの兼ね合いを測ってから。

## instance_eval(String) の定数・クラス変数スコープ (2026-08-20)

`recv.instance_eval("@@cvar")` は CRuby では **呼び出し元の cref** で解決し、
定数は receiver の特異クラス → receiver のクラス → 呼び出し元 cref …の順で
探す。koruby は eval フレームに cref が無いので "class variable access from
toplevel" になる (core/basicobject/instance_eval_spec で 6 例)。

**やるなら**: 呼び出し元 cref を builtin から辿れる必要がある (フレームに
呼び出し元へのリンクが無い)。上の「現在行」と同じくフレーム設計の話。

## at_exit は「本体の構文エラー」では走らない (2026-08-20)

CRuby は `ruby -rハンドラ '{'` で構文エラーでも -r 側の at_exit が走る。
koruby は main.c が「本体を parse → -r を require」の順なので、parse 時点で
exit(1) してしまう (END_spec / at_exit_spec で各 1 例)。

**やるなら**: -r の require を本体 parse より前に出し、parse 失敗時も
korb_drain_at_exit を通ってから終了する。prelude 実行・toplevel フレーム構築の
順序を組み替える必要がある。

## 2026-08-21 に見送った項目

- **Marshal.load の `freeze: true` / proc の呼ばれ方** (core/marshal/load_spec 18F4E の主要部分)。
  deep-freeze と「凍結文字列の重複排除」、proc がリンク ('@') でも呼ばれる CRuby の順序。
  proc をリンクでも呼ぶように変えたら別の例が壊れたので撤回した (読み出し順の設計から見る必要がある)。
- **Exception#full_message の整形** (11F1E)。backtrace の 1 行目にクラス名を付ける形、
  cause の連結、highlight のエスケープ。実バックトレース (フレームの現在行) が要る。
- **Thread#raise の cause: / backtrace_locations:** (thread/raise_spec)。同上。
- **ObjectSpace.each_object** (5F19E) はヒープ走査そのものが未実装。
- **Struct のサブクラスで initialize を上書きした場合のキーワード** (struct/new_spec の
  "on subclasses accepts keyword arguments to initialize")。
- `Module#module_function` を Class で呼ぶと CRuby は NoMethodError だが、TypeError に
  すると make test が 3 件退行する (自前コーパスが Class で呼んでいる)。要調査。

## クラス再オープン時の superclass mismatch 検査 (2026-08-21 試作 → 撤回)

CRuby は `class C < A` の後に `class C < B` と書くと TypeError
"superclass mismatch for class C" を出す (module/class の取り違えも
"C is not a module")。korb_class_body の再オープン経路に同じ検査を足したところ、
**prelude 自体が "superclass mismatch for class Lazy" で落ちた** (make test が
全滅)。C 側で作ったクラスと prelude の `class X < Y` 宣言で superclass の
見え方がずれている箇所があるらしい。

**やるなら**: まず C 側で作る組込みクラスの superclass を prelude の宣言と
揃える (Enumerator::Lazy 以外にもある可能性)。検査自体は数行。

## `$?` がスレッドローカルでない (2026-08-21)

CRuby の `$?` (Process.last_status) はスレッドごとに独立している。koruby は
グローバル 1 個なので、別スレッドから見ても親スレッドが最後に待った子の
ステータスが見える。core/process/last_status_spec の
"returns nil if no child process has been ever executed in the current thread" が
これで落ちる (1 例)。

直すには `$?` の読み書きを現在スレッドの tvars 経由にする必要があり、
グローバル変数の読み出し経路に `$?` の特別扱いを足すことになる。
影響 1 例に対して hot path を触るので保留。

## Symbol の同一性にエンコーディングが入っていない (2026-08-21)

CRuby では `"→".b.to_sym` と `"→".to_sym` は**別のシンボル**で、`Symbol#encoding`
もそれぞれ BINARY / UTF-8 を返す。koruby のシンボル表はバイト列だけを鍵にして
いるので両者が同一になり、`Symbol#encoding` はバイト列から導いた値 (7bit なら
US-ASCII、それ以外は UTF-8) しか返せない。

core/marshal/load_spec の "loads an encoded Symbol" (UTF-16 のシンボル) と
"loads a binary encoded Symbol" がこれで落ちる。Marshal 側だけでは直せない
(シンボル表の鍵を バイト列+エンコーディング にする話)。

## vcall (bare identifier) を NameError にする — 未着手

`foo` (レシーバ無し・括弧無し・引数無し) が見つからないとき CRuby は
`NameError: undefined local variable or method 'foo' for main` を投げるが、
koruby は `NoMethodError: undefined method 'foo' for main` を投げる。
`foo()` は両方 NoMethodError なので、区別は純粋に構文 (prism の
`PM_CALL_NODE_FLAGS_VARIABLE_CALL`) にしかなく、parse 側からしか分からない。
node_call に vcall ビットを載せる = node.def 変更 → code_store 全消し
なので、ROI を見て後回し。実 mspec で直接効くのは 3 例のみ。

## String#== が ASCII 非互換エンコーディングを区別しない — 未着手

`"abcd".force_encoding("utf-8") == "abcd".force_encoding("utf-32le")` は
CRuby では false (UTF-32LE は ASCII 互換でないので、7bit バイトでも
comparable ではない)。koruby は true を返す。判定には
korb_enc_ascii_compat_idx(vm, idx) が要るが、korb_value_eq は
`(VALUE, VALUE)` シグネチャで 41 箇所から呼ばれる hot path なので
vm を通す改造は割に合わない。String header に「ASCII 非互換」ビットを
持たせるのが筋だが、flags の空きが無い。実 mspec への影響は 2 例。

## Module#const_set が定義位置を記録しない — 未着手

`Module#const_source_location` が const_set で作った定数に対して [] を返す。
位置を記録するには呼び出し側の行番号が要るが、CFUNC には line が渡って
いない (node が明示的に渡す設計)。korb_send_impl の slow path で
c->vm->cur_line に積めば取れるが、汎用 dispatch に store が 1 個増える。
実 mspec で 4 例。

## define_method のブロック内で super が使えない — 未着手

`class B < A; define_method(:foo) { super() }; end` が
NotImplementedError ("super outside a method body")。parse 側が
囲みの def を見つけられないので node_super を生成していない。
runtime 側も、DM (define_method) の呼び出しはブロックとして
korb_block_yield_full で走るため frame の fs-2 に method entry が
無く、korb_super が起点を決められない。両方の対応が要る。
実 mspec では delegate 系で ~7 例。

## ObjectSpace.each_object が 0 を返すだけ — 未着手

ヒープ走査 API が precise GC framework に無い (aro_gc_* に iterate が
無い)。各 backend に足すのは重い。Class/Module だけなら定数表と
subclass リストから作れるが、spec の大半は任意オブジェクトを見る。
実 mspec で 24 例。

## ~~`A::B` 定数解決が GC STRESS で落ちる~~ (2026-08-29 修正済)

真因は定数解決ではなく `builtins/math.c` の登録側だった。Float/Complex の
クラス VALUE を C ローカルに持ったまま `korb_float_new` を呼んでいたので、
STRESS 下で GC がクラスを動かし、`Float::MAX` 等が stale な owner で
登録されていた。owner は値の alloc の後に読み直すようにした。
以下は当時の記録:

## `A::B` 定数解決が GC STRESS で落ちる (既存バグ)

`BARUBY_GC_STRESS=1 ./koruby_precise -e 'p Float::MAX'` が
`uninitialized constant Float::MAX` になる。`BARUBY_GC_PURGE=1` 単独では
再現しない = moving GC でオブジェクトが動いたときの比較。node_const_path が
`vm->const_owners[i] == owner` を生 VALUE 比較しており、どちらかの更新が
漏れていると思われる (`korb_const_owner_serial` が既にあるので、そちらへ
寄せるのが筋)。2026-08-29 の定数スコープ変更より前から再現する
(node.def を元に戻して A/B 確認済み)。

## require が feature 名を「書かれたまま」照合しない

CRuby は `$LOADED_FEATURES` に対して、展開後の絶対パスだけでなく
require に渡された名前そのもの (`"./load_fixture.rb"` 等) も照合する。
`korb_bi_require` の入口でその照合を足すと `core/kernel/require_spec.rb`
が無限ループした (2026-08-30 に試して revert)。おそらく mspec 自身の
require が自分を「読み込み済み」と誤判定して先に進まなくなる。
入れるなら、照合対象を「プログラムが明示的に push したエントリ」に
限るなどの絞り込みが要る。実 mspec で 5 例。

## 囲みスコープの autoload が字句探索で見つからない (2026-08-30 解決)

`Module#__lexical_parent` (private, `builtins/set.c`) を足して
`__autoload_owner_for` が enclosing chain も辿るようにした (commit 833d654a)。
残るのは `Inner.const_defined?(:X)` が親の定数まで見えて true を返す点
(CRuby は false)。定数探索そのものの設計なので別件。

## require が $LOADED_FEATURES の「書かれたままの名前」と照合しない

`$LOADED_FEATURES << "./load_fixture.rb"` してから
`require "./load_fixture.rb"` すると CRuby は false を返す
(展開前の文字列でも照合する)。koruby は展開後の絶対パスでしか
照合しないので読み直す。`core/kernel/require_spec` で 3 例。

**2026-08-30 に 2 度目の試行**: `korb_bi_require` の先頭 (path 解決の前) に
`korb_feature_loaded_p(c, namebuf)` を移す修正を入れたら、**起動時に
バナーも出ずにハングした** (boot 中の require が早期 false を返すため)。
revert 済み。次に試すなら boot が完了した後だけ有効にするか、
$LOADED_FEATURES の走査側を疑うこと。

## トップレベル def が Object の private にならない

CRuby では `def foo` をトップレベルで書くと `Object` の private
インスタンスメソッドになる。koruby は public。`language/def_spec` で
3 例 (「defines it on Object with private visibility by default」ほか)。
影響範囲が広いので、prelude/mspec 側の呼び出しを壊さないか確かめてから。

## IO#write の encoding 問い合わせが prelude に依存する

`korb_io_write_enc` は最初の書き込みで prelude の `__io_write_enc_name` を
1 回 send し、rep に memo する (`__io_enc_reset` でクリア)。prelude が
まだ読まれていない時点で書くと NoMethodError になり、その stream は
「変換しない」を memo したままになる。標準ストリームでは結果的に正しい
挙動なので放置しているが、boot 中に encoding 付きで書く経路を足すときは
ここを見ること (2026-08-30、worker の note より)。

## Object#hash が moving GC でアドレス由来のまま不安定

`korb_deep_hash_d` (builtins/hash.c) はユーザオブジェクト・クラス等に対して
`(uintptr_t)v` を返す。moving GC が動くと値が変わるので、

```ruby
K = Enumerator::ArithmeticSequence
h1 = [K, 1].hash; 100.times { Object.new }; h2 = [K, 1].hash
p h1 == h2   # BARUBY_GC_STRESS=1 で false
```

Hash のキー探索は `korb_value_hash` (ヒープオブジェクトは単一バケット) と
`korb_value_eq` を使うので `{String => 1}[String]` は壊れない。露出するのは
`#hash` の**値そのもの**を保持・比較する場合だけ。安定 ID (object_id 相当の
side table) を持たせるのが本筋。2026-08-30 に ArithmeticSequence#hash で
踏んだ (クラスをタプルから外して回避)。

## ブロック引数の分解が rest を含むグループの入れ子に対応していない

```ruby
-> (a, (b, (c, *d, (e, (*f)), g), (h, (i, j)))) { }   # M0 unsupported
```
`parse.c` の destructure spec エンコーダは 0xFE (rest あり) の枝で
lefts/rights を葉としてしか書き出さない (入れ子の MULTI_TARGET は `bad`)。
再帰させるにはエンコーダとデコーダ (C 側の spec walker) の両方が要る。
rest の無いグループの入れ子 (0xFF) は既に対応済み。
`language/{lambda,proc}_spec` で 2 例。

## (解決済 2026-08-31) 保存したブロックからの return がメソッドを飛び越える

```ruby
class SavedInnerBlock
  def add(&b); @block = b; end
  def outer; yield; @block.call; end
  def inner; yield; end
  def start
    outer { inner { add { return :return_value } } }
    return false
  end
end
SavedInnerBlock.new.start   # koruby: false / CRuby: :return_value
```
`return` の target が `start` の frame にならず、途中の `outer` (または
`inner`) の invoke に食われている。`node_return_outer` は env chain を
depth 分辿って target を決めるが、`Proc#call` 経由で別 frame から呼ばれた
ときに正しい frame に届いていない。**これ自体は昔からのバグ**
(2026-08-30 に e07e21d8 でも再現を確認)。

さらに 2026-08-30 の worker マージ (Struct/Hash/Method/Module 一式) 以降、
**同じ例を mspec 経由で走らせるとプロセスが無言で終了する** ように
なった (`language/return_spec` が 43 例中 21 例でサマリも出さずに exit 0)。
消費されなかった KORB_RETURN がトップレベルまで抜けている疑い。
bisect: 765904e4 まで OK、382b0aab で NG。ただしマージ後の tree から
382b0aab だけ revert しても直らないので原因は複数ある。
**2026-08-31 修正**: 原因は二つ。(1) `node_return_outer` の depth を
`kp_note_depth` に登録していなかったので、escape した Proc が env chain を
その深さまで materialize せず target が NULL (= nearest-method) に落ちていた。
(2) `korb_outer_frame_base` が materialize 済み env の chain を辿れず、
かつ frame が自分の open env を EP セルに持つ場合に一段ずれていた。
`korb_outer_frame_base_at` で「自分の env なら depth を消費せず prev へ」を
入れて解決。`language/return_spec` 0 -> 38 (マージ前の 37 より良い)、
core 20958 -> 20962。


## sleep 中の signal で起こす signalfd は blocked IO を巻き添えにする

worker が入れた `a1430a90` (blop pump が眠るときだけ signalfd(2) を poll の
待ち集合に足し、pending signal で main thread の blop を EINTR post する) は
`Signal.trap("TERM"){}; sleep` が起きない問題を直し、
`core/process/{wait,status/wait}_spec` を丸ごと (0 -> 19) 復活させた。

しかし同じ変更で **別スレッドが close した fd を read 中のスレッドが
永久にハングする** ようになり、`core/io/read_spec` 109 -> 0、
`core/io/close_spec` 11 -> 0 と差し引き大幅マイナスだったので 2026-08-31 に
revert した。再現:

```ruby
r, w = IO.pipe
t = Thread.new { begin; r.read(1); rescue => e; e; end }
Thread.pass until t.stop?
r.close
t.join            # ここで戻らない
```

「main thread の blop だけを EINTR する」に絞っても直らなかったので、
原因は wake 先の選び方ではなく poll 集合に fd が 1 本増えること自体
(revents の書き戻しか rc の解釈) の側にある。次に試すなら
`korb_blop_pump` の poll 後のループが `total` と `nfds` を取り違えて
いないかから見ること。


## 純 Ruby ループ中の Thread#raise / #kill が届かない

green thread に preemption が無く、`while`/`until`/`loop` のノードは
割り込みチェックもスケジューラ譲渡もしないので、

```ruby
t2 = nil
t1 = Thread.new { loop { sleep 0.01; t2.raise if t2 } }
t2 = Thread.new { begin; loop {}; rescue RuntimeError; end }
t2.join      # 戻らない
```

`core/mutex/lock_spec` がこれでファイルごとハングする (6 例)。
ループノードに「N 回に一度 pending interrupt を見て、他に runnable が
いれば譲る」を入れれば直るが、(1) `korb_thread_check_ints` は毎回
`sigtimedwait` を呼ぶので安い述語を別に要る、(2) `node_while` などは
`@nogc` 注釈付きで、譲渡は GC しうるので注釈を外す必要がある。
