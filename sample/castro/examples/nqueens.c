// N-queens — count solutions for an N×N board.
// Stresses: deep recursion + array R/W + nested control flow.

int board[20];

int safe(int row, int col) {
    for (int i = 0; i < row; i++) {
        int b = board[i];
        if (b == col) return 0;
        if (b - i == col - row) return 0;
        if (b + i == col + row) return 0;
    }
    return 1;
}

int solve(int row, int n) {
    if (row == n) return 1;
    int count = 0;
    for (int col = 0; col < n; col++) {
        if (safe(row, col)) {
            board[row] = col;
            count += solve(row + 1, n);
        }
    }
    return count;
}

int main() {
    return solve(0, 11) & 0xFF;   // 11-queens has 2680 solutions
}
