// md5 (RFC 1321) — bit-twiddling benchmark.
// Stresses: 32-bit ops (rotates, XOR, AND, OR), small fixed table lookups.

void *malloc(unsigned long n);
void free(void *p);

typedef unsigned int u32;

static const u32 K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const int S[64] = {
     7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
     5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
     4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
     6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

// castro stores `unsigned int` in an 8-byte slot, so 32-bit wrap-around
// has to be done explicitly with `& M32` (gcc on host wraps for free
// because u32 really is 32 bits there).  Mask after every op that
// could overflow.
#define M32 0xffffffffu

u32 rotl(u32 x, int s) {
    x = x & M32;
    return ((x << s) | (x >> (32 - s))) & M32;
}

void md5_block(u32 *state, u32 *block) {
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        u32 f, g;
        if (i < 16)       { f = (b & c) | ((~b) & d);    g = i; }
        else if (i < 32)  { f = (d & b) | ((~d) & c);    g = (5 * i + 1) & 15; }
        else if (i < 48)  { f = b ^ c ^ d;               g = (3 * i + 5) & 15; }
        else              { f = c ^ (b | (~d));          g = (7 * i) & 15; }
        u32 tmp = d;
        d = c;
        c = b;
        b = (b + rotl((a + f + K[i] + block[g]) & M32, S[i])) & M32;
        a = tmp;
    }
    state[0] = (state[0] + a) & M32;
    state[1] = (state[1] + b) & M32;
    state[2] = (state[2] + c) & M32;
    state[3] = (state[3] + d) & M32;
}

#define MSG_LEN 64
#define BLOCKS 1
#define REPS 800000

unsigned char msg[MSG_LEN];

int main() {
    for (int i = 0; i < MSG_LEN; i++) msg[i] = (unsigned char)((i * 37 + 11) & 0xff);
    u32 state[4];
    u32 block[16];
    u32 chk = 0;
    for (int rep = 0; rep < REPS; rep++) {
        state[0] = 0x67452301u;
        state[1] = 0xefcdab89u;
        state[2] = 0x98badcfeu;
        state[3] = 0x10325476u;
        // Vary message slightly each iter so the work isn't repeated work.
        // NB explicit `& 0xff` because castro's slot-based representation
        // doesn't truncate on assignment to a narrower integer type.
        msg[rep & (MSG_LEN - 1)] = (unsigned char)((rep * 0x9e + 17) & 0xff);
        for (int i = 0; i < 16; i++) {
            int p = i * 4;
            block[i] = msg[p] | (msg[p+1] << 8) | (msg[p+2] << 16) | (msg[p+3] << 24);
        }
        md5_block(state, block);
        chk = (chk * 31 + state[0] + state[1] + state[2] + state[3]) & M32;
    }
    return chk & 0xff;
}
