import time
n = 1_000_000
seed = 24680
rng = seed
a = []; b = []
for _ in range(n):
    rng = (rng * 1103515245 + 12345) % 2147483648
    a.append(((rng >> 16) % 1000) / 100.0)
    rng = (rng * 1103515245 + 12345) % 2147483648
    b.append(((rng >> 16) % 1000) / 100.0)
t0 = time.perf_counter()
s = 0.0
for i in range(n):
    s += a[i] * b[i]
t1 = time.perf_counter()
print(f"verify {s:.6f}")
print(f"ms {int((t1-t0)*1000)}")
