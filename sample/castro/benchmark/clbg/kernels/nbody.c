// Computer Language Benchmarks Game — nbody (5-body solar system).
// Stresses: tight FP-arith inner loop, sqrt via Newton iter (no <math.h>),
// small-array indexed access.

#define N_BODIES 5
#define STEPS 1000000
#define DT 0.01

double x[N_BODIES], y[N_BODIES], z[N_BODIES];
double vx[N_BODIES], vy[N_BODIES], vz[N_BODIES];
double mass[N_BODIES];

#define PI 3.141592653589793
#define SOLAR_MASS (4 * PI * PI)
#define DAYS_PER_YEAR 365.24

void init_bodies(void) {
    // sun
    x[0] = 0; y[0] = 0; z[0] = 0;
    vx[0] = 0; vy[0] = 0; vz[0] = 0;
    mass[0] = SOLAR_MASS;
    // jupiter
    x[1] = 4.84143144246472090; y[1] = -1.16032004402742839; z[1] = -0.103622044471123109;
    vx[1] = 0.00166007664274403694 * DAYS_PER_YEAR;
    vy[1] = 0.00769901118419740425 * DAYS_PER_YEAR;
    vz[1] = -0.0000690460016972063023 * DAYS_PER_YEAR;
    mass[1] = 0.000954791938424326609 * SOLAR_MASS;
    // saturn
    x[2] = 8.34336671824457987; y[2] = 4.12479856412430479; z[2] = -0.403523417114321381;
    vx[2] = -0.00276742510726862411 * DAYS_PER_YEAR;
    vy[2] = 0.00499852801234917238 * DAYS_PER_YEAR;
    vz[2] = 0.0000230417297573763929 * DAYS_PER_YEAR;
    mass[2] = 0.000285885980666130812 * SOLAR_MASS;
    // uranus
    x[3] = 12.8943695621391310; y[3] = -15.1111514016986312; z[3] = -0.223307578892655734;
    vx[3] = 0.00296460137564761618 * DAYS_PER_YEAR;
    vy[3] = 0.00237847173959480950 * DAYS_PER_YEAR;
    vz[3] = -0.0000296589568540237556 * DAYS_PER_YEAR;
    mass[3] = 0.0000436624404335156298 * SOLAR_MASS;
    // neptune
    x[4] = 15.3796971148509165; y[4] = -25.9193146099879641; z[4] = 0.179258772950371181;
    vx[4] = 0.00268067772490389322 * DAYS_PER_YEAR;
    vy[4] = 0.00162824170038242295 * DAYS_PER_YEAR;
    vz[4] = -0.0000951592254519715870 * DAYS_PER_YEAR;
    mass[4] = 0.0000515138902046611451 * SOLAR_MASS;

    // offset_momentum
    double px = 0, py = 0, pz = 0;
    for (int i = 0; i < N_BODIES; i++) {
        px += vx[i] * mass[i];
        py += vy[i] * mass[i];
        pz += vz[i] * mass[i];
    }
    vx[0] = -px / SOLAR_MASS;
    vy[0] = -py / SOLAR_MASS;
    vz[0] = -pz / SOLAR_MASS;
}

// Newton-Raphson sqrt — castro doesn't link <math.h>'s sqrt.
double my_sqrt(double s) {
    double x_ = s * 0.5;
    if (s == 0) return 0;
    for (int i = 0; i < 20; i++) {
        x_ = 0.5 * (x_ + s / x_);
    }
    return x_;
}

void advance(double dt) {
    for (int i = 0; i < N_BODIES; i++) {
        for (int j = i + 1; j < N_BODIES; j++) {
            double dx = x[i] - x[j];
            double dy = y[i] - y[j];
            double dz = z[i] - z[j];
            double d2 = dx*dx + dy*dy + dz*dz;
            double dist = my_sqrt(d2);
            double mag = dt / (d2 * dist);
            vx[i] -= dx * mass[j] * mag;
            vy[i] -= dy * mass[j] * mag;
            vz[i] -= dz * mass[j] * mag;
            vx[j] += dx * mass[i] * mag;
            vy[j] += dy * mass[i] * mag;
            vz[j] += dz * mass[i] * mag;
        }
    }
    for (int i = 0; i < N_BODIES; i++) {
        x[i] += dt * vx[i];
        y[i] += dt * vy[i];
        z[i] += dt * vz[i];
    }
}

double energy(void) {
    double e = 0.0;
    for (int i = 0; i < N_BODIES; i++) {
        e += 0.5 * mass[i] * (vx[i]*vx[i] + vy[i]*vy[i] + vz[i]*vz[i]);
        for (int j = i + 1; j < N_BODIES; j++) {
            double dx = x[i] - x[j];
            double dy = y[i] - y[j];
            double dz = z[i] - z[j];
            double d = my_sqrt(dx*dx + dy*dy + dz*dz);
            e -= mass[i] * mass[j] / d;
        }
    }
    return e;
}

int main() {
    init_bodies();
    for (int i = 0; i < STEPS; i++) advance(DT);
    // Use a checksum that's stable across implementations even when
    // energy is conserved exactly enough that the simple `(e1-e0)*N`
    // diff truncates to 0.  Sum of int-cast-mod-256 of body coordinates
    // is reproducible if all FP ops give the same bit-for-bit result.
    int s = 0;
    for (int i = 0; i < N_BODIES; i++) {
        s += (int)(x[i] * 1000) + (int)(y[i] * 1000) + (int)(z[i] * 1000);
        s += (int)(vx[i] * 1000) + (int)(vy[i] * 1000) + (int)(vz[i] * 1000);
    }
    return s & 0xff;
}
