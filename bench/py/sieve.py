import time
n = 10_000_000
t0 = time.perf_counter()
isp = [True] * (n + 1)
isp[0] = isp[1] = False
i = 2
while i * i <= n:
    if isp[i]:
        j = i * i
        while j <= n:
            isp[j] = False
            j += i
    i += 1
v = sum(isp)
t1 = time.perf_counter()
print(f"verify {v}")
print(f"ms {int((t1-t0)*1000)}")
