
## (2026-09-13) `Kernel#tap` を Ruby に移した代償: 起動 +1.37M 命令

`Kernel#tap` は CRuby でも `<internal:kernel>` の Ruby 定義で、
`source_location` を持ち `Kernel#tap` というラベルで backtrace に出る。
それに合わせて C 実装 (`korb_m_obj_tap`) を捨て prelude に移した。
sp4 で空スクリプトだけを測ると 235.74M → 237.11M 命令 (**+0.58%**)。
定常状態は不変 (マイクロ 11 本 +0.001〜+0.015%、optcarrot AOT −0.013%)。

C の builtin を prelude の Ruby に移すと、この規模の固定費が乗る。
起動を詰めるときは、prelude に積んだメソッドのうち
**互換のために Ruby でなければならないもの**と、単に書きやすくて置いたものを
分けて見直す。計測: `trials/2026-09-13-koruby-caller-review/README.md` の追記。
