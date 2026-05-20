# ASTro: AST-Based Reusable Optimization Framework

著者: 笹田耕一 (ko1@st.inc, STORES株式会社)

論文:
- VMIL 2025: "ASTro: An AST-Based Reusable Optimization Framework"
- PPL 2026: "ASTro による JIT コンパイラの試作"

## 1. 問題意識

高性能なインタプリタを作るには、一般的に以下の段階を経る:

1. AST を構築し、木を辿るインタプリタで実行（簡単だが遅い）
2. バイトコードVM を作る（性能向上するが開発コスト大）
3. JIT コンパイラを追加（さらに高速だが複雑）

各段階で VM の設計・JIT バックエンドなど言語ごとに専用の実装が必要。
Truffle/Graal や RPython は高性能だが、重量級ツールチェインに依存し、ポータビリティやデバッグ性に難がある。

## 2. ASTro の核心アイデア

### 2.1 部分評価 + C コンパイラ

- インタプリタの **部分評価 (Partial Evaluation)** の結果を **C のソースコード** として出力
- 汎用の C コンパイラ（gcc, clang 等）でコンパイルすることで、高品質なネイティブコードを得る
- C コンパイラは成熟した最適化基盤であり、新たなバックエンド開発が不要
- 第一二村射影: インタプリタ I をプログラム P に対して部分評価すると、P の残余プログラム PE(I, P) が得られる

### 2.2 ディスパッチャとエバリュエータの分離

ASTro の最も重要な設計判断:

- **エバリュエータ (EVAL_xxx)**: ノードの評価ロジック本体。ユーザが node.def に記述
- **ディスパッチャ (DISPATCH_xxx)**: ノードのフィールドを取り出してエバリュエータに渡す薄い関数。ASTroGen が自動生成

この分離により:
- 部分評価は **ディスパッチャだけを特化** すれば良い（エバリュエータは変更不要）
- 特化ディスパッチャは具体的な関数ポインタを埋め込むため、C コンパイラがインライン展開可能
- ユーザ定義のエバリュエータに手を加えずに最適化が実現

例: `1 + 2 * 3` の AST を特化すると、最外部のディスパッチャは `mov $0x7, %eax; ret` にまでインライン化される。

### 2.3 Merkle ツリーハッシュ

- 各 AST ノードに Merkle ツリーハッシュを付与
- ハッシュは (ノードの種類, 各属性のハッシュ) から計算（子ノードは再帰的）
- 同一構造の部分木は同一ハッシュ → **プロセス間・マシン間でコンパイル結果を共有可能**
- 特化ディスパッチャの関数名は `SD_<hash>` 形式

## 3. ASTroGen ツール

`lib/astrogen.rb` (約 780 行の Ruby スクリプト) が、`node.def` から以下を自動生成:

| 生成ファイル | 内容 |
|---|---|
| `node_head.h` | AST ノードの構造体定義（union でまとめる）、関数ポインタ typedef、NodeKind 構造体 |
| `node_eval.c` | EVAL_xxx 関数（ユーザ定義のロジックをそのまま出力） |
| `node_dispatch.c` | DISPATCH_xxx 関数（フィールド抽出 → EVAL呼び出し） |
| `node_hash.c` | HASH_xxx 関数（Merkle ツリーハッシュ計算） |
| `node_alloc.c` | ALLOC_xxx 関数 + NodeKind 定義 |
| `node_specialize.c` | SPECIALIZE_xxx 関数（特化Cコード生成=部分評価器） |
| `node_dump.c` | DUMP_xxx 関数（デバッグ用ノード表示） |
| `node_replace.c` | REPLACER_xxx 関数（子ノード差し替え） |

生成タスクは `register_gen_task` で登録されており、サブクラスでカスタムタスク（例: GC マーク関数）を追加可能。

### 3.1 node.def の形式

```c
NODE_DEF [@option]
node_name(CTX *c, NODE *n, type1 operand1, type2 operand2, ...)
{
    // C code for evaluation
    // EVAL_ARG(c, child_node) で子ノードを評価
}
```

- 最初の2引数 `CTX *c, NODE *n` は必須（コンテキストとノード自身）
- `NODE *` 型のオペランドは子ノード（lazy: body が `EVAL_ARG` で評価する）
- `VALUE <name>@child` で **strict 引数化** — DISPATCH が子を評価して VALUE
  として渡す。snapshot 場所は `Node#child_storage_expr` で言語ごとに選択可
  (デフォルト `sp[i]`、libgc 系言語なら C ローカルに変更可)
- `@noinline` オプションで特化時のインライン抑制が可能
- オペランド名末尾に `@ref` を付けると、ノード本体に値を埋め込まずポインタ経由で扱う（インラインキャッシュ等のミュータブルな副情報用）

### 3.2 サポートするオペランド型

- `NODE *` — 子 AST ノード（特別扱い: ディスパッチャ経由の呼び出し）
- `int32_t`, `uint32_t` — 整数フィールド
- `const char *` — 文字列フィールド
- その他ユーザ定義型（ハッシュ関数のカスタマイズが必要な場合あり）

### 3.3 NodeHead 構造

全ノードは共通ヘッダ `NodeHead` を持つ:

```c
struct NodeHead {
    struct NodeFlags {
        bool has_hash_value;
        bool has_hash_opt;          // PGC: hash_opt が確定している
        bool is_specialized;
        bool is_specializing;       // 再帰的特化防止
        bool is_dumping;            // 再帰的ダンプ防止
        bool no_inline;             // 特化時のインライン抑制
    } flags;
    const struct NodeKind *kind;
    struct Node *parent;
    node_hash_t hash_value;             // Horg: 構造ハッシュ (AOT)
    node_hash_t hash_opt;               // Hopt: profile-baked ハッシュ (PGC)
    const char *dispatcher_name;
    node_dispatcher_func_t dispatcher;  // 関数ポインタ
    enum jit_status { ... } jit_status; // JIT 状態管理
    unsigned int dispatch_cnt;          // ディスパッチ回数
    int line;                           // 診断用ソース行番号
};
```

各ノード型は `NodeKind` 構造体を持ち、デフォルトディスパッチャ・ハッシュ関数・特化関数・ダンプ関数・子ノード差し替え関数へのポインタを格納。`NodeKind` の構造体定義は `node_head.h` に自動生成される。

## 4. 4つの利用モード

### 4.1 Plain Interpreter（ベースラインインタプリタ）

- 部分評価を使わず、DISPATCH → EVAL の木巡回インタプリタとして実行
- node.def さえあれば動作する最もシンプルな形態

### 4.2 AOT Compiler（事前コンパイル）

- パース後、AST を部分評価器に渡して特化 C コードを生成
- C コンパイラでコンパイルし、特化インタプリタ or スタンドアロン実行ファイルを生成
- コンパイル時間は含まないが、実行は高速

### 4.3 Profile-Guided (PG) Compilation

- 1回目の実行でプロファイル情報を収集
- 2回目以降、プロファイル情報に基づいて特化
- call サイトのインラインキャッシュ情報を特化器に渡せる（node_call2）

### 4.4 JIT Compiler（実行時コンパイル）

実行中に C コンパイラを呼び出して特化コードを動的にロード。

## 5. ASTro JIT の設計

### 5.1 階層型キャッシュアーキテクチャ (L0/L1/L2)

```
Execution Machine                    Compile Machine
┌─────────────────────┐             ┌──────────────────┐
│ Interpreter Process │             │                  │
│ ┌───────┐ ┌──────┐ │             │  Remote code     │
│ │Interp │ │ L0   │ │   L1        │  store           │
│ │Thread │ │Thread│ ├────(Unix)───┤                  │
│ └───────┘ └──────┘ │   socket    │  ┌────┐ ┌────┐  │
│                     │             │  │ L2 │ │ L2 │  │
│ Local code store    │             │  └────┘ └────┘  │
│ H→code mapping      │             │  C compilers     │
└─────────────────────┘             └──────────────────┘
```

- **L0**: インタプリタプロセス内のスレッド (C, Pthread)
  - インタプリタスレッドから同期キューでリクエストを受ける
  - 非同期にディスパッチ関数を差し替え
  - ローカルコードストアを参照
- **L1**: 同一マシン上のデーモンプロセス (Ruby)
  - Unix domain socket で L0 と通信
  - TCP で L2 と通信
  - 複数の L0 と 1つの L2 に接続
- **L2**: コンパイル用マシン上のデーモン (Ruby)
  - ソースコードを受け取り、C コンパイラでコンパイル
  - 複数の L1 と通信可能
  - L1 と L2 は同一マシンでも別マシンでもよい

### 5.2 通信メッセージ

2種類:

- **query(h)**: ハッシュ h に対応するコンパイル済みネイティブコードがあるか問い合わせ
- **compile(h, src)**: ハッシュ h に対応するソースコード src のコンパイルを要求

メッセージ形式: (種類, サイズ, ハッシュ値, ペイロード) の4つ組バイナリプロトコル。

### 5.3 AST の状態遷移

各ノードは以下の状態を持つ:

```
Unknown → Querying → NotFound → Compiling → Compiled
                   ↘ (found)→ Compiled
```

- **Unknown**: まだ query していない
- **Querying**: query 送信済み、返答待ち
- **NotFound**: コンパイル済みコードが存在しない
- **Compiling**: compile 送信済み、返答待ち
- **Compiled**: コンパイル済みネイティブコードあり

### 5.4 コードストアの実装

ファイルシステム + 共有オブジェクト (.so) ベース:

- **ローカルコードストア** (L0, L1): `<hash>.so` ファイル。`dlsym()` で `SD_<hash>` 関数を検索
- **リモートコードストア** (L2): `<hash>.o` ファイル。なければ C コンパイラで生成
- **all.so**: 複数の .o をリンクしてまとめた共有オブジェクト。L0 起動時にまずこれをロード

効率化: L1 が暇なタイミングで既存の .o を all.so にまとめてリンカで生成。

### 5.5 JIT トリガー条件

- プログラムロード時: 各 AST について `query(h)` を発行（既存キャッシュの利用）
- ある関数を100回以上実行したとき: `compile` メッセージで特化を要求

## 6. サンプル言語

ASTro は `sample/` 以下に **17 サンプル** を抱えており、教育用の最小例から
本格的な動的言語、関数型言語、スタックマシン、DSL までを横断的にカバー
する。サンプル横断の比較は [`samples.md`](./samples.md) に集約してあるので、
本節では論文評価で使った **naruby** を代表として要約する。各サンプルの
実装詳細・性能数値は `sample/<lang>/README.md` と
`sample/<lang>/docs/{done,todo,perf,runtime}.md` に。

### 6.1 naruby — 論文評価用 Ruby サブセット

"Not A Ruby" — Ruby の文法だが機能を大幅に制限。`node.def` は約 570 行、
36 ノード型。1 バイナリで 4 つの実行モード (interpret / AOT / PG / JIT)
を切り替えられる、フレームワーク自身の評価用言語。

主なノード分類:

| カテゴリ | ノード |
|---|---|
| リテラル | `node_num` (整数) |
| 制御フロー | `node_seq`, `node_if`, `node_while` |
| 変数 | `node_scope`, `node_lget`, `node_lset` |
| 関数 | `node_def`, `node_call`, `node_call2`, `node_call_static`, `node_call_builtin` |
| 二項演算 | `node_add`, `node_sub`, `node_mul`, `node_div`, `node_mod` |
| 比較 | `node_eq`, `node_neq`, `node_lt`, `node_le`, `node_gt`, `node_ge` |

### 6.2 naruby ランタイム

- 値の型は符号付き整数のみ
- バリュースタック方式（固定サイズフレーム）
- グローバル関数テーブル (name, arity, AST) の3つ組
- インラインキャッシュ: function-table version でキャッシュ有効性を判定
- フロントエンド: Prism パーサ（Ruby 標準パーサ）を利用し、ALLOC_* で AST を構築

### 6.3 ベンチマーク (VMIL2025 当時, x86_64)

| 構成 | loop | fib | call | prime_count |
|---|---|---|---|---|
| naruby/interpret | 0.786 | 4.870 | 6.760 | 6.170 |
| naruby/compiled (AOT) | 0.001 | 1.093 | 3.435 | 0.444 |
| naruby/pg (Profile-Guided) | 0.001 | 1.143 | 2.061 | 0.443 |
| gcc -O0 | 0.042 | 0.480 | 1.121 | 0.490 |
| gcc -O2 | 0.001 | 0.115 | 0.318 | 0.434 |

AOT コンパイルで gcc -O0 に迫る性能。loop ベンチマークではループ自体が最適化で消える。

### 6.4 JIT 予備評価 (PPL2026, prime? ベンチマーク)

| 条件 | 実行時間(秒) |
|---|---|
| JIT なし | 13.64 |
| JIT あり（初回） | 1.11 |
| JIT あり（2回目, キャッシュ済み） | 1.05 |

### 6.5 他のサンプル

| sample | 概要 |
|---|---|
| `calc` | 6 ノードの最小チュートリアル |
| `abruby` | CRuby C 拡張版 Ruby サブセット (VALUE / Prism / GC を流用) |
| `koruby` | スタンドアロン Ruby、**optcarrot 完走** |
| `luastro` / `jstro` / `pystro` | Lua 5.4 / JS ES2023 / Python 3 サブセット |
| `ascheme` / `astocaml` / `asom` | R5RS Scheme / OCaml サブセット / SOM Smalltalk |
| `pascalast` / `castro` | Pascal / C サブセット (静的型) |
| `aforth` / `wastro` | Forth / WebAssembly 1.0 (スタックマシン) |
| `astr` | R サブセット (vectorized) |
| `astrogre` / `nuq` | DSL: 正規表現エンジン / `jq` クローン |

横断分析と性能ハイライトは [`samples.md`](./samples.md) §1 の言語ラインナップ表を参照。

## 7. ASTro Code Store

特化コードの保存・ロードを統一的に扱うランタイムライブラリ。`runtime/astro_code_store.{h,c}` として提供。

### 7.1 設計方針

- **保存単位**: エントリノード（関数定義のボディ等）のサブツリー丸ごと。1エントリにつき1つの `.c` ファイル
- **エントリの選択**: 言語側が決める（メソッド単位、トップレベルスクリプト等）
- **保存形式**: C ソース + コンパイル済み共有オブジェクト (`.so`)
- **ロード方式**: `all.so`（全エントリをまとめたもの）を `dlopen` + `dlsym("SD_<hash>")`

### 7.2 API

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

### 7.3 利用フロー

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

### 7.4 モード別の使い分け

| モード | compile | build | load |
|--------|---------|-------|------|
| AOT | 実行後にオフライン | 同左 | 次回起動時 |
| PG | 1回目実行後 (Hopt 込み) | 同左 | 2回目起動時 (file 引数あり) |
| JIT | 実行中に非同期 | バックグラウンドで適宜 | コンパイル完了時に reload |

JIT の場合は `all.so` に加え、新規コンパイル分を個別 `.so` として `dlopen` することも可能。

### 7.5 ファイル構成

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

### 7.6 言語側の統合

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

## 8. 既知の課題と今後の方向

実装状況は VMIL2025 から PPL2026 (JIT 試作) を経て、本書執筆時点 (2026 春)
までで進んだ部分・未踏部分が混在している。以下は **現状のステータス**
と **未踏領域** を区別して整理。

### 8.1 進捗があった項目

#### JIT 階層 (L0/L1/L2) — 試作実装済み
- naruby で L0/L1/L2 すべての層を実装し、PPL2026 で試作評価を発表。
- prime? ベンチで JIT-on 1.11s vs JIT-off 13.64s (12× speedup)、キャッシュヒット時は 1.05s。
- L1 デーモン (Ruby) の Unix socket / TCP プロトコルが実用域。

#### AOT + JIT の併用
- naruby は AOT (PGC) + JIT を共存。koruby / pystro 等は AOT 中心で
  起動時に既存 SD をロードして使う運用が確立。
- 初回ロード時バーストを AOT が吸収、ホットパスを JIT が再最適化、という構図は naruby で実証済み。

#### 例外処理
- jstro (longjmp `throw`)、pystro (`try`/`except`/`else`/`finally`/`raise`)、pascalast (catchable `try/except/finally`)、astocaml (例外) がそれぞれ別方式で実装済み。
- 結論: setjmp/longjmp は C で十分扱える。EVAL 本体は触らず、parser-pass + ranges リスト方式で意外と綺麗に書ける ([perf.md](./perf.md) の各サンプル節参照)。

#### 静的型サンプル
- pascalast (Pascal)、castro (C subset)、astocaml (OCaml subset)、wastro (Wasm 1.0) が静的型側を埋めた。
- 結論: 静的型情報を活かした **算術ノードの per-type 分裂** + AOT が、`fpc -O3` 等のネイティブコンパイラと張り合えるケースが出ている (pascalast のタイト数値ループ)。

### 8.2 未踏 / 検討中

#### 独自ローダ
- 現在は .so ファイルベースで、ページアライメントにより小さな関数でもメモリ浪費。
- 専用のネイティブコードローダ (copy & patch + ELF relocation) で密に mmap する設計が候補。CPython JIT (PEP 744) 同様の方向。
- 計算機環境ごとの実装が必要で、ポータビリティとのトレードオフ。code_store_quirks.md の 3 つの罠 (パス名キャッシュ・atomic rename・dlclose 不能) もこれで一掃される。

#### コードストア上限サイズ
- 現在は生成コードを削除しない → 実運用では LRU 等で上限管理が必要。

#### 投機的プリフェッチ
- L2 への query が h1, h2, h3... と連続する場合、h4, h5... を投機的にプリフェッチ可能。

#### コードサイズ膨張
- 部分評価を無差別に適用するとコードサイズが爆発。
- ハッシュ関数のカスタマイズ (`@ref` で副情報をハッシュから外す等) で制御可能。
- 静的型サンプル (pascalast / wastro) は per-type 分裂でノード数が増えやすく、ここの管理が今後も課題。

#### 統一 GC 基盤
- 現状は libgc (Boehm) ベースが多数派 (koruby / asom / astr / astocaml 等)、自前 mark-sweep が一部 (luastro)、CRuby GC 流用 (abruby) と分かれている。
- pluggable GC (例: Bartlett mostly-copying) を `value.def` + frame iterator + AST 不動を前提に共通化する設計案が検討中 (本書では割愛)。
