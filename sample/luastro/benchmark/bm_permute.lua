-- AWFY permute (recursively permute 6 elements → 8660 visits per benchmark call).
-- Source: https://github.com/smarr/are-we-fast-yet (benchmarks/Lua/permute.lua)

local permute = {}
permute.__index = permute

function permute:permute (n)
    self.count = self.count + 1
    if n ~= 0 then
        local n1 = n - 1
        self:permute(n1)
        for i = n, 1, -1 do
            self:swap(n, i)
            self:permute(n1)
            self:swap(n, i)
        end
    end
end

function permute:swap (i, j)
    local tmp = self.v[i]
    self.v[i] = self.v[j]
    self.v[j] = tmp
end

function permute:benchmark ()
    self.count = 0
    self.v = {0, 0, 0, 0, 0, 0}
    self:permute(6)
    return self.count
end

local p = setmetatable({}, permute)
local ITERS = 1000
local result
for _ = 1, ITERS do result = p:benchmark() end
print(result)  -- lua5.4: 8660
