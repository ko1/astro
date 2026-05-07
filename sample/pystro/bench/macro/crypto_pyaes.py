"""Pure-Python AES-CTR encryption benchmark (pyaes 1.6.1).

Adapted from pyperformance bm_crypto_pyaes.
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__)))
import pyaes

CLEARTEXT = b"This is a test. What could possibly go wrong? " * 500
KEY = b'\xa1\xf6%\x8c\x87}_\xcd\x89dHE8\xbf\xc9,'

def bench(loops):
    for _ in range(loops):
        aes = pyaes.AESModeOfOperationCTR(KEY)
        ciphertext = aes.encrypt(CLEARTEXT)
        aes = pyaes.AESModeOfOperationCTR(KEY)
        plaintext = aes.decrypt(ciphertext)
    if plaintext != CLEARTEXT:
        raise Exception("decrypt error")

if __name__ == "__main__":
    LOOPS = 8
    t0 = time.time()
    bench(LOOPS)
    t1 = time.time()
    print("ok loops=", LOOPS, "elapsed=", round(t1 - t0, 3))
