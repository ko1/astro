-- AWFY list (linked-list construction + 3-arg recursive tail traversal).
-- Source: https://github.com/smarr/are-we-fast-yet (benchmarks/Lua/list.lua)
-- Stresses `setmetatable({...}, {__index = Class})` chain creation, deep
-- recursion, and method dispatch.

local Element = {}
Element.__index = Element

function Element.new (v)
    local obj = {val = v, next = nil}
    return setmetatable(obj, Element)
end

function Element:length ()
    if not self.next then
        return 1
    else
        return 1 + self.next:length()
    end
end

local list = {}
list.__index = list

function list:make_list (length)
    if length == 0 then
        return nil
    else
        local e = Element.new(length)
        e.next = self:make_list(length - 1)
        return e
    end
end

function list:is_shorter_than (x, y)
    local x_tail, y_tail = x, y
    while y_tail do
        if not x_tail then
            return true
        end
        x_tail = x_tail.next
        y_tail = y_tail.next
    end
    return false
end

function list:tail (x, y, z)
    if self:is_shorter_than(y, x) then
        return self:tail(self:tail(x.next, y, z),
                         self:tail(y.next, z, x),
                         self:tail(z.next, x, y))
    else
        return z
    end
end

function list:benchmark ()
    local result = self:tail(self:make_list(15),
                             self:make_list(10),
                             self:make_list(6))
    return result:length()
end

local l = setmetatable({}, list)
local ITERS = 1500
local result
for _ = 1, ITERS do result = l:benchmark() end
print(result)  -- lua5.4: 10
