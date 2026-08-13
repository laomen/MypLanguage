import time
n = 300
t0 = time.perf_counter()
s = 0
for i in range(n):
    for j in range(n):
        for k in range(n):
            s += (i * 101 + j * 97 + k * 89) % 997
t1 = time.perf_counter()
print(f"verify {s}")
print(f"ms {int((t1-t0)*1000)}")
