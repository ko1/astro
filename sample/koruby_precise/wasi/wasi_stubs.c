/* koruby_precise — WASI: builtins/process.c と builtins/socket.c の代わり。
 *
 * WASI にはプロセスもソケットも無いので、その 2 ファイル (計 1,938 行) は
 * まるごとコンパイルしない。prelude/system.rb がこれらの builtin を参照して
 * いるので、入口だけ用意する:
 *
 *   - prelude のロード時に評価されるもの (__rlimit_table) は空の値を返す
 *   - 残りは呼ばれたら NotImplementedError
 *
 * 「動かないものは動かないと分かる」ことを優先している。黙って 0 を返すと
 * Process.wait が成功したように見えてしまう。
 *
 * #included into korb_runtime.c's TU. */

static RESULT korb_wasi_unsupported(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    return korb_raise(c, slots, KORB_E_NOTIMPL, 0,
                      "process/signal/socket operations are not available on WASI");
}

/* Process.getrlimit の資源名表。prelude のロード時に読まれるので空の Hash を返す
 * (資源が 1 つも無い = setrlimit/getrlimit は名前を解決できない)。 */
static RESULT korb_wasi_rlimit_table(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)self; (void)a;
    return korb_hash_new(c, slots, 0);
}

/* シグナル名 ↔ 番号。WASI にシグナルは無いので「そんな名前は無い」を返す。 */
static RESULT korb_wasi_nil(CTX *c, VALUE *slots, VALUE_REF self, VALUE_SLICE a) {
    (void)c; (void)slots; (void)self; (void)a;
    return RESULT_OK(KORB_NIL);
}

void korb_init_socket(CTX *c, VALUE *slots) { (void)c; (void)slots; }

void korb_init_process(CTX *c, VALUE *slots) {
    (void)slots;
    const VALUE obj = korb_builtin_class_obj(c->vm, KORB_C_OBJECT);
    static const char *const unsupported[] = {
        "system", "spawn", "exec", "`", "__system", "__spawn", "__exec",
        "__waitpid", "__waitpid2", "__set_last_status", "__kill", "__getpgid",
        "__setpgid", "__getpriority", "__setpriority", "__getrlimit", "__setrlimit",
        "__signal_trap", "__signal_block", "__deliver", "__process_test",
        "__process_times", "__time_at", "__mkstatus", NULL };
    for (uint32_t i = 0; unsupported[i] != NULL; i++)
        korb_class_def_cfn(c, obj, unsupported[i], korb_wasi_unsupported, -1);
    korb_class_def_cfn(c, obj, "__rlimit_table",   korb_wasi_rlimit_table, -1);
    korb_class_def_cfn(c, obj, "__signal_signame", korb_wasi_nil,          -1);
    korb_class_def_cfn(c, obj, "__signal_signo",   korb_wasi_nil,          -1);
}

/* thread.c が呼ぶが、実体は除外した process.c にある。WASI ではシグナルが
 * 配送されないので、届いていない = 何もしないでよい。 */
static RESULT korb_signal_deliver(CTX *c, VALUE *slots) { (void)c; (void)slots; return RESULT_OK(KORB_NIL); }
