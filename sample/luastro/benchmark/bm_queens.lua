-- AWFY queens (8-queens placement, 10 inner iterations per benchmark call).
-- Source: https://github.com/smarr/are-we-fast-yet (benchmarks/Lua/queens.lua)
-- Stripped of `require'benchmark'` inheritance; runs the inner loop
-- ITERS times so that AOT-cached lands in the 0.3-1s range.

local queens = {}
queens.__index = queens

function queens:queens ()
    self.free_rows  = {true, true, true, true, true, true, true, true}
    self.free_maxs  = {true, true, true, true, true, true, true, true,
                       true, true, true, true, true, true, true, true}
    self.free_mins  = {true, true, true, true, true, true, true, true,
                       true, true, true, true, true, true, true, true}
    self.queen_rows = {-1,   -1,   -1,   -1,   -1,   -1,   -1,   -1}
    return self:place_queen(1)
end

function queens:place_queen (c)
    for r = 1, 8 do
        if self:get_row_column(r, c) then
            self.queen_rows[r] = c
            self:set_row_column(r, c, false)
            if c == 8 then return true end
            if self:place_queen(c + 1) then return true end
            self:set_row_column(r, c, true)
        end
    end
    return false
end

function queens:get_row_column (r, c)
    return self.free_rows[r] and self.free_maxs[c + r] and self.free_mins[c - r + 8]
end

function queens:set_row_column (r, c, v)
    self.free_rows[r        ] = v
    self.free_maxs[c + r    ] = v
    self.free_mins[c - r + 8] = v
end

function queens:benchmark ()
    local result = true
    for _ = 1, 10 do
        result = result and self:queens()
    end
    return result
end

local q = setmetatable({}, queens)
local ITERS = 1500
local result
for _ = 1, ITERS do result = q:benchmark() end
print(result)
