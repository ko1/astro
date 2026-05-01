# todo.md — 残った未実装 / 将来の最適化

[`done.md`](./done.md) で大半は潰し済み。ここに残るのは「実装はあるが
完全でない」または「意図的に skip した」項目。

## 言語仕様の不完全な部分

### 真のサスペンド型構文
- [ ] **真のジェネレータ (`function*` / `yield` の suspend)**
      現状: 構文は受理、yield は no-op。
      対応案: `ucontext_t` でコルーチンを作り、`yield` で `swapcontext`。
      見積: ~400行
- [ ] **async/await の microtask スケジューリング**
      現状: 全部同期実行 (Promise の `__value` を即取り出し)。
      対応案: イベントループ + microtask キュー。`Promise.then(cb)` の
      コールバックを次のイベントループ tick に積む。

### 値 / オブジェクトモデルの細部
- [ ] **BigInt** の実装 — 現状は `123n` リテラル構文を受理するだけで、値は通常の
      Number として扱っている (`typeof 123n === "number"` になってしまう)。
      正しくは独立したプリミティブ型 (`typeof === "bigint"`)。実装には新しい
      heap 型 `JS_TBIGINT` (任意精度整数; mini-gmp 等)、`+ - * / %` 等の演算子の
      BigInt 対応、Number との混合をブロックする TypeError、`BigInt(value)` 変換、
      `BigInt.prototype.{toString, valueOf}` が必要
- [ ] **Symbol** をプロパティキーとして使う — 現状 well-known は文字列キー化
      してるが、`obj[symbolValue] = v` のようなユーザ定義 Symbol キーは
      JsString と同じ扱いにできない
- [ ] **真の WeakMap/WeakSet** — 自動 GC は実装済みだが、weak reference を
      考慮したマーカー (キー reachability の 2-pass walk) が未実装。現状は
      強参照で代用。
- [ ] **TypedArray の型強制** — Uint8Array に 256 を入れたら 0、Int32Array に
      double を入れたら trunc 等の coercion
- [ ] **enumerable / configurable / writable** プロパティ属性 — 現状 hasOwn
      で見える/見えないの 1bit のみ
- [ ] **String の UTF-16 length** — `.length` は byte 長、サロゲートペア
      未対応

### 細かい仕様逸脱
- [ ] `==` で Symbol/BigInt の特殊ケース — Symbol 同士は pointer 等価で動く
      が、BigInt と Number の `==` は ToNumber に倒している
- [ ] sparse array の hole 列挙 — 大半は OK だが `.forEach` 等が hole を skip
      しないかもしれない
- [ ] `Number.prototype.toLocaleString`, `toExponential`
- [ ] `Array.prototype.sort` のデフォルト比較が ToString — 正しく実装している
      が、安定ソートかは merge sort なので問題なし
- [ ] `function f.prototype` への代入が IC を invalidate — F.prototype を別
      オブジェクトに付け替えた後、既存の `instance instanceof F` は古い
      プロトタイプを参照する
- [ ] `with` 文 — 仕様上は存在 (非推奨)。jstro はパースしない
- [ ] `eval` 内で caller のローカルスコープに触れる — jstro の eval は
      新規 top-level スコープ
- [ ] `new.target` がアロー関数で「外側の関数の new.target」を継ぐ — 現状
      呼び出しごとに `c->new_target` を上書きしているので、アロー内では
      undefined に戻ってしまう

### 構文糖衣の欠落
- [ ] **getter/setter の `defineProperty` 経由のアクセサデスクリプタ** —
      `Object.defineProperty(o, 'x', { get() {...}, set(v) {...} })` は
      data descriptor として処理してしまう
- [ ] **import.meta**, dynamic `import()`
- [ ] **`@decorator`** 構文
- [ ] **Hashbang** `#!/usr/bin/env node` のサポート

## 性能上の課題

V8 TurboFan に直接届かない領域。それぞれ独立した中〜大プロジェクト。

### High — TurboFan の数値最適化を追うのが本筋
- [ ] **Generational GC (nursery + write barrier)** — binary_trees / state
      など allocation-heavy ベンチで mark+sweep が 30 % 占めている。
      短命オブジェクトを nursery で copy collection、tenured には
      昇格でしか入れない構成。見積: 500-1000 行、~1 週間。
- [ ] **Escape analysis / 型推論で box 除去** — mandelbrot / nbody は
      JV_DBL の box/unbox + 型タグ検査が支配。double をレジスタで保持
      するのは whole-method 解析が前提なので、まずは call 単位の
      escape analysis から。見積: 数週間。
- [ ] **call frame の register-pass 化** — jstro_inline_call が alloca +
      memcpy + CTX 4 フィールドの save/restore を毎回。fib / fact の
      ホット recursion がここで失速。callee_frame をレジスタで運ぶ
      ABI に切り替えれば 10-20 % 取れそう。
- [ ] **多形性 IC (4-way)** — `node_member_get` の IC が 1-way (monomorphic)
      なので、複数 shape を見るサイトで毎回 `js_shape_find_slot` に
      落ちる。poly.js が 4.4× 後ろなのはここ。
- [ ] **method call IC の融合** — `obj.foo(args)` が `foo` を IC ヒット
      で取り出した後、`js_call_func_direct` の indirect call が残っている。
      cached_fn を IC に積めば直接 call できる。

### Mid
- [ ] **regex JIT** — V8 Irregexp 相当の NFA → DFA → native コード経路。
      現状の NFA バックトラッキングマッチャでは 2.75× 後ろ。Onigmo
      組み込みもアリ。
- [ ] **proto chain IC** — `instance.method` で proto から取り出すパス
      が毎回プロトタイプ走査。`(own_shape, proto, proto_slot)` IC。
- [ ] **アリティ ≥4 の call 特化** — 現状 0-3 のみ。

### Low — マイナーチューニング
- [ ] **string transient skip-intern** — `"key" + i` の concat 結果を
      Map key として食わせるとき、毎回 intern してる (map_coll で 25 %
      の cycle)。Map key としてだけ使うなら content hash + 一時 buffer
      で済むはず。
- [ ] **オーバーフロー検出での lazy double 昇格** — 加算は overflow しない
      と仮定 (63bit 余裕がある前提)。乗算のみ `__builtin_mul_overflow`。
- [ ] **arena allocator for short-lived strings**

## デバッグ性 / 開発体験

- [ ] **Source map / 行番号付きスタックトレース**
      AST ノードに line を載せれば Error.stack が動かせる (ただしノード
      ヘッダのサイズが増える)。
- [ ] **`--dump-shapes`** — 全 hidden class の数 / hot shape のリスト
- [ ] **`--profile`** — 関数ごとの呼び出し回数 / 累計時間
- [ ] **diff fuzzer の拡張** — 関数定義 / class / try-catch 等を含む
      ランダム生成

## モジュール周り

- [ ] **`node_modules` 解決** — npm 慣習のディレクトリ走査
- [ ] **package.json の `main`/`exports`/`type: module`** 解釈
- [ ] **動的 `import("path")`** — Promise 返却
