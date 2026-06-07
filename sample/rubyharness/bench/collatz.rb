maxlen = 0; n = 1
while n < 300_000
  m = n; len = 0
  while m > 1
    m = m.even? ? m / 2 : 3 * m + 1
    len += 1
  end
  maxlen = len if len > maxlen
  n += 1
end
p maxlen
