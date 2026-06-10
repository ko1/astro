# 既知の課題と今後の方向

核心アイデアは [idea.md](./idea.md)。実装状況は VMIL2025 から PPL2026 (JIT 試作)
を経て、本書執筆時点 (2026 春) までで進んだ部分・未踏部分が混在している。
以下は **現状のステータス** と **未踏領域** を区別して整理。

## 1. 進捗があった項目

### JIT 階層 (L0/L1/L2) — 試作実装済み
- naruby で L0/L1/L2 すべての層を実装し、PPL2026 で試作評価を発表。
- prime? ベンチで JIT-on 1.11s vs JIT-off 13.64s (12× speedup)、キャッシュヒット時は 1.05s。
- L1 デーモン (Ruby) の Unix socket / TCP プロトコルが実用域。
- 設計の詳細は [idea_jit.md](./idea_jit.md)。

### AOT + JIT の併用
- naruby は AOT (PGC) + JIT を共存。koruby / pystro 等は AOT 中心で
  起動時に既存 SD をロードして使う運用が確立。
- 初回ロード時バーストを AOT が吸収、ホットパスを JIT が再最適化、という構図は naruby で実証済み。

### 例外処理
- jstro (longjmp `throw`)、pystro (`try`/`except`/`else`/`finally`/`raise`)、pascalast (catchable `try/except/finally`)、astocaml (例外) がそれぞれ別方式で実装済み。
- 結論: setjmp/longjmp は C で十分扱える。EVAL 本体は触らず、parser-pass + ranges リスト方式で意外と綺麗に書ける ([perf.md](./perf.md) の各サンプル節参照)。

### 静的型サンプル
- pascalast (Pascal)、castro (C subset)、astocaml (OCaml subset)、wastro (Wasm 1.0) が静的型側を埋めた。
- 結論: 静的型情報を活かした **算術ノードの per-type 分裂** + AOT が、`fpc -O3` 等のネイティブコンパイラと張り合えるケースが出ている (pascalast のタイト数値ループ)。

## 2. 未踏 / 検討中

### 独自ローダ
- 現在は .so ファイルベースで、ページアライメントにより小さな関数でもメモリ浪費。
- 専用のネイティブコードローダ (copy & patch + ELF relocation) で密に mmap する設計が候補。CPython JIT (PEP 744) 同様の方向。
- 計算機環境ごとの実装が必要で、ポータビリティとのトレードオフ。code_store_quirks.md の 3 つの罠 (パス名キャッシュ・atomic rename・dlclose 不能) もこれで一掃される。

### コードストア上限サイズ
- 現在は生成コードを削除しない → 実運用では LRU 等で上限管理が必要。

### 投機的プリフェッチ
- L2 への query が h1, h2, h3... と連続する場合、h4, h5... を投機的にプリフェッチ可能。

### コードサイズ膨張
- 部分評価を無差別に適用するとコードサイズが爆発。
- ハッシュ関数のカスタマイズ (`@ref` で副情報をハッシュから外す等) で制御可能。
- 静的型サンプル (pascalast / wastro) は per-type 分裂でノード数が増えやすく、ここの管理が今後も課題。

### 統一 GC 基盤
- 現状は libgc (Boehm) ベースが多数派 (koruby / asom / astr / astocaml 等)、自前 mark-sweep が一部 (luastro)、CRuby GC 流用 (abruby) と分かれている。
- pluggable GC (例: Bartlett mostly-copying) を `value.def` + frame iterator + AST 不動を前提に共通化する設計が [gc_design.md](./gc_design.md) で成文化済み (実装は `runtime/precise_gc/` 配下、reference sample は `sample/baruby_precise/`)。
