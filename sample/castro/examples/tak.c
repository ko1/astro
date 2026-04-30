int tak(int x, int y, int z) {
    if (y < x) {
        return tak(tak(x - 1, y, z),
                   tak(y - 1, z, x),
                   tak(z - 1, x, y));
    }
    return z;
}

int main() {
    return tak(27, 18, 9);
}
