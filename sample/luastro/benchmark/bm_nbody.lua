-- Trimmed n-body benchmark: 5 bodies (sun + 4 planets), 5_000 steps.
-- Reduced from the canonical Computer Language Shootout version so it
-- finishes quickly in the interpreter while still exercising float
-- arithmetic.
local PI = 3.141592653589793
local SOLAR_MASS = 4 * PI * PI
local DAYS_PER_YEAR = 365.24

local function body(x, y, z, vx, vy, vz, mass)
  return {x=x, y=y, z=z, vx=vx, vy=vy, vz=vz, mass=mass}
end

local bodies = {
  body(0,0,0, 0,0,0, SOLAR_MASS),
  body(4.84, -1.16, -0.10, 0.00166*DAYS_PER_YEAR, 0.00769*DAYS_PER_YEAR, -0.0000691*DAYS_PER_YEAR, 0.000954 * SOLAR_MASS),
  body(8.34, 4.12, -0.40, -0.00276*DAYS_PER_YEAR, 0.00499*DAYS_PER_YEAR, 0.0000230*DAYS_PER_YEAR, 0.000285 * SOLAR_MASS),
  body(12.89, -15.11, -0.22, 0.00296*DAYS_PER_YEAR, 0.00237*DAYS_PER_YEAR, -0.0000296*DAYS_PER_YEAR, 0.0000437 * SOLAR_MASS),
  body(15.38, -25.91, 0.18, 0.00268*DAYS_PER_YEAR, 0.00163*DAYS_PER_YEAR, -0.0000951*DAYS_PER_YEAR, 0.0000516 * SOLAR_MASS),
}

local function offset_momentum(b)
  local px, py, pz = 0, 0, 0
  for i = 1, #b do
    px = px + b[i].vx * b[i].mass
    py = py + b[i].vy * b[i].mass
    pz = pz + b[i].vz * b[i].mass
  end
  b[1].vx = -px / SOLAR_MASS
  b[1].vy = -py / SOLAR_MASS
  b[1].vz = -pz / SOLAR_MASS
end

local function energy(b)
  local e = 0
  for i = 1, #b do
    local bi = b[i]
    e = e + 0.5 * bi.mass * (bi.vx*bi.vx + bi.vy*bi.vy + bi.vz*bi.vz)
    for j = i+1, #b do
      local bj = b[j]
      local dx = bi.x - bj.x
      local dy = bi.y - bj.y
      local dz = bi.z - bj.z
      local d = math.sqrt(dx*dx + dy*dy + dz*dz)
      e = e - bi.mass * bj.mass / d
    end
  end
  return e
end

local function advance(b, dt)
  for i = 1, #b do
    local bi = b[i]
    for j = i+1, #b do
      local bj = b[j]
      local dx = bi.x - bj.x
      local dy = bi.y - bj.y
      local dz = bi.z - bj.z
      local d2 = dx*dx + dy*dy + dz*dz
      local mag = dt / (d2 * math.sqrt(d2))
      bi.vx = bi.vx - dx * bj.mass * mag
      bi.vy = bi.vy - dy * bj.mass * mag
      bi.vz = bi.vz - dz * bj.mass * mag
      bj.vx = bj.vx + dx * bi.mass * mag
      bj.vy = bj.vy + dy * bi.mass * mag
      bj.vz = bj.vz + dz * bi.mass * mag
    end
  end
  for i = 1, #b do
    local bi = b[i]
    bi.x = bi.x + dt * bi.vx
    bi.y = bi.y + dt * bi.vy
    bi.z = bi.z + dt * bi.vz
  end
end

offset_momentum(bodies)
print(string.format("%.9f", energy(bodies)))
for _ = 1, 200000 do advance(bodies, 0.01) end
print(string.format("%.9f", energy(bodies)))
