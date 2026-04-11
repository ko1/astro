// ASTro node common infrastructure
//
// Provides hash functions, HASH, DUMP, and alloc_dispatcher_name.
//
// #include this file from your node.c, AFTER defining:
//   - node_allocate(size_t)
//   - dispatch_info(CTX*, NODE*, bool)
// and BEFORE #including astro_code_store.c and generated files.

// ---------------------------------------------------------------------------
// Hash functions (used by generated node_hash.c)
// ---------------------------------------------------------------------------

static node_hash_t
hash_merge(node_hash_t h, node_hash_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 12) + (h >> 4);
    return h;
}

static node_hash_t
hash_cstr(const char *s)
{
    node_hash_t h = 14695981039346656037ULL; // FNV offset basis for 64-bit
    const node_hash_t FNV_PRIME = 1099511628211ULL;

    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= FNV_PRIME;
    }

    return h;
}

static node_hash_t
hash_uint32(uint32_t ui)
{
    node_hash_t x = (node_hash_t)ui;

    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;

    return x;
}

static node_hash_t
hash_double(double d)
{
    union { double d; uint64_t u; } conv;
    conv.d = d;
    return hash_uint32((uint32_t)(conv.u ^ (conv.u >> 32)));
}

static node_hash_t
hash_node(NODE *n)
{
    if (!n) return 0;
    if (n->head.flags.has_hash_value) {
        return n->head.hash_value;
    }
    else {
        return HASH(n);
    }
}

// ---------------------------------------------------------------------------
// General node operations
// ---------------------------------------------------------------------------

node_hash_t
HASH(NODE *n)
{
    if (n == NULL) {
        return 0;
    }
    else if (n->head.flags.has_hash_value) {
        return n->head.hash_value;
    }
    else if (n->head.kind->hash_func) {
        n->head.flags.has_hash_value = true;
        return n->head.hash_value = (*n->head.kind->hash_func)(n);
    }
    else {
        return 0;
    }
}

void
DUMP(FILE *fp, NODE *n, bool oneline)
{
    if (!n) {
        fprintf(fp, "<NULL>");
    }
    else if (n->head.flags.is_dumping) {
        fprintf(fp, "...");
    }
    else {
        n->head.flags.is_dumping = true;
        (*n->head.kind->dumper)(fp, n, oneline);
        n->head.flags.is_dumping = false;
    }
}

// ---------------------------------------------------------------------------
// Print a C string literal with proper escaping (used by generated node_specialize.c)
// ---------------------------------------------------------------------------

__attribute__((unused)) static void
astro_fprint_cstr(FILE *fp, const char *s)
{
    fprintf(fp, "        \"");
    for (; *s; s++) {
        switch (*s) {
        case '"':  fprintf(fp, "\\\""); break;
        case '\\': fprintf(fp, "\\\\"); break;
        case '\n': fprintf(fp, "\\n"); break;
        case '\r': fprintf(fp, "\\r"); break;
        case '\t': fprintf(fp, "\\t"); break;
        default:   fputc(*s, fp);
        }
    }
    fprintf(fp, "\"");
}

// ---------------------------------------------------------------------------
// Dispatcher name allocation (used by generated node_specialize.c)
// ---------------------------------------------------------------------------

static const char *
alloc_dispatcher_name(NODE *n)
{
    char buff[128];
    snprintf(buff, sizeof(buff), "SD_%lx", (unsigned long)hash_node(n));
    char *name = malloc(strlen(buff) + 1);
    strcpy(name, buff);
    return name;
}
