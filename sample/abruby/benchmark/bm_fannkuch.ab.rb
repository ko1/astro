# Fannkuch-redux (array permutation)
def fannkuch(n)
  perm = []
  i = 0
  while i < n
    perm.push(i)
    i += 1
  end

  count = []
  i = 0
  while i < n
    count.push(0)
    i += 1
  end

  # copy perm
  perm1 = []
  i = 0
  while i < n
    perm1.push(perm[i])
    i += 1
  end

  max_flips = 0
  checksum = 0
  nperm = 0

  i = 0
  while i < n
    count[i] = i + 1
    i += 1
  end

  while true
    # copy perm1 to perm
    i = 0
    while i < n
      perm[i] = perm1[i]
      i += 1
    end

    # count flips
    flips = 0
    k = perm[0]
    while k != 0
      # reverse perm[0..k]
      lo = 0
      hi = k
      while lo < hi
        tmp = perm[lo]
        perm[lo] = perm[hi]
        perm[hi] = tmp
        lo += 1
        hi -= 1
      end
      flips += 1
      k = perm[0]
    end

    if flips > max_flips
      max_flips = flips
    end
    if nperm % 2 == 0
      checksum += flips
    else
      checksum -= flips
    end
    nperm += 1

    # next permutation
    i = 1
    found = false
    while i < n
      # rotate perm1[0..i]
      tmp = perm1[0]
      j = 0
      while j < i
        perm1[j] = perm1[j + 1]
        j += 1
      end
      perm1[i] = tmp

      count[i] = count[i] - 1
      if count[i] > 0
        found = true
        break
      else
        count[i] = i + 1
        i += 1
      end
    end
    break unless found
  end

  p(checksum)
  max_flips
end

p(fannkuch(9))
