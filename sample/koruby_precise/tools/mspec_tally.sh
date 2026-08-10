#!/bin/sh
# Summarise a DUMP=... tsv from mspec_real_run.rb.
# per-file pass is examples-fail-err and can go negative (mspec reports more
# errors than examples when before/after blocks raise), so clamp at 0.
awk -F'\t' '$2=="WFAIL"{w++;next} {p=$2; if(p<0)p=0; s+=p; fa+=$3; er+=$4;
  if($3==0&&$4==0&&$2>0)cl++} END {printf "pass=%d fail=%d err=%d WFAIL=%d clean=%d\n",s,fa,er,w,cl}' "$@"
