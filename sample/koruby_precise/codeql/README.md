# koruby_precise — CodeQL GC-effect / borrow checks (試作)

`docs/c_ext_api_design.md` §4.1 の静的検査(may-gc 推論 + borrow 寿命)を
CodeQL で試すための最小一式。

## 変更後に走らせるゲート

```sh
make codeql-check      # = sh codeql/run.sh
```

2 フェーズ、両方 pass しないと非ゼロ終了:
1. **self-test** — `test/borrow_cases.c` から DB を作り、クエリが**その 2 バグを
   ちょうど検出**することを要求(クエリが「常に 0」に退化していないか保証)。
2. **real-code** — koruby のビルドをトレースして DB を作り、`borrow_after_gc.ql`
   が **0 hazard** であることを要求。

DB は `codeql/.db/`(gitignore 済)に作る。CodeQL CLI(`gh extension install
github/gh-codeql`)+ `codeql pack install`(run.sh が自動)が要る。フル DB 再構築
を含むので毎ビルドでなく**変更後に明示的に**回す想定(所要 ~2〜3 分)。

## クエリ

- `maygc.ql` — 検証用。単一 seed `korb_alloc` から直接コールグラフの推移閉包で
  「may-gc 関数」を列挙(相互再帰は QL の fixpoint で解ける)。
- `borrow_after_gc.ql` — 本命。`str->buf->data`(= `KorbStrBuf::data`、移動する
  文字列バイトバッファへの生ポインタ)を borrow-source とし、**borrow → may-gc call
  → use** の経路(`DataFlow::localFlow` + CFG 順)を検出。re-derive(途中で取り直し)
  すると flow が届かず自動的に安全判定になる。

## セットアップ(このマシンでの実績)

```sh
# CodeQL CLI(gh 経由)。ssh 鍵問題があれば拡張は HTTPS で直接 clone:
git clone --depth 1 https://github.com/github/gh-codeql.git \
  ~/.local/share/gh/extensions/gh-codeql
gh codeql version                      # 初回に CLI 本体(~525MB)を DL/展開
CQ=~/.local/share/gh/extensions/gh-codeql/dist/release/v2.26.2/codeql

# 標準 cpp ライブラリ(codeql/cpp-all ほか)を取得
( cd codeql && "$CQ" pack install )
```

## DB 作成(ビルドをトレース)

ccache を無効にして全 TU を確実に抽出する:

```sh
"$CQ" database create /path/to/koruby-db --language=cpp --overwrite \
  --command="bash -c 'make clean; CCACHE_DISABLE=1 make -j8 koruby_precise'"
```

## クエリ実行

```sh
"$CQ" query run --database=/path/to/koruby-db codeql/maygc.ql          # 検証
"$CQ" query run --database=/path/to/koruby-db codeql/borrow_after_gc.ql
```

## 実測(2026-08-05, Ryzen 9 5900HX, CodeQL 2.26.2)

DB スコープ: **72,528 LoC**(koruby_precise 49,868 + 共通 runtime 2,436 +
system/GMP 等ヘッダ 17,399)、本体を持つ関数 **2,997**、呼び出し **28,831**。
`#include` した libc/GMP ヘッダも DB に入るが大半は宣言(本体は koruby+runtime)。

- `maygc.ql`: **1,474 関数**を may-gc と推論(単一 seed `korb_alloc` からの推移
  閉包)。直接コールグラフに対し健全かつ完全、eval **2.2s**。
- `borrow_after_gc.ql` の精度改善(3 段階):

  | 版 | 時間 | メモリ | alert | 問題 |
  |---|---|---|---|---|
  | ① 命令レベル二重 `getASuccessor+()` | >15分で kill | — | (kill) | 重すぎ |
  | ② BB 到達 + astro 限定 | 97.7s | 5.68GB | 3,516 | ループ re-derive を FP |
  | ③ SSA + held-in-pointer | **23.4s** | 2.51GB | 48 | `ncp=strlen(borrow)` 等を FP |
  | ④ ③ + borrow-flows(変換/ポインタ演算のみ) | **~23s** | 2.5GB | **0** | — |

  精度と速度は一致した(シード集合が「全 `->buf->data`」→「ポインタ変数に保持した
  borrow」に激減 → 探索空間縮小)。

  **最終形 ④ の設計**:
  - **SSA reaching-def**: re-derive(`p = s->buf->data` 再実行)は別 SSA def に
    なるので、ループ内 re-derive は自動で安全判定。
  - **def ノード再通過 barrier**: ループ back-edge で同じ def に戻る=re-derive
    なので staleness をリセット(②→③ で残ったループ FP をここで解消)。
  - **held-in-pointer 限定**(`borrowFlowsTo`): 変数がポインタ型で、借用が
    **変換 / ポインタ演算のみ**を介して流れる場合だけ borrow-source とする。
    `int n = utf8len(s->buf->data)`(引数、戻りは count)や `char c = data[i]`
    (添字、値は 1 バイト)は借用の保持ではないので除外 → ③の 48 FP が消える。

**borrow-source は 2 payload**(context.h の flexible-array 全て):
`KorbStrBuf.data`(char*, 文字列バイト)+ `KorbArrayItems.data`(VALUE*,
Array/Hash 要素、`ary->items->data`)。それ以外の struct フィールドは
`VALUE ARO_GC_EDGE`(VALUE_REF/edge 管理、生ポインタでない)。

**結果(2026-08-05)**: 本物 koruby に held-borrow(ポインタ変数保持)は
**44 箇所**(KorbStrBuf 28 + KorbArrayItems 16)存在し、そのうち **may-gc を
跨ぐ stale-borrow ハザードは 0**。検証: `array_enum.c` の held `VALUE *dd0 =
d->items->data` は GC-free path(Fixnum qsort)限定で使われ、`<=>` dispatch
(may-gc)path は inline で re-derive している(コメントにも明記)→ 0 は正しい。
フィクスチャ(`test/borrow_cases.c`)で **2 TP(linear hold / loop-carried hold)・
3 TN(use-before-gc / no-gc-between / re-derive-loop)** を正しく判定。→ クエリは
非 vacuous かつ精密で、現行 koruby はこのハザード種別についてクリーン(rooting
規律 + RESULT_AUDIT の成果)。

## 現状 → 本設計への橋渡し

いまの borrow-source は「`KorbStrBuf::data` への FieldAccess」を構造マッチしている
(現行コードに `korb_str_data` アクセサ関数はまだ無いため)。ext API 導入後は、
公開アクセサ(`korb_str_data` 等)+ `ARO_NOGC`/`ARO_MAYGC` 属性(`runtime/aro_gc_effect.h`)
を borrow-source / may-gc seed に切り替える。属性は gcc ビルドを壊さないよう
`MacroInvocation` 照合で拾う(詳細は `docs/c_ext_api_design.md` §4.1)。
