/*
 * File-type table for the `-t LANG` / `-T LANG` filter.
 *
 * Each entry is `{name, {glob, ..., NULL}}`.  Globs are matched with
 * fnmatch(3) against the basename only — full-path globs aren't
 * useful since the walker already resolves directories itself.
 *
 * Order in the static table determines `--type-list` output order;
 * it has no semantic effect beyond that.  Types may overlap (a `.h`
 * file is both `c` and `cpp`); both `-t c` and `-t cpp` will pick it
 * up if either is asked for.
 */

#include "types.h"
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

/* The static table.  Names are lowercase + ASCII; user input is
 * normalised at lookup time only by exact-strcmp, so users should
 * spell types in lowercase to match. */
#define G(...)  (const char *[]){ __VA_ARGS__, NULL }

static const are_type_t builtin_types[] = {
    {"c",        G("*.c", "*.h")},
    {"cpp",      G("*.cpp", "*.cxx", "*.cc", "*.hpp", "*.hxx", "*.hh", "*.h")},
    {"rust",     G("*.rs")},
    {"go",       G("*.go")},
    {"ruby",     G("*.rb", "*.rake", "*.gemspec", "Rakefile", "Gemfile",
                   "Gemfile.lock", "*.ru", "config.ru")},
    {"python",   G("*.py", "*.pyi", "*.pyx", "*.pyw")},
    {"py",       G("*.py", "*.pyi", "*.pyx", "*.pyw")},
    {"js",       G("*.js", "*.jsx", "*.mjs", "*.cjs")},
    {"ts",       G("*.ts", "*.tsx", "*.mts", "*.cts")},
    {"java",     G("*.java")},
    {"kotlin",   G("*.kt", "*.kts")},
    {"scala",    G("*.scala", "*.sc")},
    {"swift",    G("*.swift")},
    {"objc",     G("*.m", "*.mm", "*.h")},
    {"cs",       G("*.cs", "*.csx")},
    {"haskell",  G("*.hs", "*.lhs")},
    {"ocaml",    G("*.ml", "*.mli")},
    {"erlang",   G("*.erl", "*.hrl")},
    {"elixir",   G("*.ex", "*.exs")},
    {"clojure",  G("*.clj", "*.cljs", "*.cljc", "*.edn")},
    {"lisp",     G("*.lisp", "*.lsp", "*.cl")},
    {"scheme",   G("*.scm", "*.ss", "*.rkt")},
    {"lua",      G("*.lua")},
    {"perl",     G("*.pl", "*.pm", "*.t")},
    {"php",      G("*.php", "*.phtml")},
    {"sh",       G("*.sh", "*.bash", "*.zsh", "*.ksh", ".bashrc", ".zshrc",
                   ".bash_profile", ".profile", "*.bash_aliases")},
    {"fish",     G("*.fish")},
    {"sql",      G("*.sql")},
    {"html",     G("*.html", "*.htm", "*.xhtml")},
    {"css",      G("*.css")},
    {"scss",     G("*.scss", "*.sass")},
    {"less",     G("*.less")},
    {"vue",      G("*.vue")},
    {"svelte",   G("*.svelte")},
    {"md",       G("*.md", "*.markdown", "*.mdx")},
    {"rst",      G("*.rst")},
    {"tex",      G("*.tex", "*.bib", "*.cls", "*.sty")},
    {"json",     G("*.json", "*.jsonc")},
    {"yaml",     G("*.yaml", "*.yml")},
    {"toml",     G("*.toml")},
    {"xml",      G("*.xml", "*.xsd", "*.xsl", "*.xslt")},
    {"ini",      G("*.ini", "*.cfg", "*.conf")},
    {"make",     G("Makefile", "GNUmakefile", "*.mk", "*.mak")},
    {"cmake",    G("CMakeLists.txt", "*.cmake")},
    {"docker",   G("Dockerfile", "Dockerfile.*", "*.dockerfile",
                   ".dockerignore", "docker-compose*.yml", "docker-compose*.yaml")},
    {"proto",    G("*.proto")},
    {"asm",      G("*.s", "*.S", "*.asm")},
    {"vim",      G("*.vim", ".vimrc", "vimrc")},
    {"r",        G("*.R", "*.r", "*.Rmd")},
    {"dart",     G("*.dart")},
    {"zig",      G("*.zig")},
    {"nim",      G("*.nim", "*.nims")},
    {"crystal",  G("*.cr")},
};

#define BUILTIN_N (sizeof(builtin_types) / sizeof(builtin_types[0]))

/* User-defined types added via --type-add.  Stored separately so we
 * never need to grow the static `builtin_types` array.  Each entry's
 * `globs` is a heap-allocated NULL-terminated char ** that we own. */
static are_type_t *user_types = NULL;
static size_t      user_n     = 0;
static size_t      user_cap   = 0;

const are_type_t *
are_type_find(const char *name)
{
    for (size_t i = 0; i < BUILTIN_N; i++) {
        if (strcmp(builtin_types[i].name, name) == 0) return &builtin_types[i];
    }
    for (size_t i = 0; i < user_n; i++) {
        if (strcmp(user_types[i].name, name) == 0) return &user_types[i];
    }
    return NULL;
}

const are_type_t *
are_type_at(size_t i)
{
    if (i < BUILTIN_N)             return &builtin_types[i];
    if (i < BUILTIN_N + user_n)    return &user_types[i - BUILTIN_N];
    return NULL;
}

size_t
are_type_count(void)
{
    return BUILTIN_N + user_n;
}

bool
are_type_matches(const are_type_t *t, const char *basename)
{
    if (!t || !basename) return false;
    for (const char **g = t->globs; *g; g++) {
        if (fnmatch(*g, basename, 0) == 0) return true;
    }
    return false;
}

/* Parse `name:glob1:glob2...` and append.  Mutates a copy of `spec`. */
void
are_type_add(const char *spec)
{
    char *const dup = strdup(spec);
    char *colon = strchr(dup, ':');
    if (!colon) {
        free(dup);
        return;  /* bad spec — silently drop; caller is the CLI parser */
    }
    *colon = '\0';
    const char *name = dup;

    /* Count remaining colons to size the globs array. */
    size_t n = 1;
    for (const char *p = colon + 1; *p; p++) if (*p == ':') n++;

    char **globs = (char **)calloc(n + 1, sizeof(char *));
    char *p = colon + 1;
    size_t i = 0;
    while (p && *p) {
        char *next = strchr(p, ':');
        if (next) { *next = '\0'; next++; }
        globs[i++] = strdup(p);
        p = next;
    }
    globs[i] = NULL;

    if (user_n == user_cap) {
        user_cap = user_cap ? user_cap * 2 : 4;
        user_types = (are_type_t *)realloc(user_types, sizeof(are_type_t) * user_cap);
    }
    user_types[user_n].name  = strdup(name);
    user_types[user_n].globs = (const char **)globs;
    user_n++;

    free(dup);
}
