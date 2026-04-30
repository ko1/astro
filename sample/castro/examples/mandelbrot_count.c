// Mandelbrot iteration counting (no I/O, sums total iterations).
// Returns sum over a w*h grid of escape times (capped at MAX_ITER).
int mandelbrot_count(int w, int h, int max_iter) {
    int total = 0;
    for (int py = 0; py < h; py++) {
        double y0 = -1.0 + 2.0 * (double)py / (double)h;
        for (int px = 0; px < w; px++) {
            double x0 = -2.0 + 3.0 * (double)px / (double)w;
            double x = 0.0, y = 0.0;
            int iter = 0;
            while (x * x + y * y <= 4.0 && iter < max_iter) {
                double xt = x * x - y * y + x0;
                y = 2.0 * x * y + y0;
                x = xt;
                iter++;
            }
            total += iter;
        }
    }
    return total;
}

int main() {
    return mandelbrot_count(200, 150, 1500);
}
