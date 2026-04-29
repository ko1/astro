struct point {
    int x;
    int y;
};

int dist_sq(struct point *p) {
    return p->x * p->x + p->y * p->y;
}

int main() {
    struct point a;
    a.x = 3;
    a.y = 4;
    printf("a=(%d,%d) d2=%d\n", a.x, a.y, dist_sq(&a));

    struct point b = {5, 12};
    printf("b=(%d,%d) d2=%d\n", b.x, b.y, dist_sq(&b));

    // pointer to struct
    struct point *p = &b;
    p->x = 8;
    printf("b after = (%d,%d)\n", b.x, b.y);
    return 0;
}
