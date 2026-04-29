-- AWFY towers (Towers of Hanoi, 13 disks → 8191 moves per benchmark call).
-- Source: https://github.com/smarr/are-we-fast-yet (benchmarks/Lua/towers.lua)
-- Stripped of `require'benchmark'` inheritance.

local towers = {}
towers.__index = towers

local function create_disk (size)
    return {size = size, next = nil}
end

function towers:push_disk (disk, pile)
    local top = self.piles[pile]
    if top and disk.size >= top.size then
      error 'Cannot put a big disk on a smaller one'
    end
    disk.next = top
    self.piles[pile] = disk
end

function towers:pop_disk_from (pile)
    local top = self.piles[pile]
    assert(top, 'Attempting to remove a disk from an empty pile')
    self.piles[pile] = top.next
    top.next = nil
    return top
end

function towers:move_top_disk (from_pile, to_pile)
    self:push_disk(self:pop_disk_from(from_pile), to_pile)
    self.moves_done = self.moves_done + 1
end

function towers:build_tower_at (pile, disks)
    for i = disks, 1, -1 do
        self:push_disk(create_disk(i), pile)
    end
end

function towers:move_disks (disks, from_pile, to_pile)
    if disks == 1 then
        self:move_top_disk(from_pile, to_pile)
    else
        local other_pile = 6 - from_pile - to_pile
        self:move_disks(disks - 1, from_pile, other_pile)
        self:move_top_disk(from_pile, to_pile)
        self:move_disks(disks - 1, other_pile, to_pile)
    end
end

function towers:benchmark ()
    self.piles = {}
    self:build_tower_at(1, 13)
    self.moves_done = 0
    self:move_disks(13, 1, 2)
    return self.moves_done
end

local t = setmetatable({}, towers)
local ITERS = 500
local result
for _ = 1, ITERS do result = t:benchmark() end
print(result)  -- lua5.4: 8191
