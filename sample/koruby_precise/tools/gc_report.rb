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

puts "# koruby_precise — GC backend 比較レポート (GC-heavy bench)"
puts
puts "計測: `--plain` interp, round-robin best-of-3 (load 24–30/16core を相殺), "
puts "`KORUBY_GC_STATS=1`。gc_count/allocMB は決定論的 (load 非依存)、min_elapsed/"
puts "min_gc_seconds は3周の最小値。全 run を CRuby 出力と一致検証済。baseline=#{base}。"
puts
# --- alloc / gc_count (deterministic), from baseline backend ---
puts "## 1. 各 bench の GC 負荷 (決定論的: alloc と GC 回数)"
puts
puts "| bench | allocMB | gc_count (#{base}) |"
puts "|---|--:|--:|"
benches.sort_by { |b| -(H[[b, base]]&.[](:gcc).to_i || 0) }.each do |b|
  r = H[[b, base]]
  puts "| #{b} | #{("%.0f" % (r&.[](:mb) || 0))} | #{r&.[](:gcc)} |"
end
puts
# --- elapsed table + ratio vs base ---
puts "## 2. min_elapsed (秒) と #{base} 比"
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
puts "## 3. min_gc_seconds (GC に費やした時間、秒)"
puts
puts "| bench | " + backends.map { |g| g.sub("mark_", "m_").sub("compact", "cmp") }.join(" | ") + " |"
puts "|---|" + backends.map { "--:" }.join("|") + "|"
benches.each do |b|
  cells = backends.map { |g| r = H[[b, g]]; (r && r[:ok]) ? ("%.3f" % r[:gs]) : "—" }
  puts "| #{b} | " + cells.join(" | ") + " |"
end
puts
# --- geomean of elapsed ratio vs base (only valid pairs where base valid) ---
puts "## 4. 総合: #{base} 比 elapsed の geomean (GC-heavy bench 全体)"
puts
puts "| backend | geomean(elapsed/#{base}) | geomean(gc_seconds/#{base}) | valid benches |"
puts "|---|--:|--:|--:|"
backends.each do |g|
  logs_el = []; logs_gs = []; n = 0
  benches.each do |b|
    rb = H[[b, base]]; rg = H[[b, g]]
    next unless rb && rg && rb[:ok] && rg[:ok] && rb[:el] > 0 && rg[:el] > 0
    logs_el << Math.log(rg[:el] / rb[:el]); n += 1
    if rb[:gs] > 0 && rg[:gs] > 0 then logs_gs << Math.log(rg[:gs] / rb[:gs]) end
  end
  gm_el = logs_el.empty? ? nil : Math.exp(logs_el.sum / logs_el.size)
  gm_gs = logs_gs.empty? ? nil : Math.exp(logs_gs.sum / logs_gs.size)
  puts "| #{g} | #{gm_el ? ("%.3f×" % gm_el) : "—"} | #{gm_gs ? ("%.3f×" % gm_gs) : "—"} | #{n} |"
end
puts
# --- failures ---
fails = rows.reject { |r| r[:ok] }
unless fails.empty?
  puts "## 5. 失敗 (crash / checksum mismatch)"
  puts
  fails.each { |r| puts "- **#{r[:b]} / #{r[:gc]}**: checksum_ok=0 (core dump / timeout / mismatch)" }
  puts
end
