import time
n = 256
A = [i % 1000 for i in range(n * n)]
B = [i % 7 for i in range(n * n)]
C = [0] * (n * n)
t0 = time.perf_counter()
for i0 in range(n):
    for k0 in range(n):
        av = A[i0 * n + k0]
        base_c = i0 * n
        base_b = k0 * n
        for j1 in range(n):
            C[base_c + j1] += av * B[base_b + j1]
trace = 0
for i in range(n):
    trace += C[i * n + i]
t1 = time.perf_counter()
print(f"verify {trace}")
print(f"ms {int((t1-t0)*1000)}")
