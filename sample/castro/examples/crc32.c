// CRC-32 over a synthetic data stream.
// Stresses: tight bit-level loop (& | ^ >> << ~), no memory traffic.

unsigned int crc32(int len) {
    unsigned int crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        unsigned int c = (unsigned int)(i * 7 + 13);
        crc = crc ^ c;
        for (int j = 0; j < 8; j++) {
            unsigned int mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

int main() {
    return (int)(crc32(2500000) & 0xFF);
}
