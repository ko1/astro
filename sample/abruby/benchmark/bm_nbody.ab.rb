# N-body simulation (Float + object + method dispatch)
class Body
  def initialize(x, y, z, vx, vy, vz, mass)
    @x = x
    @y = y
    @z = z
    @vx = vx
    @vy = vy
    @vz = vz
    @mass = mass
  end
  def x = @x
  def y = @y
  def z = @z
  def vx = @vx
  def vy = @vy
  def vz = @vz
  def mass = @mass
  def set_vx(v)
    @vx = v
  end
  def set_vy(v)
    @vy = v
  end
  def set_vz(v)
    @vz = v
  end
  def move(dt)
    @x = @x + dt * @vx
    @y = @y + dt * @vy
    @z = @z + dt * @vz
  end
end

def advance(bodies, dt)
  i = 0
  nbodies = bodies.length
  while i < nbodies
    bi = bodies[i]
    j = i + 1
    while j < nbodies
      bj = bodies[j]
      dx = bi.x - bj.x
      dy = bi.y - bj.y
      dz = bi.z - bj.z
      dsq = dx * dx + dy * dy + dz * dz
      dist = dsq ** 0.5
      mag = dt / (dsq * dist)
      bi.set_vx(bi.vx - dx * bj.mass * mag)
      bi.set_vy(bi.vy - dy * bj.mass * mag)
      bi.set_vz(bi.vz - dz * bj.mass * mag)
      bj.set_vx(bj.vx + dx * bi.mass * mag)
      bj.set_vy(bj.vy + dy * bi.mass * mag)
      bj.set_vz(bj.vz + dz * bi.mass * mag)
      j += 1
    end
    i += 1
  end
  i = 0
  while i < nbodies
    bodies[i].move(dt)
    i += 1
  end
end

pi = 3.141592653589793
solar_mass = 4.0 * pi * pi
days_per_year = 365.24

bodies = [
  Body.new(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, solar_mass),
  Body.new(4.84143144246472090, -1.16032004402742839, -0.103622044471123109,
           0.00166007664274403694 * days_per_year, 0.00769901118419740425 * days_per_year,
           -0.0000690460016972063023 * days_per_year, 0.000954791938424326609 * solar_mass),
  Body.new(8.34336671824457987, 4.12479856412430479, -0.403523417114321381,
           -0.00276742510726862411 * days_per_year, 0.00499852801234917238 * days_per_year,
           0.0000230417297573763929 * days_per_year, 0.000285885980666130812 * solar_mass),
  Body.new(12.8943695621391310, -15.1111514016986312, -0.223307578892655734,
           0.00296460137564761618 * days_per_year, 0.00237847173959480950 * days_per_year,
           -0.0000296589568540237556 * days_per_year, 0.0000436624404335156298 * solar_mass),
  Body.new(-15.7592624220987730, -25.0653184130168400, 0.179258772950371181,
           0.00268067772490389322 * days_per_year, 0.00162224685042289360 * days_per_year,
           -0.0000951592254519715870 * days_per_year, 0.0000515138902046611451 * solar_mass)
]

i = 0
while i < 30000
  advance(bodies, 0.01)
  i += 1
end
# Print x of sun as checksum
p(bodies[0].x)
