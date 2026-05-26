# ascheme_precise: sp 引数化 PoC 計画メモ

## 動機

`sample/ascheme_precise/docs/perf.md` §4.2 で観察された通り、 fib35 plain
で precise (`copy` = 1.13s) は conservative libgc (= 0.57s) の **1.98×**。
原因は precise rooting の **`c->sp` per-call load/store** = 純再帰で worst
case。

baruby_precise はこれを「dispatcher の 3rd 引数 `VALUE *sp`」 で解消済
(= iter 61 移行)。 同パターンを ascheme_precise にも適用すれば、 fib35 で
**15-30%、 全体 geomean で +5-15%** の speedup が見込める。

## PoC で確認した範囲

2026-05-26 に部分実装 + 早期 revert (= 規模見積もり)。 検証済の変更:

### ✓ 確定: small framework changes

1. **`sample/ascheme_precise/ascheme_gen.rb`** に override 追加:
   ```ruby
   def common_param_count
     3
   end
   def child_dispatch_args(slot, field)
     "c, #{field}, sp"
   end
   ```
   結果: `node_eval.c` / `node_dispatch.c` / `node_alloc.c` の auto-gen
   が `(CTX *c, NODE *n, VALUE *sp, ...)` signature を出すようになった

2. **`sample/ascheme_precise/node.h`**:
   ```c
   typedef VALUE (*node_dispatcher_func_t)(CTX *c, NODE *n, VALUE *sp);
   static inline VALUE EVAL(CTX *c, NODE *n, VALUE *sp) {
       return (*n->head.dispatcher)(c, n, sp);
   }
   ```

3. **`sample/ascheme_precise/node.def`**: perl 一括置換で 38 個の NODE_DEF
   header に `, VALUE * restrict sp` を挿入:
   ```sh
   perl -i -pe 's/^(node_[a-z_0-9]+\(CTX \* restrict c, NODE \* restrict n)/$1, VALUE * restrict sp/' node.def
   ```

4. **`sample/ascheme_precise/context.h`** の `SP_PUSH` を「local sp を
   宣言しない」 形に書換:
   ```c
   #define SP_PUSH(c, name, n) do {                           \
       assert((name) + (n) <= g_sp_scratch + ASCHEME_SP_SCRATCH_SIZE); \
       for (int _spi = 0; _spi < (n); _spi++) (name)[_spi] = 0; \
       (c)->sp = (name) + (n);                                \
   } while (0)
   ```
   `name` は呼び出し側が既に scope に持つ変数 (= NODE_DEF body の `sp`
   パラメータ)。

### ✗ 残作業: large mechanical edits

PoC build で **42 個の compile error** が `main.c` から出る。 内訳:

#### A. `SP_PUSH(c, sp, N)` 系 (= main.c の 42 sites)

`main.c` の関数 (= `scm_apply`, `scm_callcc`, builtin helpers 等) は
NODE_DEF ではないので sp parameter を持たない。 旧 `SP_PUSH(c, sp, N)`
が `VALUE *sp = c->sp;` を declare していたが、 新 macro は declare
しないため `sp` が未定義。

**fix**: 各関数の入口で:
```c
VALUE * restrict sp = c->sp;
SP_PUSH(c, sp, N);   // c->sp = sp + N (sp 自体は局所変数)
...
SP_POP(c, sp);
```

main.c の 42 sites を機械的に修正:
```sh
# Find affected functions (= ones using SP_PUSH without sp param)
grep -nE "SP_PUSH\(c, sp[a-z_]*," main.c
```

custom-name sites (= `sp_q`, `sp_top`, `sp_nlh`, `sp_iter`, `sp_nl`):
こちらは別名で declare していた → そのまま `VALUE *sp_q = c->sp;` 形に。

#### B. `EVAL(c, n)` 呼出 sites (= main.c に 2 箇所、 node_eval.c に多数)

`main.c:2547`, `main.c:4784` の `EVAL(c, body)` などに sp 引数を追加。
node_eval.c は auto-gen なので A 修正後の rebuild で大半自動解決。

#### C. node.def NODE_DEF body の SP_PUSH/POP 慣用句

```
27 × SP_PUSH(c, sp, N) と 38 × SP_POP(c, sp) を含む NODE_DEF body
```

これらは新 macro semantic で **そのまま動く** (= sp は param、 SP_PUSH
は c->sp 更新のみ)。 ただし冗長な `SP_POP(c, sp)` (= c->sp = sp 戻し)
は次の EVAL 呼出が同じ sp を渡すので不要に近い。 コード自体は残しても
correctness OK、 後で cleanup。

#### D. `node_emit_ast.c` および AOT SD generation

NODE_DEF signature change で SD (= specialised dispatcher) の出力にも
sp が必要。 emit_ast.c が直接 `EVAL_<name>(c, ...)` を call する箇所も
sp threading。

#### E. AOT code_store キャッシュ無効化

既存 `code_store/all.so` の SD は旧 2-arg signature でリンクされている。
PoC build 後、 `--clear-cs` で再ベイクが必要。 hash mismatch で
silent fallback (= plain dispatch) になるリスクを test で確認。

## 実装手順 (= 推奨フェーズ)

### Phase 1: skeleton (= 確認済の framework change)
- ascheme_gen.rb、 node.h、 node.def 一括置換、 context.h SP_PUSH 修正
- これで auto-gen は通る、 main.c でリンク 42 errors

### Phase 2: main.c の `SP_PUSH(c, sp, N)` 修正 (= 機械的)
- 各関数の入口に `VALUE *sp = c->sp;` 挿入 (= 42 箇所)
- custom-name version (`sp_q` 等) は `VALUE *sp_q = c->sp;`
- EVAL() call 2 箇所に sp 引数追加 (= caller の sp を渡す)

### Phase 3: smoke test
- `make GC=copy && make test` で 16/16 + R5RS 179/179 pass を確認
- `./ascheme_precise -q --aot-compile bench/big/fib35.scm` で AOT も pass

### Phase 4: bench
- `bench-results/20260526/bench_baruby.sh` 相当の bench を ascheme で実行
- `sp` 引数化前 (= 8105bf85) と後で fib35 / cps_loop の elapsed 比較
- 期待: fib35 で **20-30% speedup**

### Phase 5: AOT speedup の cross-check
- `--clear-cs --aot-compile` で fresh SD bake → AOT mode でも speedup 確認
- 既存 SD と shape が異なるため hash mismatch → 自動 rebake する想定

### Phase 6: cleanup (= optional)
- node.def body から不要 `SP_POP(c, sp)` を除去
- `g_sp_scratch` array が不要になるか検討 (= sp は caller chain で繋がっており、
  global scratch は startup 時のみ)

## 工数見積もり

- Phase 1: 30 分 (= 確認済)
- Phase 2: 1-2 時間 (= main.c 42 sites × 1 行追加)
- Phase 3: 30 分 (= test debug)
- Phase 4-5: 1 時間 (= bench + commentary)
- Phase 6: 30 分 (= optional)

合計 **~4 時間** + bench wallclock。 当面 high-priority な user 要望が
無ければ後回し OK。 やる場合は別 session の頭で集中して。

## risk

- baruby_precise iter 61 で同 migration をしているので pattern は実証済
- main.c の関数群は GC-aware だが sp scope は per-function なので shadow なし
- AOT cache mismatch は --clear-cs で OK (= 既知の rebake パターン)

## 関連 file

- `docs/perf.md` §4.2 (= fib35 1.98× 差の caveat)
- `sample/baruby_precise/baruby_gen.rb` (= reference impl)
- `sample/baruby_precise/node.def` (= sp 引数 NODE_DEF パターン例)
- `sample/baruby_precise/context.h:391-408` (= alloc helper の sp 渡し方)
