import time
n = 100_000_000
seed = 24680
t0 = time.perf_counter()
rng = seed
inside = 0
for _ in range(n):
    rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
    x = (rng >> 7) / 16777216.0
    rng = (rng * 1103515245 + 12345) & 0x7FFFFFFF
    y = (rng >> 7) / 16777216.0
    if x * x + y * y <= 1.0:
        inside += 1
t1 = time.perf_counter()
print(f"verify {inside}")
print(f"ms {int((t1-t0)*1000)}")
