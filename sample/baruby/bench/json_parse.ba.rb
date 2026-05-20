# json_parse — recursive-descent JSON parser implemented in baruby.
#
# Macro pattern: parse a fixed JSON document into a nested AST (Array of
# Arrays, since baruby has no Hash) on every iter.  Stresses string
# slicing, recursion, array allocation + growth, and small-Array
# returns from parse helpers (each returns `[value, next_index]`).
#
# Profile:
# - String slicing (text[i, n]) — most tokens ≤ 7 chars (SSO target)
# - Array.push with growth (parse_array / parse_object build chains)
# - Recursion depth ~4 (top → array → object → value → string)
# - Lots of [value, idx] 2-element Arrays returned from every parser
#   helper (~mid-life: a few function frames before discarded)

def skip_ws(s, i)
  n = s.size
  while i < n
    c = s[i]
    if c == " " || c == "\n" || c == "\t" || c == "\r"
      i = i + 1
    else
      return i
    end
  end
  i
end

def parse_string(s, i)
  # s[i] == '"' on entry; scan to closing '"'.  No escape handling.
  start = i + 1
  i = start
  n = s.size
  while i < n
    if s[i] == "\""
      return [s[start, i - start], i + 1]
    end
    i = i + 1
  end
  [s[start, n - start], n]
end

def parse_number(s, i)
  # Integer-only.  Stops at first non-digit.
  start = i
  n = s.size
  if s[i] == "-"
    i = i + 1
  end
  while i < n
    c = s[i]
    if c >= "0" && c <= "9"
      i = i + 1
    else
      return [s[start, i - start].to_i, i]
    end
  end
  [s[start, n - start].to_i, n]
end

def parse_value(s, i)
  i = skip_ws(s, i)
  c = s[i]
  if c == "["
    parse_array(s, i)
  elsif c == "{"
    parse_object(s, i)
  elsif c == "\""
    parse_string(s, i)
  elsif c == "t"
    [true, i + 4]
  elsif c == "f"
    [false, i + 5]
  elsif c == "n"
    [nil, i + 4]
  else
    parse_number(s, i)
  end
end

def parse_array(s, i)
  i = i + 1                # skip '['
  out = []
  i = skip_ws(s, i)
  if s[i] == "]"
    return [out, i + 1]
  end
  while true
    r = parse_value(s, i)
    out.push(r[0])
    i = skip_ws(s, r[1])
    c = s[i]
    if c == ","
      i = i + 1
    elsif c == "]"
      return [out, i + 1]
    else
      return [out, i]
    end
  end
end

def parse_object(s, i)
  i = i + 1                # skip '{'
  out = []
  i = skip_ws(s, i)
  if s[i] == "}"
    return [out, i + 1]
  end
  while true
    i = skip_ws(s, i)
    kr = parse_string(s, i)
    i = skip_ws(s, kr[1])
    i = i + 1              # skip ':'
    vr = parse_value(s, i)
    out.push([kr[0], vr[0]])
    i = skip_ws(s, vr[1])
    c = s[i]
    if c == ","
      i = i + 1
    elsif c == "}"
      return [out, i + 1]
    else
      return [out, i]
    end
  end
end

# Sum integer leaves in the parsed AST.  baruby has no `is_a?` /
# `Integer === v`, so we walk by shape rather than type-test: the
# parser produces top-level Array of objects, each object is Array of
# [string_key, value] pairs, values are int / string / Array-of-ints.

def sum_tags(arr)
  # arr: Array of ints (possibly empty).
  total = 0
  i = 0
  n = arr.size
  while i < n
    total = total + arr[i]
    i = i + 1
  end
  total
end

def sum_record(rec)
  # rec: Array of [key, value] pairs.  key is string; value is int or Array.
  total = 0
  i = 0
  n = rec.size
  while i < n
    pair = rec[i]
    key = pair[0]
    val = pair[1]
    if key == "id"
      total = total + val
    elsif key == "tags"
      total = total + sum_tags(val)
    end
    # "name" → ignored
    i = i + 1
  end
  total
end

def sum_ints(top)
  total = 0
  i = 0
  n = top.size
  while i < n
    total = total + sum_record(top[i])
    i = i + 1
  end
  total
end

# 5-record array of 3-field objects with a small nested tag list each.
# Realistic small-object pattern; per-iter all allocations short-lived.
input = "[{\"id\":1,\"name\":\"alice\",\"tags\":[10,20,30]},{\"id\":2,\"name\":\"bob\",\"tags\":[40,50]},{\"id\":3,\"name\":\"carol\",\"tags\":[60,70,80,90]},{\"id\":4,\"name\":\"dave\",\"tags\":[]},{\"id\":5,\"name\":\"eve\",\"tags\":[100]}]"

# Per parse: integer leaves are ids + tag values.
# ids = 1+2+3+4+5 = 15.  tags = (10+20+30)+(40+50)+(60+70+80+90)+()+(100) = 60+90+300+0+100 = 550.
# Per parse total = 565.  20_000 iters × 565 = 11_300_000 (~1 s on immix_gen).
i = 0
total = 0
while i < 20_000
  r = parse_value(input, 0)
  total = total + sum_ints(r[0])
  i = i + 1
end
p total
