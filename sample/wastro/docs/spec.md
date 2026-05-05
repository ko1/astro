# wastro 言語仕様

`wastro` は **WebAssembly 1.0 (MVP) + 一部 wasm 2.0** の実装。WebAssembly
(Wasm) は Web ブラウザや組込みでの実行を想定したスタックマシンの命令セット
で、ソース言語ではなく中間表現/アセンブラに近い性質を持つ。wastro は
`.wat` (テキスト形式) と `.wasm` (バイナリ形式) を両方読み込み、ツリー
ウォーカで実行する。

完全な仕様は [WebAssembly Core Specification](https://webassembly.github.io/spec/core/)
を参照。本書は wastro が動かす範囲を端的に示す。

## 用語

- **モジュール (module)**: Wasm の翻訳単位。関数・メモリ・テーブル・グローバル変数を 1 つのまとまりにする。
- **関数 (function)**: 引数を取り戻り値を返す手続き。Wasm の最小実行単位。
- **スタックマシン**: 計算を **値スタック** で行う実行モデル。命令は「スタックから取って計算してスタックに積む」を繰り返す。
- **線形メモリ (linear memory)**: モジュールが使える 1 本のバイト配列 (heap 相当)。
- **テーブル**: 関数参照などの索引付きリスト。間接呼出に使う。
- **WAT**: Wasm Text Format。S 式風の人間可読形式 (`.wat`)。
- **WASM**: Wasm Binary Format。コンパクトなバイナリ形式 (`.wasm`)。

## 値の型 (4 つだけ)

| 型 | サイズ | 意味 |
|---|---|---|
| `i32` | 32-bit | 整数 (符号は命令側で決まる) |
| `i64` | 64-bit | 整数 |
| `f32` | 32-bit | IEEE-754 single |
| `f64` | 64-bit | IEEE-754 double |

(`v128` SIMD・参照型は wasm 2.0+。wastro は未対応。)

## モジュールの基本形 (WAT)

WAT は S 式 — `(...)` でネストする。最小例:

```wat
(module
  (func $fib (export "fib") (param $n i32) (result i32)
    (local.get $n)
    (i32.const 2)
    (i32.lt_s)
    (if (result i32)
      (then (local.get $n))
      (else
        (i32.add
          (call $fib (i32.sub (local.get $n) (i32.const 1)))
          (call $fib (i32.sub (local.get $n) (i32.const 2)))))))
)
```

- `(module ...)` がトップレベル
- `(func $name ...)` で関数定義
- `(param $x i32)` `(result i32)` で型シグネチャ
- `(local $tmp i32)` でローカル変数
- `(export "name")` で外部から呼べるようにする

### Folded vs Stack-style

WAT には 2 つの書き方があり、混在も可能:

```wat
;; folded (前置 S 式)
(i32.add (local.get $x) (i32.const 1))

;; stack-style (本来の Wasm の見た目)
local.get $x
i32.const 1
i32.add
```

両方とも同じ命令列に展開される。

## 算術命令

各型ごとに専用命令。`i32.add` と `i64.add` は別命令。

### 整数 (`i32` / `i64`)

| 命令 | 意味 |
|---|---|
| `i32.add  i32.sub  i32.mul` | 加減乗 (オーバーフローは wraparound) |
| `i32.div_s  i32.div_u` | 符号付き / 符号なし除算 |
| `i32.rem_s  i32.rem_u` | 剰余 |
| `i32.and  i32.or  i32.xor` | ビット論理 |
| `i32.shl  i32.shr_s  i32.shr_u` | シフト |
| `i32.rotl  i32.rotr` | 回転 |
| `i32.clz  i32.ctz  i32.popcnt` | leading 0 / trailing 0 / 立ってるビット数 |
| `i32.eqz` | == 0 ? |

### 浮動小数 (`f32` / `f64`)

| 命令 | 意味 |
|---|---|
| `f64.add  f64.sub  f64.mul  f64.div` | 算術 |
| `f64.abs  f64.neg  f64.sqrt` | |
| `f64.ceil  f64.floor  f64.trunc  f64.nearest` | 丸め |
| `f64.min  f64.max  f64.copysign` | |

### 比較

```
i32.eq   i32.ne   i32.lt_s  i32.lt_u  i32.gt_s  i32.gt_u  i32.le_s  i32.le_u  i32.ge_s  i32.ge_u
f64.eq   f64.ne   f64.lt    f64.gt    f64.le    f64.ge
```

比較の結果は `i32` (0 = false、1 = true)。

### 型変換

```
i32.wrap_i64                 i64 → i32 (下位 32 bit)
i64.extend_i32_s/_u          i32 → i64
f64.promote_f32              f32 → f64
f32.demote_f64               f64 → f32
i32.trunc_f64_s/_u           f64 → i32 (例外あり)
i32.trunc_sat_f64_s/_u       saturating 版 (wasm 2.0)
f64.convert_i32_s/_u         i32 → f64
i32.reinterpret_f32          ビット列の再解釈
```

## ローカル変数・グローバル変数

```wat
(func (param $x i32) (local $tmp i32)
  (local.get $x)         ;; ローカル読み
  (local.set $tmp ...)   ;; ローカル書き
  (local.tee $tmp ...)   ;; 書きつつスタックに残す
  ...)

(global $counter (mut i32) (i32.const 0))   ;; グローバル変数 (mutable)

  (global.get $counter)
  (global.set $counter ...)
```

グローバル変数は `(mut ...)` でないと書込不可。

## 制御構造

### `block` / `loop` / `br`

`block` は名前付きの「ここを抜けるラベル」、`loop` は「ここに戻るラベル」。
`br $label` で抜ける/戻る:

```wat
(block $exit
  (loop $continue
    ;; 条件
    (br_if $exit (i32.eqz (local.get $i)))
    ;; 本体
    (local.set $i (i32.sub (local.get $i) (i32.const 1)))
    ;; 先頭に戻る
    (br $continue)))
```

| 命令 | 意味 |
|---|---|
| `block` ... `end` | 名前付きスコープ。`br` で末尾に飛べる |
| `loop` ... `end` | 名前付きスコープ。`br` で先頭に飛ぶ |
| `br $label` | 無条件分岐 |
| `br_if $label` | スタック値が真なら分岐 |
| `br_table $l1 $l2 ... $default` | スタック値で分岐先を選択 (jump table) |
| `return` | 関数から戻る |
| `unreachable` | 必ずトラップ (到達不能の印) |
| `nop` | 何もしない |

### `if`

```wat
(if (result i32)               ;; 結果型は省略可 (void なら不要)
  (i32.lt_s (local.get $n) (i32.const 0))
  (then (i32.const -1))
  (else (i32.const 1)))
```

## 関数呼出

### 直接呼出 `call`

```wat
(call $fib (i32.const 30))
```

### 間接呼出 `call_indirect`

テーブル経由で動的に関数を呼ぶ。型シグネチャを実行時にチェック:

```wat
(type $sig (func (param i32) (result i32)))
(table funcref (elem $f1 $f2 $f3))

(call_indirect (type $sig)
  (i32.const 5)              ;; 引数
  (i32.const 1))             ;; テーブルインデックス → $f2 を呼ぶ
```

シグネチャが合わなければトラップ。

## 線形メモリ

```wat
(memory (export "mem") 1 10)            ;; 初期 1 ページ、最大 10 ページ (1 ページ = 64 KiB)

(data (i32.const 0) "hello\00")          ;; 初期化データ

(func (export "load") (param $i i32) (result i32)
  (i32.load (local.get $i)))             ;; 4 byte 読み出し

(func (export "store") (param $i i32) (param $v i32)
  (i32.store (local.get $i) (local.get $v)))
```

ロード/ストア命令は型・幅・符号のバリエーションが充実:

```
i32.load           i64.load           f32.load     f64.load
i32.load8_s        i32.load8_u        i32.load16_s i32.load16_u
i32.store          i64.store          f32.store    f64.store
i32.store8         i32.store16        i64.store8   i64.store16  i64.store32
```

| 命令 | 意味 |
|---|---|
| `memory.size` | 現在のページ数 |
| `memory.grow` | n ページ伸ばす (失敗時 -1) |

範囲外アクセスはトラップ。

## モジュールの構成要素

```wat
(module
  (type ...)        ;; 関数シグネチャ宣言
  (import "module" "name" (func | memory | global | table) ...)
  (func ...)
  (table ...)
  (memory ...)
  (global ...)
  (export "name" (func | memory | global | table) ...)
  (start $init)     ;; モジュール読込時に呼ばれる関数
  (elem ...)        ;; テーブル要素初期化
  (data ...)        ;; メモリ初期化
)
```

## Import (host との接続)

wastro は組込みの host 関数 (`env.*`) を提供:

| Import 名 | 効果 |
|---|---|
| `env.log_i32 / log_i64 / log_f32 / log_f64` | 値を stdout に出力 |
| `env.putchar` | 1 文字出力 |
| `env.print_bytes` | (ptr, len) でメモリ範囲を出力 |

```wat
(module
  (import "env" "log_i32" (func $log (param i32)))

  (func (export "main")
    (call $log (i32.const 42))))
```

未バインドの import はスタブが入り、実際に呼ぶとトラップする。

## spec-test harness

Wasm 公式 spec-testsuite の `.wast` 形式 (テスト宣言) を直接実行できる:

```sh
./wastro --test test.wast
```

`(assert_return (invoke "fib" (i32.const 10)) (i32.const 55))` のような
アサーションを順に実行・検証する。

## 例

```wat
;; fib in WAT (folded)
(module
  (func $fib (export "fib") (param $n i32) (result i32)
    (if (result i32) (i32.lt_s (local.get $n) (i32.const 2))
      (then (local.get $n))
      (else (i32.add
        (call $fib (i32.sub (local.get $n) (i32.const 1)))
        (call $fib (i32.sub (local.get $n) (i32.const 2))))))))
```

```wat
;; fact in stack-style WAT
(module
  (func $fact (export "fact") (param $n i32) (result i32)
    (local $r i32)
    i32.const 1
    local.set $r
    block $exit
      loop $cont
        local.get $n
        i32.eqz
        br_if $exit
        local.get $r
        local.get $n
        i32.mul
        local.set $r
        local.get $n
        i32.const 1
        i32.sub
        local.set $n
        br $cont
      end
    end
    local.get $r))
```

## サポートしない

- SIMD (`v128` 命令群) — wasm 2.0
- 参照型 (`anyref` / `externref`) — wasm 2.0
- 例外処理 (`try` / `catch` / `throw`) — wasm 2.0 proposal
- マルチメモリ・マルチテーブル
- スレッド (atomics / `wait` / `notify`)
- GC proposal (`struct.new` 等)
- Tail call (`return_call` / `return_call_indirect`)
- Bulk memory ops のうち高度な機能 (`memory.copy` / `memory.fill` 一部)
- WASI host 関数群 (簡易な `env.*` のみ)

詳細: [`done.md`](done.md) / [`todo.md`](todo.md)。
