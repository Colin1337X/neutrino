# Physics worker scaling in Neutrino v0.17.2

Windows x64, AMD Ryzen 5 9600X with six logical processors visible to Windows, Zig 0.15.2, C23 Release at `-O3`. The CFD benchmark advances one simulated second in a 4 × 3 × 2 m room on a 24³ grid. It uses a 0.5 m/s prescribed inlet, an automatically balanced exhaust, and no-slip exterior walls. The reflector benchmark uses the relay room, two baffles, and 12 added reflectors. It traces up to four bounces for sound and light.

| CPU workers | Prescribed exhaust | Dense sound | Dense light |
| ---: | ---: | ---: | ---: |
| 1 | 181.19 ms | 107.35 µs | 95.30 µs |
| 2 | 105.54 ms | 97.00 µs | 75.42 µs |
| 4 | 96.15 ms | 96.60 µs | 81.32 µs |
| 8 | 134.84 ms | 93.50 µs | 77.37 µs |

Each number is the median of 15 batch means from three sequential trials with rotated worker-count order and a warm-up batch per trial. Batches contain two CFD solves or 400 reflector queries. The 8-worker run scheduled eight software lanes on six logical processors.

The CFD solver executes two Jacobi sweeps per dispatch for closed grids without voxel obstacles. Each worker reuses three scratch planes and recomputes its adjacent planes locally. This halves pressure sweep rendezvous and removes generic neighbor lookup for interior cells. The fresh v0.17.1 baseline took 395.28 ms at one worker and 184.03 ms at four. That rerun already showed a four-worker speedup. The v0.17.2 changes improved both measurements further.

Recursive propagation sends all candidate depths in one dispatch. Tiles retain accepted paths and merge them in original candidate order. The search can reject a path before intersection work when its final face points away from the listener. Windows short batches reuse one thread-pool work object per dispatch group. The refreshed v0.17.1 four-worker benchmark measured 250.00 µs for sound and 225.00 µs for light.

All worker counts emitted the same 592,118 bytes in the verification run. This covers the complete serialized velocity and tracer grids, exact printed CFD summaries, complete sound and light traces, and their typed readings. The exhaust carried 3 m³/s in and 3 m³/s out. Both reflector queries retained four recursive arrivals. CTest checks the same output across all four worker counts.

```powershell
foreach ($workers in 1, 2, 4, 8) {
    $env:NEUTRINO_PHYSICS_WORKERS = "$workers"
    ./build/scene_physics_test.exe --pressure-timing prescribed
    ./build/scene_physics_test.exe --propagation-timing dense
    ./build/scene_physics_test.exe --scaling-proof
}
```
