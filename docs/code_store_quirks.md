# ASTro Code Store — 実装上の罠メモ

ASTro の code store (`runtime/astro_code_store.c`) は runtime で `SD_<hash>.c` を
吐き → `make` で `all.so` にリンク → `dlopen` + `dlsym` で dispatcher を差し替える、
という仕組み。この際に踏んだ罠と暫定対処を残しておく。将来「自前ローダー」
(todo で検討中の copy & patch 方式) に置き換わるとほとんど不要になる。

## 罠 1: `dlopen(3)` はパス名キャッシュ

glibc の `dlopen` は、既にロード済みの共有オブジェクトをパス名で識別する。

```c
h1 = dlopen("/tmp/lib.so", RTLD_LAZY);
// ディスク上の /tmp/lib.so を mv で別ファイルに置き換え (inode は変わる)
h2 = dlopen("/tmp/lib.so", RTLD_LAZY);
// h1 == h2.  新しい /tmp/lib.so のシンボルは dlsym で見えない。
```

`man dlopen` には "If the object is already loaded ... the same handle is
returned" とだけ書いてあり inode ベースに見えるが、実装 (glibc `elf/dl-open.c`,
`elf/dl-load.c`) は `l_name` / `l_libname` — つまりパス名を優先して突き合わせる。
inode チェックは `RTLD_NOLOAD` 等の限定的な経路でしか走らない。

再現:
```sh
cat > dlt.c <<'EOF'
#include <dlfcn.h>
#include <stdio.h>
int main() {
    void *h1 = dlopen("/tmp/lib.so", RTLD_LAZY);
    getchar(); // ここで /tmp/lib.so を新しい .so で置き換える
    void *h2 = dlopen("/tmp/lib.so", RTLD_LAZY);
    printf("same=%d\n", h1 == h2); // → same=1
}
EOF
```

**暫定対処** (`astro_cs_reload`): リロードごとに `all.<N>.so` (`N` は世代
カウンタ) というユニークなパス名でハードリンク (or cp) してから `dlopen`。
古いハンドルは dlclose しない — 既存ノードが関数ポインタを持っている。

副作用: `all.<N>.so` がプロセス寿命中累積する。起動時 (`astro_cs_init`) に
`all.[0-9]*.so` を掃除して持ち越さない。

## 罠 2: `all.so` を上書き中に読まれる可能性

`make` のデフォルトレシピ `$(CC) -shared -o all.so $^` はリンク中のファイルが
他プロセス (or 他スレッド) から途中状態で見える。`dlopen` された瞬間に
リンクが終わっていないと壊れたハンドルになる。

**暫定対処** (`astro_cs_build`): `all.tmp.so` にリンクし、終わったら `mv` で
`all.so` に atomic rename。リンク中の不完全な `all.so` は決して見えない。

## 罠 3: ノードは古い dlopen ハンドルを参照し続ける

`swap_dispatcher` で AST ノードの `dispatcher` を書き換えた後、そのポインタは
特定の `all.<N>.so` の内部を指している。その .so を dlclose してしまうと
ノードの次回 EVAL が無効ポインタ呼び出しになる。

**暫定対処**: dlclose しない。マップは process 終了まで残す。`all.<N>.so`
世代別ファイル方式とも整合する。

## なぜ "自前ローダー" で解消するのか

上記 3 件は全部 "共有ライブラリ" の枠にこだわるから起きる:

- パス名キャッシュ → ELF relocation を自前で解決すれば回避
- `all.so` の atomic 差し替え → そもそも中間ファイルがない
- dlclose できない → ページ単位で個別に mmap しておけば不要部分だけ
  munmap できる

todo (sample/abruby/docs/todo.md §ノードフィールドのロード時解決) にある
"copy & patch + ELF relocation" 方式はこれらを一掃するので、ASTro として
中長期的にはそちら。それまでは本メモの暫定対処で運用する。

## 参考

- glibc `elf/dl-open.c` / `elf/dl-load.c` — 実装
- CPython JIT (PEP 744): 同じ理由で自前ローダーを選択している
- OpenJ9 Shared Classes Cache: AOT コードの validation + relocation を 2 フェーズロード
