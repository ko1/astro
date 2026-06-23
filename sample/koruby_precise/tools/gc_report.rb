#!/usr/bin/env ruby
# Render a GC-backend comparison report (markdown) from a gc-bench TSV.
# Usage: ruby tools/gc_report.rb <tsv> [baseline_backend]
tsv = ARGV[0] or abort "need tsv"
base = ARGV[1] || "copy"
rows = []
File.foreach(tsv) do |l|
  next if l.start_with?("bench\t") || l.strip.empty?
  b, gc, el, gcc, gs, mb, ok = l.chomp.split("\t")
  rows << { b: b, gc: gc, el: el.to_f, el_raw: el, gcc: gcc, gs: gs.to_f, gs_raw: gs, mb: mb.to_f, ok: ok == "1" }
end
benches  = rows.map { |r| r[:b]  }.uniq
backends = rows.map { |r| r[:gc] }.uniq
H = {}
rows.each { |r| H[[r[:b], r[:gc]]] = r }

def fmt(x, w, p = 2) = (x.nil? ? "—" : ("%.#{p}f" % x)).rjust(w)

puts "# koruby_precise — GC backend 比較レポート (全 #{benches.size} bench × #{backends.size} backend)"
puts
puts "計測: `--plain` interp, round-robin best-of-3 (load 24–30/16core を相殺), "
puts "`KORUBY_GC_STATS=1`。allocMB/gc_count は決定論的 (load 非依存)、実行時間/GC時間は"
puts "3周の最小値[秒]。全 run を CRuby 出力と一致検証済。baseline=#{base}。"
puts "**集約値 (geomean 等) は出さない — ベンチ母集団の選び方で操作できるため。生数字で判断する。**"
puts
# --- alloc / gc_count (deterministic), from baseline backend ---
puts "## 1. 各 bench の GC 負荷 (決定論的, load 非依存)"
puts
puts "allocMB = 総アロケーション量 [MB]、gc_count = GC (collection) 起動回数 (#{base} backend、他 backend もほぼ同じ)。"
puts
puts "| bench | allocMB (総アロケーション) | gc_count (GC回数) |"
puts "|---|--:|--:|"
benches.sort_by { |b| -(H[[b, base]]&.[](:gcc).to_i || 0) }.each do |b|
  r = H[[b, base]]
  puts "| #{b} | #{("%.0f" % (r&.[](:mb) || 0))} | #{r&.[](:gcc)} |"
end
puts
# --- elapsed table + ratio vs base ---
puts "## 2. 実行時間 (wall-clock 全体, 秒) — best-of-3, 括弧内は #{base} 比"
puts
puts "各セル = プログラム全体の実行時間 [秒] (mutator + GC 込み)。`(x.xx×)` は #{base} backend との比 (1.00 未満が速い)。"
puts
puts "| bench | " + backends.map { |g| g.sub("mark_", "m_").sub("compact", "cmp") }.join(" | ") + " |"
puts "|---|" + backends.map { "--:" }.join("|") + "|"
benches.each do |b|
  baseel = H[[b, base]]&.[](:el)
  cells = backends.map do |g|
    r = H[[b, g]]
    next "—" unless r && r[:ok] && r[:el] > 0
    ratio = (baseel && baseel > 0) ? r[:el] / baseel : nil
    g == base ? ("%.2f" % r[:el]) : ("%.2f (%.2f×)" % [r[:el], ratio])
  end
  puts "| #{b} | " + cells.join(" | ") + " |"
end
puts
# --- gc_seconds table ---
puts "## 3. GC 時間 (GC に費やした時間のみ, 秒) — best-of-3"
puts
puts "各セル = その run で GC (collection) に費やした時間 [秒] のみ。上の実行時間の内数。"
puts
puts "| bench | " + backends.map { |g| g.sub("mark_", "m_").sub("compact", "cmp") }.join(" | ") + " |"
puts "|---|" + backends.map { "--:" }.join("|") + "|"
benches.each do |b|
  cells = backends.map { |g| r = H[[b, g]]; (r && r[:ok]) ? ("%.3f" % r[:gs]) : "—" }
  puts "| #{b} | " + cells.join(" | ") + " |"
end
puts
# NOTE: 集約値 (geomean 等) は出さない。ベンチ母集団の選び方で操作できるため
# (ユーザ厳命)。per-benchmark の生数字のみ。
# --- failures ---
fails = rows.reject { |r| r[:ok] }
unless fails.empty?
  puts "## 5. 失敗 (crash / checksum mismatch)"
  puts
  fails.each { |r| puts "- **#{r[:b]} / #{r[:gc]}**: checksum_ok=0 (core dump / timeout / mismatch)" }
  puts
end
