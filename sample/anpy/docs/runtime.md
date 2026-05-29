# AnPy ランタイム / 値表現

## 値: タグ付き `VALUE`（`intptr_t`）

| パターン | 意味 |
|---|---|
| LSB = 1 | 即値 `int`（値 = `v >> 1`、ChocoPy は 32bit なので余裕） |
| `2` / `4` / `6` | `None` / `False` / `True` |
| 8-aligned, ≠0 | ヒープオブジェクトポインタ |

即値はポインタを含まないので GC は奇数/2/4/6 のワードを無視する。ヒープは 8-aligned で
通常通り辿られる。ヒープ種別はヘッダ `enum anpy_kind`:

- `K_STR` 不変文字列（len + data）
- `K_LIST` 可変固定長リスト（len + VALUE*）
- `K_OBJ` インスタンス（class ptr + 属性スロット配列）
- `K_FUNC` クロージャ（静的記述子 + 捕捉環境）— ユーザには見えない
- `K_CLASS` クラス値（コンストラクタ）— ユーザには見えない

GC は **libgc**（Boehm）。`int`/`bool`/`None` は即値なので確保ゼロ。

## 環境とスコープ

ChocoPy はネスト関数・`global`・`nonlocal` を持つ。実装は**字句環境フレーム**:

- `anpy_cell` = 1 個の `VALUE` を箱化（共有参照）。クロージャが変更を観測できる。
- `anpy_env` = `name -> cell` のハッシュ + 親ポインタ。
- 変数読み: フレーム鎖を辿って最初の cell。
- 関数呼び出し: `anpy_invoke` が親=捕捉環境の新フレームを作り、引数→cell、`global x`→
  グローバル cell を別名束縛、`nonlocal x`→囲み cell を別名束縛、ローカル変数→新 cell
  （リテラル初期化）、ネスト関数→このフレームを捕捉したクロージャ cell、を登録して本体を実行。
- メソッドは**グローバル環境を捕捉**（仕様 §6.4: メソッドは生成時環境を捕捉しない）。

cell 共有により `nonlocal`/`global` の変更可視性が自然に出る。`c->env` が現在フレーム
（EVAL は `CTX*, NODE*` 固定シグネチャなので環境は CTX 経由で渡す）。

## クラスと動的ディスパッチ

- パース時に各クラスの own 属性/メソッドを収集。`anpy_finalize_classes` が継承チェーンを
  たどって **属性スロット**（継承込み）と**メソッド表**（オーバーライド適用）を平坦化。
- インスタンスは属性スロット配列を持つ。`o.attr` はクラスの属性名→スロットで解決。
- `o.m(...)` は `o` の動的クラスのメソッド表を引いて `o` を第1引数に渡して呼ぶ。
- `C()` はスロット確保 → 属性をリテラル初期化（グローバル環境で評価）→ `__init__` を
  ディスパッチ呼び出し。

## 非局所制御 / 実行時エラー

- `return` は `c->returning`/`c->retval` で巻き戻し、seq/ループが検査、呼び出し境界で捕捉。
- 実行時エラーは `anpy_runtime_error` が stderr に出力し `longjmp` でトップレベルへ。
  `main` が `setjmp` で囲む。

## 静的検査と実行の順序

`main`: パース → `anpy_finalize_classes` → 型検査（`--no-typecheck` で省略）→
エラーがあれば実行せず終了 → `anpy_install_globals`（クラス/関数/グローバル変数を
グローバル環境へ）→ トップレベル文を実行。

## AOT 特殊化

`--aot-compile`: トップレベル文と**各関数/メソッド本体**を `astro_cs_compile`。本体は
`EVAL(c, fn->body)` という runtime ポインタ越し dispatch なので各々を独立 entry に登録
（cf. usage.md「Entry nodes」）。SD は `anpy_*` ヘルパを外部参照するのでホスト exe は
`-rdynamic` でリンク。性能は [perf.md](perf.md)。
