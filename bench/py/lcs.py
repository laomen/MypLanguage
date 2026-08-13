import time
n = 2000
seed = 24680
rng = seed
s = []; t = []
for _ in range(n):
    rng = (rng * 1103515245 + 12345) % 2147483648
    s.append((rng >> 16) % 4)
    rng = (rng * 1103515245 + 12345) % 2147483648
    t.append((rng >> 16) % 4)
W = n + 1
dp = [0] * (W * W)
t0 = time.perf_counter()
for i in range(1, n + 1):
    si = s[i - 1]
    for j in range(1, n + 1):
        if si == t[j - 1]:
            dp[i * W + j] = dp[(i - 1) * W + (j - 1)] + 1
        elif dp[(i - 1) * W + j] >= dp[i * W + (j - 1)]:
            dp[i * W + j] = dp[(i - 1) * W + j]
        else:
            dp[i * W + j] = dp[i * W + (j - 1)]
v = dp[n * W + n]
t1 = time.perf_counter()
print(f"verify {v}")
print(f"ms {int((t1-t0)*1000)}")
