// This file is auto-generated from node.def.
// specializers

        static void
        SPECIALIZE_node_num(FILE *fp, NODE *n, bool is_public)
        {
        
            const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
            n->head.dispatcher_name = dispatcher_name;

            // comment
            fprintf(fp, "// ");
            DUMP(fp, n, true);
            fprintf(fp, "\n");

        

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "VALUE\n");
            fprintf(fp, "%s(CTX *c,  NODE *n)\n", dispatcher_name);
            fprintf(fp, "{\n");
            fprintf(fp, "    dispatch_info(c, n, false);\n");
    fprintf(fp, "    VALUE v = EVAL_node_num(c, n, \n");
    fprintf(fp, "        %d", n->u.node_num.num);
    fprintf(fp, "\n    );\n");
            fprintf(fp, "    dispatch_info(c, n, true);\n");
            fprintf(fp, "    return v;\n");
            fprintf(fp, "}\n\n");
        }

        static void
        SPECIALIZE_node_add(FILE *fp, NODE *n, bool is_public)
        {
            SPECIALIZE(fp, n->u.node_add.lv);
    SPECIALIZE(fp, n->u.node_add.rv);
            const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
            n->head.dispatcher_name = dispatcher_name;

            // comment
            fprintf(fp, "// ");
            DUMP(fp, n, true);
            fprintf(fp, "\n");

            if (n->u.node_add.lv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_add.lv->head.dispatcher_name); }
    if (n->u.node_add.rv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_add.rv->head.dispatcher_name); }

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "VALUE\n");
            fprintf(fp, "%s(CTX *c,  NODE *n)\n", dispatcher_name);
            fprintf(fp, "{\n");
            fprintf(fp, "    dispatch_info(c, n, false);\n");
    fprintf(fp, "    VALUE v = EVAL_node_add(c, n, \n");
    fprintf(fp, "        n->u.node_add.lv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_add.lv));
    fprintf(fp, ",\n");
    fprintf(fp, "        n->u.node_add.rv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_add.rv));
    fprintf(fp, "\n    );\n");
            fprintf(fp, "    dispatch_info(c, n, true);\n");
            fprintf(fp, "    return v;\n");
            fprintf(fp, "}\n\n");
        }

        static void
        SPECIALIZE_node_sub(FILE *fp, NODE *n, bool is_public)
        {
            SPECIALIZE(fp, n->u.node_sub.lv);
    SPECIALIZE(fp, n->u.node_sub.rv);
            const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
            n->head.dispatcher_name = dispatcher_name;

            // comment
            fprintf(fp, "// ");
            DUMP(fp, n, true);
            fprintf(fp, "\n");

            if (n->u.node_sub.lv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_sub.lv->head.dispatcher_name); }
    if (n->u.node_sub.rv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_sub.rv->head.dispatcher_name); }

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "VALUE\n");
            fprintf(fp, "%s(CTX *c,  NODE *n)\n", dispatcher_name);
            fprintf(fp, "{\n");
            fprintf(fp, "    dispatch_info(c, n, false);\n");
    fprintf(fp, "    VALUE v = EVAL_node_sub(c, n, \n");
    fprintf(fp, "        n->u.node_sub.lv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_sub.lv));
    fprintf(fp, ",\n");
    fprintf(fp, "        n->u.node_sub.rv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_sub.rv));
    fprintf(fp, "\n    );\n");
            fprintf(fp, "    dispatch_info(c, n, true);\n");
            fprintf(fp, "    return v;\n");
            fprintf(fp, "}\n\n");
        }

        static void
        SPECIALIZE_node_mul(FILE *fp, NODE *n, bool is_public)
        {
            SPECIALIZE(fp, n->u.node_mul.lv);
    SPECIALIZE(fp, n->u.node_mul.rv);
            const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
            n->head.dispatcher_name = dispatcher_name;

            // comment
            fprintf(fp, "// ");
            DUMP(fp, n, true);
            fprintf(fp, "\n");

            if (n->u.node_mul.lv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_mul.lv->head.dispatcher_name); }
    if (n->u.node_mul.rv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_mul.rv->head.dispatcher_name); }

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "VALUE\n");
            fprintf(fp, "%s(CTX *c,  NODE *n)\n", dispatcher_name);
            fprintf(fp, "{\n");
            fprintf(fp, "    dispatch_info(c, n, false);\n");
    fprintf(fp, "    VALUE v = EVAL_node_mul(c, n, \n");
    fprintf(fp, "        n->u.node_mul.lv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_mul.lv));
    fprintf(fp, ",\n");
    fprintf(fp, "        n->u.node_mul.rv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_mul.rv));
    fprintf(fp, "\n    );\n");
            fprintf(fp, "    dispatch_info(c, n, true);\n");
            fprintf(fp, "    return v;\n");
            fprintf(fp, "}\n\n");
        }

        static void
        SPECIALIZE_node_div(FILE *fp, NODE *n, bool is_public)
        {
            SPECIALIZE(fp, n->u.node_div.lv);
    SPECIALIZE(fp, n->u.node_div.rv);
            const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
            n->head.dispatcher_name = dispatcher_name;

            // comment
            fprintf(fp, "// ");
            DUMP(fp, n, true);
            fprintf(fp, "\n");

            if (n->u.node_div.lv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_div.lv->head.dispatcher_name); }
    if (n->u.node_div.rv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_div.rv->head.dispatcher_name); }

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "VALUE\n");
            fprintf(fp, "%s(CTX *c,  NODE *n)\n", dispatcher_name);
            fprintf(fp, "{\n");
            fprintf(fp, "    dispatch_info(c, n, false);\n");
    fprintf(fp, "    VALUE v = EVAL_node_div(c, n, \n");
    fprintf(fp, "        n->u.node_div.lv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_div.lv));
    fprintf(fp, ",\n");
    fprintf(fp, "        n->u.node_div.rv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_div.rv));
    fprintf(fp, "\n    );\n");
            fprintf(fp, "    dispatch_info(c, n, true);\n");
            fprintf(fp, "    return v;\n");
            fprintf(fp, "}\n\n");
        }

        static void
        SPECIALIZE_node_mod(FILE *fp, NODE *n, bool is_public)
        {
            SPECIALIZE(fp, n->u.node_mod.lv);
    SPECIALIZE(fp, n->u.node_mod.rv);
            const char *dispatcher_name = alloc_dispatcher_name(n); // SD_%lx % hash_node(n)
            n->head.dispatcher_name = dispatcher_name;

            // comment
            fprintf(fp, "// ");
            DUMP(fp, n, true);
            fprintf(fp, "\n");

            if (n->u.node_mod.lv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_mod.lv->head.dispatcher_name); }
    if (n->u.node_mod.rv) { fprintf(fp, "static inline VALUE %s(CTX *c, NODE *n);\n", n->u.node_mod.rv->head.dispatcher_name); }

            if (!is_public) fprintf(fp, "static ");
            fprintf(fp, "VALUE\n");
            fprintf(fp, "%s(CTX *c,  NODE *n)\n", dispatcher_name);
            fprintf(fp, "{\n");
            fprintf(fp, "    dispatch_info(c, n, false);\n");
    fprintf(fp, "    VALUE v = EVAL_node_mod(c, n, \n");
    fprintf(fp, "        n->u.node_mod.lv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_mod.lv));
    fprintf(fp, ",\n");
    fprintf(fp, "        n->u.node_mod.rv,\n");
    fprintf(fp, "        %s", DISPATCHER_NAME(n->u.node_mod.rv));
    fprintf(fp, "\n    );\n");
            fprintf(fp, "    dispatch_info(c, n, true);\n");
            fprintf(fp, "    return v;\n");
            fprintf(fp, "}\n\n");
        }

