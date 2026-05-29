# AnPy 性能メモ

計測: `make bench`（= `ruby benchmark/run_bench.rb`）。各 ChocoPy プログラムを ~1 秒
スケールで `python3` / `anpy`(interp) / `anpy`(AOT) で実行し、**出力一致を確認してから**
best-of-N の wall time を取る。リファレンスは CPython 3.12。

## 代表値（1 コア, gcc -O2, CPython 3.12）

```
benchmark         py3(s)  anpy-int(s)  anpy-aot(s)    int/py3  aot/int
--------------------------------------------------------------------------
fib_rec            0.170        0.650        0.638      3.82x    0.98x
loop_sum           1.731        0.948        0.913      0.55x    0.96x
count_prime        0.379        0.458        0.427      1.21x    0.93x
collatz            4.025        2.203        1.819      0.55x    0.83x
tak                0.388        2.368        2.352      6.11x    0.99x
method_loop        0.525        0.813        0.807      1.55x    0.99x
list_sum           0.180        0.073        0.079      0.40x    1.09x
--------------------------------------------------------------------------
geomean                                                 1.27x    0.96x
```

`int/py3 < 1.0` = AnPy インタプリタが CPython より速い。`aot/int < 1.0` = AOT が効く。

## 解釈

数値は計測の裏付けがある事実に限る（ルート `docs/perf.md` 方針）。

### 1. ループ主体では CPython より速い

`loop_sum` 0.53× / `collatz` 0.53× / `list_sum` 0.41× / `count_prime` の内側ループ。
整数は即値（GC/box なし）で、ループ本体は `EVAL_ARG` でディスパッチが畳まれた木を
辿るだけなので、CPython のバイトコード+box より速い。

### 2. 呼び出し主体では CPython より遅い

`tak` 6.2× / `fib` 3.7× / `method_loop` 1.6×。原因は **関数呼び出しごとの環境フレーム**:
`anpy_invoke` が GC で新フレームを確保し、引数・ローカル・ネスト関数を名前ハッシュに
登録する。CPython は最適化済みフレーム（配列スロット）なのでここで負ける。これは
スコープを名前ベースで実装している設計コスト（[runtime.md](runtime.md)）。

### 3. AOT 特殊化は ~0.96×（小幅）

ディスパッチチェーンは SD に畳まれるが、ホットパスに残るのは env フレーム確保・
名前解決・`anpy_*` ヘルパ呼び出しで、これらは inline されない。`collatz` の 0.83× が
最も効いた例（内側 while がほぼ純ループでフレーム確保が少ない）。

`for` ループ本体も `node_while` と同様 `node_for` 内で `EVAL_ARG(body)` で回す
（以前は C ヘルパ経由の `EVAL(body)` で本体が SD に入らず、AOT 下でも本体が
デフォルト dispatcher で走っていた）。これで for/while の特殊化が揃ったが、
`list_sum` のように本体が名前解決律速だと AOT の取り分は誤差に埋もれる（1.09×）。
**名前→スロット解決**（[todo.md](todo.md)）後に初めて for 本体 inline 化が効く見込み。

## 速くするなら（未着手, [todo.md](todo.md)）

- **名前→スロット解決**: 変数参照を (depth, index) に静的解決し、per-call の名前ハッシュ
  登録を廃す。call 主体ベンチの主因を除ける。abc の fixnum 化に相当する次の一手。
- **フレームのアリーナ/再利用**: 呼び出しフレームを GC でなくスタック/プールに。
