# CFD geometry and closed boundaries

Baseline `39e6105`. The Windows C23 Release build used `-O3` on a Ryzen 5 9600X.

## Physical checks

The loopback web fixture ran 11 ordered actions on an 8³ grid and returned four typed probe measurements. The hypothetical plan preserved the workspace bytes on every run.

| Probe | Variant | Relative tracer |
| --- | --- | ---: |
| Opposite exterior edge after 2 s | Periodic | 0.127903 |
| Opposite exterior edge after 2 s | Closed | 3.75 × 10⁻¹⁸ |
| Behind a 0.1 m wall after 10 s | Center raster | 0.002968 |
| Behind a 0.1 m wall after 10 s | Conservative raster | 9.15 × 10⁻²⁴ |

Center rasterization reported one unresolved obstacle in the thin-wall case. Conservative rasterization reported zero. The native tests also checked tracer conservation during pure diffusion, zero normal wall velocity, and reduced divergence after projection. On one branch a baffle moved 0.3 m within the same voxel and retained the 0.25 s field. On a sibling branch it moved 1 m into another voxel and invalidated the field. Both outcomes held after serialization and reload. [Typed measurements and web timings](sealed-room.json)

## Raster benchmark

At 24³ cells, box-range rasterization reproduced the previous center-sampling mask byte for byte. The timings are medians of five batch means for rasterization alone. Flow projection, transport, and UI costs are outside these measurements.

| Boxes | Previous cell scan | Box ranges |
| ---: | ---: | ---: |
| 8 | 130 µs | 0.8 µs |
| 32 | 450 µs | 2.9 µs |
| 128 | 1,810 µs | 13 µs |

All five native test targets passed on Windows CPU, Windows Vulkan RT, and Ubuntu with ASan, LSan, and UBSan. Hardware ray tracing and periodic OpenCL pressure retained their CPU parity checks. Closed pressure projection uses the CPU. Conservative rasterization expands a wall to its overlapping voxels, so it can close openings smaller than the grid. The sealed exterior uses zero normal velocity and diffusion flux. Inflow, outflow, and no-slip wall conditions remain open. [Raw raster timings](raster-timing.csv)

## Reproduction

```sh
ctest --test-dir build --output-on-failure
build/scene_physics_test --raster-timing
python benchmarks/multiphysics.py build/neutrino --fixtures benchmarks/closed-cfd.json --out sealed-room.json --reps 7
```
