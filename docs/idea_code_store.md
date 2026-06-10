# ASTro Code Store

特化コードの保存・ロードを統一的に扱うランタイムライブラリ。`runtime/astro_code_store.{h,c}` として提供。
「ハッシュでコードを一意に特定する」という核心アイデア ([idea.md](./idea.md) §2.4–2.5) の実装基盤であり、
AOT / PG / JIT ([idea_jit.md](./idea_jit.md)) すべてがこの上に乗る。

## 1. 設計方針

- **保存単位**: エントリノード（関数定義のボディ等）のサブツリー丸ごと。1エントリにつき1つの `.c` ファイル
- **エントリの選択**: 言語側が決める（メソッド単位、トップレベルスクリプト等）
- **保存形式**: C ソース + コンパイル済み共有オブジェクト (`.so`)
- **ロード方式**: `all.so`（全エントリをまとめたもの）を `dlopen` + `dlsym("SD_<hash>")`

## 2. API

実体は `runtime/astro_code_store.h`。

```c
// 初期化: store_dir/all.so があれば dlopen 済みにする。
//   src_dir : node.h / node_eval.c 等の場所 (生成 .c が #include する)
//   version : ホストバイナリの mtime 等。前回保存時と異なれば store を破棄
void astro_cs_init(const char *store_dir, const char *src_dir, uint64_t version);

// hash 検索 → ヒットすれば dispatcher を差し替え true。
//   file != NULL : PGC ルックアップ (Hopt) を先に試す
//   file == NULL : AOT のみ (Horg)
bool astro_cs_load(NODE *n, const char *file);

// 特化 C ソースを生成。
//   file == NULL : AOT — store_dir/c/SD_<Horg>.c
//   file != NULL : PGC — store_dir/c/SD_<Hopt>.c + hopt_index.txt 追記
void astro_cs_compile(NODE *entry, const char *file);

// store_dir 配下の全 .c を make -j → all.tmp.so → all.so atomic rename。
//   extra_cflags : Ruby 等の追加 -I/-D が必要なら渡す。NULL でも可
void astro_cs_build(const char *extra_cflags);

// build 後に呼ぶと all.<N>.so を新世代パスで dlopen し直す
// (dlopen のパス名キャッシュ回避; code_store_quirks.md 罠 1 参照)
void astro_cs_reload(void);

// 診断: 特化済みノードの dispatcher を objdump で逆アセンブル表示
void astro_cs_disasm(NODE *n);
```

## 3. 利用フロー

```
[1回目の実行]
  astro_cs_init("code_store", ".", VERSION)   all.so がない → load は miss
  ALLOC → astro_cs_load (miss)
  ... 実行 ...
  astro_cs_compile(entry, NULL)                code_store/c/SD_<Horg>.c 生成
  astro_cs_build(NULL)                         全 .c → .o → all.so
  astro_cs_reload()                            新世代 all.<N>.so を dlopen
  astro_cs_load(entry, NULL)                   hit → dispatcher 差し替え

[2回目の実行]
  astro_cs_init(...)                           前回の all.so をそのまま再利用
  ALLOC → astro_cs_load (hit) → 高速実行
```

## 4. モード別の使い分け

| モード | compile | build | load |
|--------|---------|-------|------|
| AOT | 実行後にオフライン | 同左 | 次回起動時 |
| PG | 1回目実行後 (Hopt 込み) | 同左 | 2回目起動時 (file 引数あり) |
| JIT | 実行中に非同期 | バックグラウンドで適宜 | コンパイル完了時に reload |

JIT の場合は `all.so` に加え、新規コンパイル分を個別 `.so` として `dlopen` することも可能。

## 5. ファイル構成

```
code_store/
  c/SD_<hash1>.c     ← 特化 C ソース
  c/SD_<hash2>.c
  o/SD_<hash1>.o     ← コンパイル済みオブジェクト
  o/SD_<hash2>.o
  all.so             ← 全 .o をまとめた共有オブジェクト (atomic mv で更新)
  all.<N>.so         ← reload 用の世代別ハードリンク
  Makefile           ← astro_cs_build が自動生成
  hopt_index.txt     ← PGC: (Horg, file, line) → Hopt のマップ
```

各 `.c` ファイルはサブツリーの特化コードを自己完結で含む。共通する部分木は複数ファイルに重複するが、`static` 関数のためリンク問題は起きない。LTO で重複を最適化することも可能。

dlopen / リンクまわりの罠と暫定対処は [`code_store_quirks.md`](./code_store_quirks.md) を参照。

## 6. 言語側の統合

calc など最小サンプルの REPL は、入力ごとにこう呼ぶ:

```c
NODE *ast = parse(line);
if (!ast->head.flags.is_specialized) {
    astro_cs_compile(ast, NULL);
    astro_cs_build(NULL);
    astro_cs_reload();
    astro_cs_load(ast, NULL);
}
EVAL(c, ast);
```

JIT を持つサンプル (naruby) では、ホットノード検出時に L0 スレッド経由で
`astro_cs_compile` → `astro_cs_build` → `astro_cs_reload` をバックグラウンドで実行し、
完了後に `astro_cs_load` を呼ぶ。code store はハッシュ→dispatcher マップの管理と
`.so` のロードに専念し、「何をエントリとするか」「いつコンパイルするか」の
ポリシーは言語側に委ねる。
