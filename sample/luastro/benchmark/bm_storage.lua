-- AWFY storage (depth-7 4-ary tree allocation → 5461 nodes per benchmark call).
-- Source: https://github.com/smarr/are-we-fast-yet (benchmarks/Lua/storage.lua)
-- GC-pressure benchmark — 100% allocations, no compute.  Inlined Random.

local Random = {}
Random.__index = Random
function Random.new ()
    return setmetatable({seed = 74755}, Random)
end
function Random:next ()
    self.seed = ((self.seed * 1309) + 13849) % 65536
    return self.seed
end

local storage = {}
storage.__index = storage

function storage:build_tree_depth (depth, random)
    self.count = self.count + 1
    if depth == 1 then
        return {n = random:next() % 10 + 1}
    else
        local arr = {n = 4}
        for i = 1, 4 do
            arr[i] = self:build_tree_depth(depth - 1, random)
        end
        return arr
    end
end

function storage:benchmark ()
    local random = Random.new()
    self.count = 0
    self:build_tree_depth(7, random)
    return self.count
end

local s = setmetatable({}, storage)
local ITERS = 200
local result
for _ = 1, ITERS do result = s:benchmark() end
print(result)  -- lua5.4: 5461
