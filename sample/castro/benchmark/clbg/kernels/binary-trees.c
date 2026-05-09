// Computer Language Benchmarks Game — binary-trees (single-thread, no GC).
// Stresses: malloc/free intensity, recursive traversal, pointer chasing.

void *malloc(unsigned long n);
void free(void *p);

typedef struct Tree {
    struct Tree *left;
    struct Tree *right;
} Tree;

Tree *make_tree(int depth) {
    Tree *t = malloc(sizeof(Tree));
    if (depth <= 0) {
        t->left = 0;
        t->right = 0;
    } else {
        t->left  = make_tree(depth - 1);
        t->right = make_tree(depth - 1);
    }
    return t;
}

int check_tree(Tree *t) {
    if (t->left == 0) return 1;
    return 1 + check_tree(t->left) + check_tree(t->right);
}

void free_tree(Tree *t) {
    if (t->left != 0) {
        free_tree(t->left);
        free_tree(t->right);
    }
    free(t);
}

int main() {
    int total = 0;
    int N = 16;
    int min_depth = 4;
    // Stretch tree
    Tree *stretch = make_tree(N + 1);
    total += check_tree(stretch);
    free_tree(stretch);
    // Long-lived tree
    Tree *long_lived = make_tree(N);
    // Vary depths from min_depth to N
    for (int depth = min_depth; depth <= N; depth += 2) {
        int iterations = 1 << (N - depth + min_depth);
        for (int i = 0; i < iterations; i++) {
            Tree *t = make_tree(depth);
            total += check_tree(t);
            free_tree(t);
        }
    }
    total += check_tree(long_lived);
    free_tree(long_lived);
    return total & 0xff;
}
