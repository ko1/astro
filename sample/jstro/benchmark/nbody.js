// nbody — based on the Computer Language Benchmarks Game version.
// Heavy on object property access in a tight loop.

var PI = 3.141592653589793;
var SOLAR_MASS = 4 * PI * PI;
var DAYS_PER_YEAR = 365.24;

function Body(x, y, z, vx, vy, vz, mass) {
  this.x = x; this.y = y; this.z = z;
  this.vx = vx; this.vy = vy; this.vz = vz;
  this.mass = mass;
}

function Jupiter() {
  return new Body(
    4.84143144246472090e+00,
    -1.16032004402742839e+00,
    -1.03622044471123109e-01,
    1.66007664274403694e-03 * DAYS_PER_YEAR,
    7.69901118419740425e-03 * DAYS_PER_YEAR,
    -6.90460016972063023e-05 * DAYS_PER_YEAR,
    9.54791938424326609e-04 * SOLAR_MASS
  );
}
function Saturn() {
  return new Body(
    8.34336671824457987e+00,
    4.12479856412430479e+00,
    -4.03523417114321381e-01,
    -2.76742510726862411e-03 * DAYS_PER_YEAR,
    4.99852801234917238e-03 * DAYS_PER_YEAR,
    2.30417297573763929e-05 * DAYS_PER_YEAR,
    2.85885980666130812e-04 * SOLAR_MASS
  );
}
function Uranus() {
  return new Body(
    1.28943695621391310e+01,
    -1.51111514016986312e+01,
    -2.23307578892655734e-01,
    2.96460137564761618e-03 * DAYS_PER_YEAR,
    2.37847173959480950e-03 * DAYS_PER_YEAR,
    -2.96589568540237556e-05 * DAYS_PER_YEAR,
    4.36624404335156298e-05 * SOLAR_MASS
  );
}
function Neptune() {
  return new Body(
    1.53796971148509165e+01,
    -2.59193146099879641e+01,
    1.79258772950371181e-01,
    2.68067772490389322e-03 * DAYS_PER_YEAR,
    1.62824170038242295e-03 * DAYS_PER_YEAR,
    -9.51592254519715870e-05 * DAYS_PER_YEAR,
    5.15138902046611451e-05 * SOLAR_MASS
  );
}
function Sun() {
  return new Body(0, 0, 0, 0, 0, 0, SOLAR_MASS);
}

function offsetMomentum(bodies) {
  var px = 0, py = 0, pz = 0;
  for (var i = 0; i < bodies.length; i++) {
    var b = bodies[i];
    px += b.vx * b.mass;
    py += b.vy * b.mass;
    pz += b.vz * b.mass;
  }
  bodies[0].vx = -px / SOLAR_MASS;
  bodies[0].vy = -py / SOLAR_MASS;
  bodies[0].vz = -pz / SOLAR_MASS;
}

function advance(bodies, dt) {
  var n = bodies.length;
  for (var i = 0; i < n; i++) {
    var bi = bodies[i];
    for (var j = i + 1; j < n; j++) {
      var bj = bodies[j];
      var dx = bi.x - bj.x;
      var dy = bi.y - bj.y;
      var dz = bi.z - bj.z;
      var d2 = dx*dx + dy*dy + dz*dz;
      var mag = dt / (d2 * Math.sqrt(d2));
      bi.vx -= dx * bj.mass * mag;
      bi.vy -= dy * bj.mass * mag;
      bi.vz -= dz * bj.mass * mag;
      bj.vx += dx * bi.mass * mag;
      bj.vy += dy * bi.mass * mag;
      bj.vz += dz * bi.mass * mag;
    }
  }
  for (var i = 0; i < n; i++) {
    var b = bodies[i];
    b.x += dt * b.vx;
    b.y += dt * b.vy;
    b.z += dt * b.vz;
  }
}

function energy(bodies) {
  var e = 0;
  var n = bodies.length;
  for (var i = 0; i < n; i++) {
    var bi = bodies[i];
    e += 0.5 * bi.mass * (bi.vx*bi.vx + bi.vy*bi.vy + bi.vz*bi.vz);
    for (var j = i + 1; j < n; j++) {
      var bj = bodies[j];
      var dx = bi.x - bj.x;
      var dy = bi.y - bj.y;
      var dz = bi.z - bj.z;
      e -= bi.mass * bj.mass / Math.sqrt(dx*dx + dy*dy + dz*dz);
    }
  }
  return e;
}

var bodies = [Sun(), Jupiter(), Saturn(), Uranus(), Neptune()];
offsetMomentum(bodies);

var n = 100000;  // tune for ~1s sustained scale
var start = Date.now();
var e0 = energy(bodies);
for (var i = 0; i < n; i++) advance(bodies, 0.01);
var e1 = energy(bodies);
var elapsed = (Date.now() - start) / 1000;
console.log("e0 = " + e0);
console.log("e1 = " + e1);
console.log("steps = " + n + ", elapsed = " + elapsed + "s");
