# Pressure openings in the ventilated relay room

Windows x64, Ryzen 5 9600X, Zig 0.15.2, C23 Release at `-O3`. Each solve advances one second in a 4 × 3 × 2 m room with a 0.5 m/s full-face inlet and stationary no-slip walls. The exhaust is either automatically balanced or held at 0 Pa exterior gauge pressure. Times are medians of five batch means after warm-up. Each batch runs 16 solves at 8³ and 12³, and two at 24³.

| Grid | Exhaust | One worker | Up to four workers | RMS divergence (s⁻¹) |
| --- | --- | ---: | ---: | ---: |
| 8³ | Prescribed | 1.851 ms | 1.807 ms | 0.01118 |
| 8³ | Pressure | 1.372 ms | 1.592 ms | 8.54 × 10⁻⁷ |
| 12³ | Prescribed | 12.326 ms | 11.905 ms | 0.01701 |
| 12³ | Pressure | 13.398 ms | 12.489 ms | 7.70 × 10⁻⁷ |
| 24³ | Prescribed | 422.432 ms | 289.874 ms | 0.02116 |
| 24³ | Pressure | 716.432 ms | 593.365 ms | 9.29 × 10⁻⁷ |

The 8³ and 12³ grids stay on one CPU worker in both runs. The 24³ grid uses the configured worker limit. Both exhausts pass 3 m³/s to the precision shown in the trace. At 24³, pressure projection reduced the measured divergence residual by about 23,000× and took 2.05× as long as the prescribed exhaust with four workers. Every recorded physical output matched across worker counts.

The web-command fixtures run two hypothetical plans. The prescribed-inlet case restored the pressure field at 1 s, measured 3 m³/s inlet and exhaust, and recorded zero backflow. The reverse-pressure case applied 0.12 Pa more exterior pressure at X-max. After 1 s it admitted 0.15 m³/s through X-max, exhausted 0.15 m³/s through X-min, and recorded relative tracer concentration 0.07437 near X-max and zero at the far probe. Both plans left the workspace unchanged.

`timing.csv` contains individual batches. `web-traces.json` contains the ordered action traces, typed readings, and individual request times.

```powershell
$env:NEUTRINO_PHYSICS_WORKERS = '1'
./build/scene_physics_test.exe --pressure-timing
$env:NEUTRINO_PHYSICS_WORKERS = '4'
./build/scene_physics_test.exe --pressure-timing
python benchmarks/multiphysics.py ./build/neutrino.exe --fixtures benchmarks/pressure-openings.json --out pressure-web.json --reps 7
```
