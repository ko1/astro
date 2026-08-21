koruby_precise (sample/koruby_precise) の rubyspec core 充足を、run-to-goal で自律的に進めてください。長い wakeup を挟まず、ゴール(in-scope の pass を伸ばしきる)まで連続で。

## 最初にやること
1. メモリ MEMORY.md と project_koruby_precise_sweep_2026_08_13.md を読む(baseline・error バケツ・罠・スコープ境界がある)。
2. 実 mspec で再ベースライン:
   DUMP=/home/ko1/.claude/jobs/.../core.tsv ruby tools/mspec_real_run.rb ~/ruby/src/master/spec/ruby/core 12
   （DUMP は **ENV** で渡す。ARGV ではない。前回 75.5% / pass≈16,876 / err≈2,078）
   バケツ集計: awk -F'\t' '$2~/^[0-9]+$/{split($1,p,"/");e[p[2]]+=$4}END{for(k in e)if(e[k])print e[k],k}' core.tsv | sort -rn

## ループの回し方（1 fix = 1 commit）
- DUMP から err の多い in-scope な file を選ぶ。
- その file の失敗を **CRuby と直接比較**して再現(小さい .rb を書いて `./koruby_precise x.rb` と `ruby x.rb` を diff)。runspec1.rb は shim で数字を水増しする/with_timezone 等を欠くので、実挙動の確認は必ず本物 ruby と突き合わせる。
- 根本原因を1つ直す → CRuby 一致を確認 → 下記ゲート → コミット(日本語メッセージ、末尾に `Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>`、`Claude-Session:` は付けない)。
- 次の file/バケツへ。数千の err を機械的にではなく、共通根本原因(1つ直すと複数 spec が通る)を優先。

## 毎変更のゲート（緑を確認してからコミット）
- make test（baseline **100,098 PASS**。下回ったら退行。Error 1 は fail>0 の正常終了）
- make codeql-check（C を触ったとき）
- C の alloc/root を触ったら STRESS+PURGE: `KORB_GC_STRESS=1 KORB_GC_PURGE=1 ./koruby_precise x.rb`
- AOT/codegen 関連を触ったら make optcarrot-aot FRAMES=30（checksum 60838 必須）
- astrogre を触ったら self-test(118/0) と tests/run.rb(155/155)

## 罠・厳守事項（メモリより）
- builtins/*.c は korb_runtime.c に #include。編集後は必ず `touch korb_runtime.c` してから make。
- koruby 実行は必ず `timeout -k <猶予> <秒>` 付き（暴走を reap 可能に）。
- Ruby fixture は Write tool で書く(bash heredoc は `!` を、printf/echo は `$!` を壊す)。一時ファイルは $CLAUDE_JOB_DIR/tmp。
- 生成物 node_eval.c と *.so はコミットしない。node.def 由来のものは node.def を直す。
- prelude/*.rb は sweep を回している最中に編集しない(起動ごとに読まれ偽退行を出す)。
- 新規グローバル禁止(CTX を引数で渡す)、C は const/restrict を積極付与、警告は無視せず原因を直す。
- git stash 禁止、`git add -A` 禁止(共有 worktree)。コミット前に対象ファイルの diff を確認し、無関係な既存変更を巻き込まない。
- スコープ外(直さない): 実 transcoding(EUC-JP/Windows-31J/UTF-16/32・Unicode case)、TracePoint(棚上げ)、プラットフォーム依存の platform-guard spec。これらは skip し、なぜ out-of-scope かを一言 log する。

## 進め方の姿勢
- 高 ROI 候補: io(291、IO::Buffer 未実装が大)、string/encoding、fiber/raise、kernel、module。ただし毎回 DUMP で実測して選ぶ。
- 区切りごとに「どの file を err 何→何にした / make test 現在値」を短く報告。
- 定期的に(数コミットごと)メモリ project_koruby_precise_sweep_2026_08_13.md を最新の数字・残バケツで更新。
- 全部終わる系のタスクではないので、in-scope の伸びしろが尽きる/ROI が落ちるまで回し、そこで残りの構造的ギャップを一覧化して報告。バックグラウンドプロセスは終了時に必ず止める。

まず再ベースラインして、最初のターゲットを選ぶところから始めてください。
```

## 参考: 現状(2026-08-21 時点)
- make test baseline: **100,523 PASS**
- 実 mspec core: **85.8%**(pass 19,278 / fail 2,117 / err 1,073 / clean 1,083 / 2,144 file)
  最新 DUMP は sweep_0821e (scratchpad)。language は 79.8%。
- 直近: spawn/exec/system を CRuby の起動規約に(子から errno を報告する
  close-on-exec パイプ、[cmdname,argv0]、#to_str/#to_ary、未知オプションキーの
  ArgumentError、umask:/pgroup:/[:out,:err]、system の exception:、posix_sh_cmds)、
  IO.new(fd) のモードを fcntl(F_GETFL) から、Marshal の 'c'/'m'/'M'。
- 次の候補: marshal/load_spec の proc 呼び出し順(4)、module/autoload_spec の
  lexical-scope 再探索(3 ERROR)。
- 保留(docs/todo.md): $? のスレッドローカル化、シンボル表の鍵にエンコーディング、
  module body 内 eval の cref、autoload 中の per-thread 可視性。

## 単一 spec の再実行(毎回間違える)
`SPEC_TEMP_DIR=<writable> ./koruby_precise tools/mspec_launch.rb <spec>`
**ruby で起動すると CRuby を測る**。banner が `(koruby/ASTro)` かで確認する。
