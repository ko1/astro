#!/usr/bin/env ruby
#
# Fetch and prepare a real-world NDJSON dataset for the `jsonl` bench
# suite.  Source: GitHub Archive (https://www.gharchive.org), which
# publishes hourly NDJSON dumps of all GitHub public events under
# CC0-equivalent terms.
#
# Output:  bench/data/jsonl/gharchive.jsonl
#          (~100 MB / 30 000 events, mixed type distribution)
#
# Idempotent: re-running with the file already present is a no-op.
#
# We keep the dataset to ~30 K records (out of ~280 K in a peak hour)
# so each bench cell finishes in ~1 s on jq — sustained scale, not
# microbench.  Selecting the prefix is reproducible (same input
# regardless of when the script is run, as long as the underlying
# 1-hour file hasn't been re-uploaded).

require 'fileutils'
require 'open3'

OUT_DIR  = File.expand_path('jsonl', __dir__)
OUT_FILE = File.join(OUT_DIR, 'gharchive.jsonl')

# Pick a fixed hour (peak weekday slot) for reproducibility.  CC0,
# data.gharchive.org doesn't require auth.
URL_GZ  = 'https://data.gharchive.org/2024-01-15-12.json.gz'
LINES   = 30_000   # truncate to this many records

FileUtils.mkdir_p(OUT_DIR)

if File.exist?(OUT_FILE) && File.size(OUT_FILE) > 50_000_000
  warn "[gen_jsonl] #{OUT_FILE} already present (#{File.size(OUT_FILE) / 1024 / 1024} MB), skipping"
  exit 0
end

tmp_gz = "#{OUT_FILE}.gz.tmp"
warn "[gen_jsonl] downloading #{URL_GZ} (~120 MB compressed)..."
unless system('curl', '-sf', '-o', tmp_gz, URL_GZ)
  abort "[gen_jsonl] curl failed (no network? URL changed?). " \
        "Manually fetch any GitHub Archive hour from " \
        "https://data.gharchive.org/, decompress, and head -#{LINES} " \
        "into #{OUT_FILE}."
end

warn "[gen_jsonl] decompressing + truncating to #{LINES} lines..."
out, err, st = Open3.capture3("gunzip -c #{tmp_gz} | head -n #{LINES} > #{OUT_FILE}")
File.unlink(tmp_gz) rescue nil
abort "[gen_jsonl] decompress/truncate failed: #{err}" unless st.success?

mb = File.size(OUT_FILE) / 1024 / 1024
warn "[gen_jsonl] done — #{OUT_FILE} (#{mb} MB)"
