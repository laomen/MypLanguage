import time
n = 800000
seed = 24680
t0 = time.perf_counter()
rng = seed
a = []
for _ in range(n):
    rng = (rng * 1103515245 + 12345) % 2147483648
    a.append(rng)
a.sort()
v = a[n // 2] + sum(a)
t1 = time.perf_counter()
print(f"verify {v}")
print(f"ms {int((t1-t0)*1000)}")
