-- AWFY bounce (100 balls, 50 bounce ticks per benchmark call → 1331 bounces).
-- Source: https://github.com/smarr/are-we-fast-yet (benchmarks/Lua/bounce.lua)
-- Inlined `Random` class (was `som.lua`); replaced `band(x, 65535)` with
-- `% 65536` since luastro doesn't ship `bit32`.

local Random = {}
Random.__index = Random
function Random.new ()
    return setmetatable({seed = 74755}, Random)
end
function Random:next ()
    self.seed = ((self.seed * 1309) + 13849) % 65536
    return self.seed
end

local Ball = {}
Ball.__index = Ball

local abs = math.abs

function Ball.new (random)
    local obj = {
        x = random:next() % 500,
        y = random:next() % 500,
        x_vel = (random:next() % 300) - 150,
        y_vel = (random:next() % 300) - 150,
    }
    return setmetatable(obj, Ball)
end

function Ball:bounce ()
    local x_limit, y_limit = 500, 500
    local bounced = false
    self.x = self.x + self.x_vel
    self.y = self.y + self.y_vel
    if self.x > x_limit then
        self.x = x_limit
        self.x_vel = 0 - abs(self.x_vel)
        bounced = true
    end
    if self.x < 0 then
        self.x = 0
        self.x_vel = abs(self.x_vel)
        bounced = true
    end
    if self.y > y_limit then
        self.y = y_limit
        self.y_vel = 0 - abs(self.y_vel)
        bounced = true
    end
    if self.y < 0 then
        self.y = 0
        self.y_vel = abs(self.y_vel)
        bounced = true
    end
    return bounced
end

local bounce = {}
bounce.__index = bounce

function bounce:benchmark ()
    local random     = Random.new()
    local ball_count = 100
    local bounces    = 0
    local balls      = {}
    for i = 1, ball_count do balls[i] = Ball.new(random) end
    for _ = 1, 50 do
        for i = 1, #balls do
            if balls[i]:bounce() then
                bounces = bounces + 1
            end
        end
    end
    return bounces
end

local b = setmetatable({}, bounce)
local ITERS = 500
local result
for _ = 1, ITERS do result = b:benchmark() end
print(result)  -- lua5.4: 1331
