# tokenize — CSV-like string split into an array of substrings.
#
# Macro pattern: long input string + per-iter tokenize.  Each iter creates
# 120 short-lived BaStrings (token substrings via `text[start, len]`) and
# pushes them into a freshly allocated BaArray.  Stresses the joint
# string-slice + array-push allocation path that other benches don't
# exercise together.
#
# Different from string_concat_dyn (concat building) and substr_churn
# (substring of one long-lived string with simple index loop):
#   - tokenize allocates the result *array* per iter (vs single buffer)
#   - tokens are heterogeneous length (3-6 chars) → mix of size classes
#   - drives `baruby_ary_push` (with array growth) alongside
#     `baruby_str_slice`

def make_csv(n)
  base = "red,blue,green,yellow,orange,purple"
  s = ""
  i = 0
  while i < n
    s = s + base
    if i < n - 1
      s = s + ","
    end
    i = i + 1
  end
  s
end

def tokenize_sum(text)
  tokens = []
  total_len = 0
  i = 0
  start = 0
  n = text.size
  while i < n
    if text[i] == ","
      tok = text[start, i - start]
      tokens.push(tok)
      total_len = total_len + tok.size
      start = i + 1
    end
    i = i + 1
  end
  if start < n
    tok = text[start, n - start]
    tokens.push(tok)
    total_len = total_len + tok.size
  end
  total_len
end

# 20 × "red,blue,green,yellow,orange,purple" joined by "," → 120 tokens
# per parse, sum of lengths = 20 × (3+4+5+6+6+6) = 600.
# 17500 iters × 600 = 10500000 → ~1 s on copy backend.
input = make_csv(20)
sum = 0
iter = 0
while iter < 17500
  sum = sum + tokenize_sum(input)
  iter = iter + 1
end
p sum
