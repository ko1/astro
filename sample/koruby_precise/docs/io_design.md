# koruby Thread / I/O 設計 — green thread M:N と blop 層

目標: Ruby Thread を **green thread (M) × native thread (N)** で実装し、blocking I/O を
select/epoll/io_uring の差し替え可能な backend で多重化する。**Phase 1 は N=1**
(GVL 相当は自明)。io_uring は Fiber scheduler のためではなく **Thread の M:N 化**の
ための基盤である。

> **実装状況** (2026-08-14 現在): Phase 1 の core は `builtins/thread.c` に実装済み —
> green thread scheduler (Thread.new/join/value/pass/current/list/stop/wakeup)、blop 層
> (korb_blop_wait / post / cancel / pump)、TIMER (sleep / join timeout)、POLL
> (IO#wait_readable / wait_writable / IO.select)、割り込み (Thread#raise / #kill、
> pending_ints + check_ints)。**fd ベースの IO 層も完了** (下の「IO 層の作り直し」節、
> stdio 撤去済み)。
>
> 未実装: **engine は poll(2) の 1 本だけ** (vtable 化も epoll / uring engine もまだ。
> probe 順序 uring→epoll→poll は設計のみ)、**blop kind も POLL と TIMER しか使っていない**
> (READ / WRITE / CFUNC は enum と union のフィールドだけ)、eventfd wake、close cancel、
> dead thread の stack reap。詳細は `docs/todo.md` の Thread/IO 節。

## フェーズ

| phase | 内容 |
|---|---|
| **1 (この文書の主対象)** | native 1 本 + green thread M 本。yield 点は blocking 操作のみ (協調・preemption なし)。engine = poll / epoll / io_uring |
| 2 以降 | CFUNC worker pool (DNS 等)、io_uring completion での regular-file 非同期化、N native (GVL + safepoint)、真の並列 (GC 全面改修、別プロジェクト) |

## 語彙

- **blop** = BLocking OPeration。CRuby の *blocking operation*
  (`rb_nogvl` / `rb_io_blocking_operation_enter` 系) に対応する koruby 語。
- **wait** = blop を実行し完了まで待つ (green thread を park。native はブロックしない)。
- **post** = engine/worker が完了を書き込み waiter を runnable にする。
- **engine** = backend 実装 (poll / epoll / uring)。
- **pump** = 完了回収ループ。scheduler の idle ループが呼ぶ、**native thread が眠る唯一の場所**。
- **korb_thread** = Ruby の Thread (green thread)。CRuby の `rb_thread_t` に対応。
  native 側は Phase 2 以降に `korb_native_thread` (CRuby の `struct rb_native_thread`
  と同じ対応関係)。状態: `RUNNING / READY / PENDED / DEAD`。

## 全体像

```
Ruby: IO#read, IO#write, IO#wait_readable, IO.select, Kernel#sleep, Thread
        │
IO 層 (io.c: fd ベースに作り直し)
   korb_io_read_bytes / korb_io_write_bytes   … String との境界 (copy はここに閉じる)
        │
blop 層 (唯一の suspension point)
   korb_blop_wait(c, slots, &blop)
        │ prep が ENOTSUP → 汎用 readiness fallback (nonblock 試行 + POLL park + retry)
engine (vtable)
   korb_blop_engine_uring   … completion 本物 (READ/WRITE を SQE で。regular file も非同期)
   korb_blop_engine_epoll   … readiness
   korb_blop_engine_poll    … POSIX fallback (poll(2)。select(2) は FD_SETSIZE=1024 で不採用)
        │
scheduler: run queue + park/unpark (既存 Fiber 基盤 = ucontext + malloc C stack を流用)
```

- **interface は completion 形に統一**し、readiness 系 engine が completion を
  エミュレートする (逆だと io_uring が POLL_ADD 止まりになり、regular file の
  非同期化が永遠にできない)。
- engine probe: **uring → (ENOSYS / EPERM) → epoll → poll**。
  EPERM は container の seccomp (Docker default profile は io_uring を deny) で
  常用経路として踏まれるので、黙って fallback する。
  `KORUBY_BLOP_ENGINE=poll|epoll|uring` で強制可。

## blop 層 API

```c
enum korb_blop_kind {
    KORB_BLOP_POLL,     /* fd readiness。pollfd 配列 (n=1: wait_readable, n>1: IO.select) */
    KORB_BLOP_READ,     /* fd → buf */
    KORB_BLOP_WRITE,    /* buf → fd */
    KORB_BLOP_TIMER,    /* 純粋な時間待ち (sleep / join timeout) */
    KORB_BLOP_CFUNC,    /* 任意 blocking C (worker 行き)。Phase 2 — 形だけ予約 */
};

enum korb_blop_flags { KORB_BLOP_F_TIMEOUT = 1 };

struct korb_blop {
    uint8_t  kind, flags;
    /* 共通。timeout は TIMER の本体であると同時に、IO#timeout (Ruby 3.2) が
       READ/WRITE/POLL に deadline を課すため union に入れない */
    struct timespec timeout;
    ssize_t  result;                  /* 完了時に engine が埋める (下表) */
    struct korb_thread *waiter;      /* 内部用: wait が埋める */
    union {                           /* op 固有 (NODE の u. と同じ流儀) */
        struct { struct pollfd *fds; nfds_t nfds; }             poll;
        struct { int fd; void *buf; size_t len; int64_t off; }  rw;    /* READ/WRITE 共用 */
        struct { void *(*fn)(void *); void *arg;
                 void  (*ubf)(void *); void *ubf_arg; }         cfunc; /* rb_nogvl 同形 */
    } u;
};

/* ---- 唯一の suspension point ---------------------------------------- */
/* blop を実行し完了まで待つ。
 *  - green thread を park。native thread は他の green thread を実行
 *  - fast path: 即完了できるなら park しない
 *  - 戻り時に b->result 確定。エラーの raise は caller (IO 層) の仕事
 *  - 戻り際に必ず check_ints (pending interrupt を RESULT_RAISE / KILL で配送)
 *  - may-GC (park 中に他 green thread が alloc する) */
RESULT korb_blop_wait(CTX *c, VALUE *slots, struct korb_blop *b);

/* ---- engine 側 ------------------------------------------------------- */
void korb_blop_post     (struct korb_vm *vm, struct korb_blop *b, ssize_t result);
void korb_blop_cancel   (struct korb_vm *vm, struct korb_blop *b, int neg_errno);
void korb_blop_cancel_fd(struct korb_vm *vm, int fd, int neg_errno);   /* IO#close 用 */

struct korb_blop_engine {
    const char *name;
    int  (*init)   (struct korb_vm *);
    void (*destroy)(struct korb_vm *);
    int  (*prep)   (struct korb_vm *, struct korb_blop *);  /* ENOTSUP → 汎用 fallback */
    int  (*cancel) (struct korb_vm *, struct korb_blop *);
    int  (*pump)   (struct korb_vm *, const struct timespec *);
};
int korb_blop_init(struct korb_vm *vm);   /* probe + eventfd 常設登録 */
```

### result の意味

| kind | result |
|---|---|
| POLL | ready になった fd 数 (poll(2) と同じ。0 = timeout)。per-fd 結果は `fds[i].revents` |
| READ / WRITE | 転送 byte 数 / 負値 = -errno (`-ETIMEDOUT` / `-ECANCELED` 含む) |
| TIMER | 0 = 満了 / `-ECANCELED` |
| CFUNC | `(intptr_t)fn(arg)` の戻り / `-ECANCELED` (QUEUED 中に取り消し) |

### blop の置き場所と寿命

blop 本体は **caller (= park する green thread) の C スタック上**に置く。fiber の
C スタックは malloc (不動・GC 外) なので、engine が `&blop` を保持したまま park
しても安全。alloc 不要、寿命は wait スコープと一致。`u.poll.fds` の pollfd 配列も
同様に caller スタックでよい。

将来 submit / wait を 2 相に分離したくなっても (uring バッチ等)、caller 所有の
blop はそのまま「先に積んで後で待つ」形に割れる。Phase 1 では公開しない。

### 汎用 readiness fallback (poll / epoll engine の READ/WRITE)

```
for (;;) {
    n = read/write(fd, buf, len);          /* fd は O_NONBLOCK */
    n >= 0        → result = n; return     /* 0 = EOF もここ */
    EINTR         → continue
    !EAGAIN       → result = -errno; return
    EAGAIN        → 内部 POLL blop で park
                    /* 起床 = ready だったが、同 fd を待つ別 green thread に
                       先に消費されたかもしれない → ループ先頭で再試行 */
}
```

I/O 本体を実行するのは pump ではなく**起こされた green thread 自身** (reactor)。
uring は kernel が実行済みの結果を受け取る (proactor)。

### 既知の制約: regular file

poll は regular file に対し常に ready を返し、read(2) は O_NONBLOCK を無視して
ディスク待ちする。**poll/epoll engine では遅いディスク read が native thread ごと
止まる** (= Phase 1 の全 green thread)。uring engine は READ を SQE で投げるため
この穴がない。readiness では原理的に解決不能。

## IO 層の作り直し (fd ベース)

現行 io.c の `FILE*` (stdio) は completion モデルと噛み合わないため **raw fd +
自前バッファ**に作り直す。

- `KorbIORep`: fd / O_NONBLOCK / read-ahead バッファ (**libc malloc = 不動**。
  KorbFiberRep と同じ扱い)。
- **stable-buffer 規約: `u.rw.buf` / `u.poll.fds` は不動メモリ限定**
  (malloc / C スタック / IO rep 内部バッファ)。movable な KorbStrBuf を渡すのは禁止
  — completion モードでは park 中に kernel が書き込むため、GC が動くと stale になる。
  readiness fallback では自明に安全だが、engine 切替で安全性が変わる規約は事故の元
  なので一律に課す。
- 規約を守る場所は helper 2 つに閉じる: `korb_io_read_bytes` / `korb_io_write_bytes`。
  call site は VALUE_REF (String) を渡すだけで、stable バッファとの 1 copy は
  helper 内部。CRuby が write で `rb_str_tmp_frozen_acquire` の安定スナップショットを
  取るのと同じ構図。
- Ruby レベルの outbuf (`IO#read(len, outbuf)`) は最後に replace で書き戻すだけ。
  C 層に String の生バッファは渡らない。
- `IO.select` は io.c 側で pollfd 配列を組んで POLL blop 1 個
  (真の意味論 =「その時点で ready な全部」が revents で一発で返る)。

### 実装状況 (2026-08-10 完了)

**stdio は撤去済み。descriptor を包む `FILE*` はゼロ。**

```c
typedef struct KorbIORep {
    int      fd;              /* -1 = closed; KORB_IO_FD_MEM = in-memory sink */
    uint8_t  eof;             /* read(2) が 0 を返した */
    uint8_t  sync;            /* write ごとに flush */
    uint8_t  nonblk;          /* O_NONBLOCK: block する代わりに park する */
    char    *rbuf; uint32_t rpos, rlen, rcapa;   /* read-ahead: 生きているのは [rpos, rlen) */
    char    *wbuf; uint32_t wlen, wcapa;         /* write-behind */
} KorbIORep;
```

byte 層 (descriptor に触るのはここだけ):
`korb_io_wr_p` / `korb_io_fill_p` / `korb_io_getb_p` / `korb_io_unget` /
`korb_io_seek_rep` / `korb_io_tell_rep` / `korb_io_flush_rep_p` /
`korb_io_close_rep` / `korb_io_park`。
`_p` 版は `CTX *c` を取り、`c == NULL` が「ここでは park してはいけない」の印
(exit 経路・close・in-memory sink)。park 中に配送された Thread#raise / #kill は
`RESULT *perr` out-param で伝播する。

置き換え結果:

| 旧 | 現在 |
|---|---|
| `fread` | `rbuf` から供給、空なら `read(2)`。EAGAIN → park → retry |
| `fwrite` | `wbuf` に積んで閾値/sync/flush/close で `write(2)` (部分書き込みループ) |
| `fgetc` | `korb_io_getb_p` |
| `ungetc` | `korb_io_unget` — **1 バイト制限が無くなった** (バッファが自前なので run ごと押し戻せる) |
| `fseek`/`ftell` | `lseek(2)` + `rlen - rpos` 補正。seek で rbuf 破棄、wbuf は flush |
| `feof` | `korb_io_fill_p` が 0 を返したとき |
| `fflush` | `korb_io_flush_rep_p` |
| `fdopen`/`fclose`/`freopen` | 不要。reopen は `open(2)` + `dup2(2)` + バッファ破棄 |
| `getline(3)` | `korb_io_read_line` (rbuf 上の memchr スキャン) |
| `open_memstream` (出力捕捉) | `fd == KORB_IO_FD_MEM` の rep |

**park 規律 (実装済み)**: 転送バッファは rep 内 (libc alloc、不動) なので park を
跨いでも動かない。String との境界は copy 1 回。**String のバイト列へのポインタを
park を跨いで保持するのは禁止** — read は「fill → String を再導出 → memcpy」の形に
統一し (`read_bytes` / `read_all_bytes` / `read_line` が同じ骨格)、write は park し得る
stream では run 全体を先に wbuf へ copy してから drain する。
この規律違反は CodeQL の borrow-after-gc が実際に 1 件検出した (`IO#reopen` の path)。

**順序規律 (実装済み)**: 必ず **nonblock で試す → EAGAIN → park → retry**。
逆順 (park してから blocking 呼び出し) は spurious wakeup / abort された接続 /
複数 reader の競合で scheduler ごと固まる。`korb_io_park` のコメントに明記。

**nonblk にする範囲**: koruby が方針を握れる descriptor だけ — `IO.pipe` の両端、
popen の親側、socket (`SO_TYPE` で検出するので `IO.new(sockfd)` も対象)。
std stream は他プロセスと共有する open file description なので kernel blocking のまま。
`spawn` の子には blocking な fd を渡す (`O_NONBLOCK` は description の属性なので、
EAGAIN を理解しない子プログラムに漏れる)。

**exit と fork**: stdio が消えたので自動 flush が無い。`korb_io_flush_std` を
main の全 return / `exit` / `exit!` / `abort` と **fork の直前**に置く
(子がバッファを継承すると二重出力になる)。

**stdout のバッファリング**: tty なら sync、piped ならバッファ。`IO#sync=` は
実際に効く (true にすると溜まっている分を drain するので、代入前の出力にも効く)。

**残っている `FILE*`**: `open_memstream` のみ。`korb_fprint_*` (inspect/to_s の
プリンタ群) が `FILE*` を取るのはこの文字列ビルダ用で、descriptor には繋がらない。
`Kernel#p` / `#print` は「memstream に組み立て → rep へ 1 回 write」で、
`#puts` と同じ sink を通る (混ぜると出力順序が割れるため分離不可)。

## 双方向ストリームの約束事 (2026-08 に踏んだ罠)

koruby の IO は **1 オブジェクト = 1 ディスクリプタ**。双方向 (`IO.popen(cmd, "r+")`、
socketpair、socket) を扱うときにここから来る制約が 3 つある。

1. **duplex popen は pipe(2) では作れない。** pipe は片方向なので、親が 1 本の fd で
   読み書きするには `socketpair(AF_UNIX, SOCK_STREAM)` を使い、子には peer を stdin と
   stdout の両方に配る。親側は rw=3 の IO 1 つ。
2. **`#close_read` / `#close_write` は相手が socket なら `shutdown(2)` する。**
   koruby は方向ビット (`@__io_mode`) を落として両方向無くなった時点で close する実装
   だが、socket ではそれだけだと peer に EOF が伝わらない。duplex popen で
   `close_write` しても子が読み続け、親の read と相互待ちになる。
3. **read(2) でブロックする前に自分の書き込みバッファを flush する** (`korb_io_fill_p`)。
   相手が返事を待っている間こちらの出力が溜まったままだと両者が止まる。CRuby は
   duplex IO のバッファ状態を共有しているので同じ効果になる。

関連して `IO.copy_stream` は `#read(n)` ではなく **`#readpartial` を使う** (n バイト
揃うまで待つ read だと、対話的な pipe 越しのコピーが進まない)。

## cancel / 割り込み (Thread#kill / #raise / signal)

Phase 1 の単純化: **kill を発行する側も green thread なので、発行時点で対象は
RUNNING ではあり得ない** (同時に走るのは 1 人)。対象は READY か PENDED のみ。

```c
struct korb_thread {
    uint8_t  state;                 /* RUNNING / READY / PENDED / DEAD */
    struct korb_blop *blop;         /* PENDED 中に待っている blop (それ以外 NULL)。
                                       CRuby 3.4 の rb_io_blocking_operation_enter/exit
                                       に相当する追跡 */
    VALUE    pending_ints;          /* 割り込み queue: 例外 VALUE / KILL マーカ (GC root) */
    ...
};

void   korb_thread_interrupt (struct korb_vm *vm, struct korb_thread *t, VALUE exc_or_kill);
RESULT korb_thread_check_ints(CTX *c, VALUE *slots);   /* RUBY_VM_CHECK_INTS 相当 */
```

- `interrupt`: pending_ints に積む。対象が PENDED なら
  `korb_blop_cancel(vm, t->blop, -ECANCELED)` で蹴り起こす。
- 配送点は `korb_blop_wait` の戻り (Phase 1 ではそこ以外で割り込みが見える瞬間がない)。
- **kill は rescue 不能・ensure は走る**: 通常の例外 VALUE ではなく専用 unwind
  (RESULT の KILL 状態) で流す。
- **完了 vs cancel の race は「完了が勝つ」で統一**: uring の ASYNC_CANCEL が
  ENOENT を返したら実結果が立ち、割り込みは直後の check_ints で配送
  (READ が consume したデータは kill と同着なら失われる — CRuby と同じ)。
- `IO#close` → `korb_blop_cancel_fd(vm, fd, -EBADF)`。待っていた thread は
  CRuby 同様 `IOError: stream closed in another thread`。
- timeout は割り込みと独立: engine が `-ETIMEDOUT` を post → IO 層が
  `IO::TimeoutError` に変換。

### pump wake チャネル (eventfd)

engine init 時に eventfd (無ければ self-pipe) を 1 本**常設で POLL 登録**する。

- Phase 1 の用途: **POSIX signal**。pump が epoll_wait / io_uring_enter で眠っている
  最中の Ctrl-C は、signal handler が eventfd に write (async-signal-safe) して
  pump を起こし、main thread に Interrupt を積む。
- 将来そのまま昇格: worker の CFUNC 完了 post、N native 化での cross-thread kick。

## CFUNC (Phase 2 — 形の予約のみ)

多重化できない blocking (getaddrinfo の DNS、任意の C ライブラリ) の逃がし先。
`rb_nogvl(func, data1, ubf, data2)` と同形で、**worker native thread** で実行し
完了を post する。worker pool は**初回 CFUNC で lazy 起動** (使わないプログラムは
native 1 本のまま)。

- **ubf は 3 択** (CRuby 踏襲):
  - `NULL` — 割り込み不可。kill は fn が返るまで保留、直後の check_ints で配送。
  - `KORB_UBF_KICK` (組み込み) — `RUBY_UBF_IO` 相当。worker に pthread_kill
    (no-op ハンドラ・SA_RESTART なしの専用 signal) → syscall が EINTR で返る。
    fn は EINTR を見て畳んで返る契約。
  - custom — self-pipe に書く / 複製 fd を close する等。
- **state machine で race を潰す**: `QUEUED → RUNNING → DONE`。
  QUEUED で cancel = queue から抜いて post(-ECANCELED)、fn は走らない。
  RUNNING で cancel = ubf を呼ぶ (**ubf は複数回呼ばれ得る前提で冪等・thread-safe**)。
  fn はやがて返り実結果が post、割り込みは直後に配送 (完了が勝つ規則)。
- **fn / ubf の絶対契約: CTX・VALUE に触るの禁止**。VM と並行に走る間に moving GC が
  動くため、違反は即 SEGV。引数・結果は plain C データ (malloc) 限定。
  worker 実装時に CodeQL ルール (fn の call graph に korb_* / VALUE 触りがあれば
  error) をセットで追加する。

## GC / CodeQL との接続

- `korb_blop_wait` は **may-GC** (park 中に他 green thread が alloc)。ただし
  swapcontext 越しのため korb_alloc への呼び出し閉包では推論できない —
  **maygc.ql / borrow_after_gc.ql / value_after_gc.ql の seed に明示追加**すること
  (さもないと wait 跨ぎの stale borrow / stale VALUE が節穴になる)。
- stable-buffer 規約の機械化: 「`ARO_BORROW` accessor 由来 (= data_priv 由来) の
  ポインタが `u.rw.buf` / `u.poll.fds` に流れたら error」— 既存 borrow-flow 述語の
  流用でルール 1 本。blop 実装と同時に gate へ足す。

## Phase 1 の宣言事項 (制約)

1. **協調スケジューリングのみ**。blocking 点 (blop_wait) と Thread.pass 以外で
   yield しない。compute-bound な green thread は他の全 thread を止める。
2. regular file のディスク待ちは poll/epoll engine では native ごとブロック
   (uring engine なら非同期)。
3. CFUNC 未実装の間、getaddrinfo 等は native を止める。
4. 割り込み配送は blop_wait 戻りのみ。

## N native 化で来るもの (先送りの理由ごと)

preemption (quantum) / RUNNING への非同期割り込み / STW GC の safepoint は、
**3 つとも「実行中コードが定期的に踏む check point (safepoint)」1 機構に還元**
される。fastpath に check を入れる設計 (method entry のカウンタ / Go 流 signal
差し込み / GC poll 点との共用) は N native フェーズでまとめて払う。Phase 1 で
先払いするのは形だけ: `korb_thread_interrupt` / `check_ints` の API 形、
`t->blop` 逆リンク、eventfd チャネル — いずれも N native でそのまま使う。
