import time, math
n = 5000
steps = 2
px = [(i * 37) % 1000 for i in range(n)]
py = [(i * 53) % 1000 for i in range(n)]
pz = [(i * 71) % 1000 for i in range(n)]
vx = [0.0] * n; vy = [0.0] * n; vz = [0.0] * n
m = [1.0 + (i % 10) * 0.1 for i in range(n)]
t0 = time.perf_counter()
for _ in range(steps):
    for a in range(n):
        ax = ay = az = 0.0
        pxa, pya, pza = px[a], py[a], pz[a]
        for b in range(n):
            if b != a:
                dx = px[b] - pxa; dy = py[b] - pya; dz = pz[b] - pza
                d2 = dx * dx + dy * dy + dz * dz + 1e-6
                inv = 1.0 / (d2 * math.sqrt(d2))
                mm = m[b] * inv
                ax += dx * mm; ay += dy * mm; az += dz * mm
        vx[a] += ax; vy[a] += ay; vz[a] += az
    for i in range(n):
        px[i] += vx[i]; py[i] += vy[i]; pz[i] += vz[i]
ke = 0.0
for i in range(n):
    ke += 0.5 * m[i] * (vx[i] * vx[i] + vy[i] * vy[i] + vz[i] * vz[i])
t1 = time.perf_counter()
print(f"verify {ke:.6f}")
print(f"ms {int((t1-t0)*1000)}")
