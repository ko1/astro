// This file is auto-generated from node.def.
// dumpers

static void
DUMP_node_num(FILE *fp, NODE *n, bool oneline)
{
    if (oneline) {
        fprintf(fp, "(node_num ");
          fprintf(fp, "%d", n->u.node_num.num);
        fprintf(fp, ")");
  }
  else {
    // ...
  }
}

static void
DUMP_node_add(FILE *fp, NODE *n, bool oneline)
{
    if (oneline) {
        fprintf(fp, "(node_add ");
          DUMP(fp, n->u.node_add.lv, oneline);;
        fprintf(fp, " ");
        DUMP(fp, n->u.node_add.rv, oneline);
        fprintf(fp, ")");
  }
  else {
    // ...
  }
}

static void
DUMP_node_sub(FILE *fp, NODE *n, bool oneline)
{
    if (oneline) {
        fprintf(fp, "(node_sub ");
          DUMP(fp, n->u.node_sub.lv, oneline);;
        fprintf(fp, " ");
        DUMP(fp, n->u.node_sub.rv, oneline);
        fprintf(fp, ")");
  }
  else {
    // ...
  }
}

static void
DUMP_node_mul(FILE *fp, NODE *n, bool oneline)
{
    if (oneline) {
        fprintf(fp, "(node_mul ");
          DUMP(fp, n->u.node_mul.lv, oneline);;
        fprintf(fp, " ");
        DUMP(fp, n->u.node_mul.rv, oneline);
        fprintf(fp, ")");
  }
  else {
    // ...
  }
}

static void
DUMP_node_div(FILE *fp, NODE *n, bool oneline)
{
    if (oneline) {
        fprintf(fp, "(node_div ");
          DUMP(fp, n->u.node_div.lv, oneline);;
        fprintf(fp, " ");
        DUMP(fp, n->u.node_div.rv, oneline);
        fprintf(fp, ")");
  }
  else {
    // ...
  }
}

static void
DUMP_node_mod(FILE *fp, NODE *n, bool oneline)
{
    if (oneline) {
        fprintf(fp, "(node_mod ");
          DUMP(fp, n->u.node_mod.lv, oneline);;
        fprintf(fp, " ");
        DUMP(fp, n->u.node_mod.rv, oneline);
        fprintf(fp, ")");
  }
  else {
    // ...
  }
}

