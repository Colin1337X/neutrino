# Automatic physics scheduling in Neutrino v0.18.0

The scheduler uses workload size and the CPUs available to the process. On this Windows x64 host (Ryzen 5 9600X with six logical processors visible, Zig 0.15.2, C23 `-O3`), it selected four workers for the 24³ prescribed exhaust and two for both dense reflector queries. It selected one worker for the 8³ and 12³ CFD grids and for the sparse reflector scene.

| Mode | Prescribed exhaust | Dense sound | Dense light |
| --- | ---: | ---: | ---: |
| Auto | 93.92 ms (4) | 94.75 µs (2) | 77.15 µs (2) |
| Manual 1 | 180.63 ms (1) | 104.59 µs (1) | 91.33 µs (1) |
| Manual 2 | 103.47 ms (2) | 91.56 µs (2) | 78.26 µs (2) |
| Manual 4 | 92.07 ms (4) | 99.10 µs (4) | 84.77 µs (4) |
| Manual 8 | 137.52 ms (8) | 111.56 µs (8) | 90.79 µs (8) |

Parentheses show the actual worker count. The manual 8 run scheduled eight software workers on six logical processors. Each number is the median of 15 batch means from three sequential trials with rotated mode order. Each trial begins with a warm-up batch, and timed batches contain two CFD solves or 400 reflector queries.

The proof output covers the complete serialized CFD field, flow metrics, full sound and light traces, and typed readings. It was byte-identical with the environment variable unset, set to `auto`, and set to each manual count tested. The SHA-256 also matches the v0.17.2 Windows proof for these fixtures.

```powershell
foreach ($mode in 'auto','1','2','4','8') {
    $env:NEUTRINO_PHYSICS_WORKERS = $mode
    ./build/scene_physics_test.exe --pressure-timing prescribed
    ./build/scene_physics_test.exe --propagation-timing dense
    ./build/scene_physics_test.exe --scaling-proof
}
```
