int  astro_jit_start(const char *l1_uds_path);
void astro_jit_stop(void);
void astro_jit_submit_query(NODE *node);
void astro_jit_submit_compile(NODE *node);
