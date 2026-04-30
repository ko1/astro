-- CLBG binary-trees (GC-pressure benchmark — allocate, count, free).
-- Source: Mike Pall's Lua port via
--   https://github.com/hanabi1224/Programming-Language-Benchmarks
--     /blob/main/bench/algorithm/binarytrees/1.lua
-- Replaced `io.write(string.format(...))` with a single `print`-able
-- string so output comparison against lua5.4 is line-stable.

local function BottomUpTree(depth)
    if depth > 0 then
        depth = depth - 1
        return {BottomUpTree(depth), BottomUpTree(depth)}
    else
        return {}
    end
end

local function ItemCheck(tree)
    if tree[1] then
        return 1 + ItemCheck(tree[1]) + ItemCheck(tree[2])
    else
        return 1
    end
end

local function run(N)
    local mindepth = 4
    local maxdepth = mindepth + 2
    if maxdepth < N then maxdepth = N end

    local out = {}
    local stretchdepth = maxdepth + 1
    local stretchtree = BottomUpTree(stretchdepth)
    out[#out+1] = string.format("stretch tree of depth %d\t check: %d", stretchdepth, ItemCheck(stretchtree))

    local longlivedtree = BottomUpTree(maxdepth)

    for depth = mindepth, maxdepth, 2 do
        local iterations = 2 ^ (maxdepth - depth + mindepth)
        local check = 0
        for _ = 1, iterations do
            check = check + ItemCheck(BottomUpTree(depth))
        end
        out[#out+1] = string.format("%d\t trees of depth %d\t check: %d", iterations, depth, check)
    end

    out[#out+1] = string.format("long lived tree of depth %d\t check: %d", maxdepth, ItemCheck(longlivedtree))
    return table.concat(out, "\n")
end

-- N=15 lands in ~1s plain on luastro; AOT-cached should be ~0.3s.
print(run(15))
