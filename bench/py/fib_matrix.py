import time
M = 1000000007
def fib_mat(n):
    a00, a01, a10, a11 = 1, 1, 1, 0
    r00, r01, r10, r11 = 1, 0, 0, 1
    while n > 0:
        if n & 1:
            r00, r01, r10, r11 = (r00*a00+r01*a10)%M, (r00*a01+r01*a11)%M, (r10*a00+r11*a10)%M, (r10*a01+r11*a11)%M
        a00, a01, a10, a11 = (a00*a00+a01*a10)%M, (a00*a01+a01*a11)%M, (a10*a00+a11*a10)%M, (a10*a01+a11*a11)%M
        n >>= 1
    return r01
n = 100_000_000
t0 = time.perf_counter()
v = 0
for _ in range(50000):
    v += fib_mat(n)
t1 = time.perf_counter()
print(f"verify {v}")
print(f"ms {int((t1-t0)*1000)}")
