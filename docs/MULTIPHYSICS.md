# Headless simulation plans

Run a typed experiment in the terminal or web composer with /sim followed by the plan JSON. Narrator tools use the same executor when /stats on is active. Production solvers run in native C23. The optional development benchmark uses Python and psutil.

The [scenario fixtures](../benchmarks/multiphysics-scenarios.json) contain runnable experiments. Copy a plan object after /sim. A minimal example is:

    {
      "neutrino_tool": "sim_plan",
      "name": "drain test",
      "mode": "hypothetical",
      "actions": [
        {"neutrino_tool":"fluid","height":1,"tank_area":2,"outlet_area":0.003,"seconds":10}
      ],
      "queries": [
        {"id":"drained","predicate":"fluid.volume_exceeds","step":1,"threshold":80}
      ]
    }

## Transactions and consequences

The executor restores branch-local physics and stats from a recent checkpoint and replays subsequent visible events. Workspaces without a usable checkpoint replay their event log. It applies the current reply's previously staged edits and executes the plan in order. Physics-only plans work on scratch state without copying the entire story. Inventory and world edits acquire a temporary copy when first needed.

- Hypothetical mode discards all edits. Each benchmark fixture verifies that the workspace bytes stay unchanged.
- Stage mode retains validated mutations for the accepted narrator reply. A manual /sim stage explicitly commits the plan to the active branch. Its precommit report identifies pending edits and the UI then confirms the commit.
- A failed action or unevaluable action condition discards the plan. Its predicates report SIMULATION_FAILED.
- Optional max_actions sets a per-plan limit between 1 and 32. Plans accept at most 16 queries. Individual solvers keep their existing resource limits.
- Optional when on an action evaluates a predicate against the current scratch geometry or an earlier step. Use the same predicate fields as a query without id. Set expect to false to run on a false result. A nonmatching condition skips the action while preserving step numbers.
- Optional continue set to true lets the narrator use the resulting report to select another tool when its tool budget allows it. Otherwise the narrator proceeds to prose.

Reports distinguish TRUE, FALSE, INSUFFICIENT_DATA, UNSUPPORTED, and SIMULATION_FAILED, with evidence where available. A query referencing a skipped step returns insufficient data. Coordinates are metres with Z up. Step references are one-based.

### Checkpoints and resource limits

Use `/sim checkpoint 32` to set the snapshot interval in accepted simulation events. Use `/sim checkpoint off` to clear snapshots and return to full replay, or `/sim checkpoint on` for the default. Each story retains up to eight snapshots, and states requiring more than 256 KiB of estimated snapshot memory fall back to replay. Snapshots carry the mechanical clock, rigid state, spilled water, runoff, geometry, material settings and character stats. The event log persists independently of the snapshots.

`/sim budget` shows the current limits. Set an individual limit with a command such as `/sim budget rays 8192`, or restore defaults with `/sim budget reset`. The limits apply to plans built by the narrator and plans submitted by the player. Measurements include cumulative estimated work, solver steps, simulated duration, rays, CFD cells, soft-body nodes and report bytes, plus estimated peak working and mapped memory for supported solvers. The executor checks modeled work before dispatch and discards an ordered plan that exceeds its limits. Estimates exclude process-wide allocations and GPU driver memory.

Consecutive flow, light and acoustic queries in the same ordered plan can reuse a decoded or freshly solved CFD field. The trace identifies reuse. A scene edit drops the working cache, and the plan releases it when it finishes.

### Persistent CFD fields

Configure a CFD medium with `scene_medium` and advance it with `scene_flow_step`, for example `{"neutrino_tool":"scene_flow_step","seconds":0.25}`. The first advance starts at the medium's initial conditions at zero seconds. Each later advance starts with the saved tracer and velocity grid for that layout on the active branch. `scene_flow`, acoustic attenuation, and optical extinction read the latest grid without advancing it. The story clock and field clock advance independently.

Before a field has been saved, queries still use the configured `scene_medium.seconds` lookahead. Use `scene_flow_reset` to return to that behavior. Replacing the medium or changing the room bounds resets the saved field. Voxel obstacle edits retain the field when the resolved occupancy mask is identical. Geometry that changes that mask invalidates the field. Switching layouts preserves each layout's field.

Hypothetical or rejected plans leave the saved field unchanged. Accepted frames persist through a restart and follow rewinds and sibling branches. `scene_inspect` reports `flow_time_s`, and `/trace` lists the advance interval and solver backend. Use `/sim budget field_bytes VALUE` to bound encoded frame data per plan. The default is 8 MiB. Each accepted advance stores a lossless binary64 frame in the workspace event log. A 24³ frame adds about 576 KiB before JSON metadata. Long runs grow the log, and a frame larger than the general checkpoint limit restores through event replay.

### CFD boundaries and thin geometry

Set `"boundary":"closed"` on a CFD medium to seal the outer domain. This sets normal velocity to zero at the exterior faces, applies zero normal diffusion flux, and keeps interpolation within the grid. The default `"boundary":"periodic"` wraps the domain. The periodic OpenCL pressure kernel remains available for eligible calculations. A closed exterior uses CPU pressure projection, including when the requested backend is GPU.

Use `"boundary":"no_slip"` for stationary exterior walls and `boundaries` to override individual faces. Face keys are `x_min`, `x_max`, `y_min`, `y_max`, `z_min`, and `z_max`. Use one boundary per face. Boundary types are `inflow`, `outflow`, `pressure`, `no_slip`, and `free_slip`. A vent covers the whole face by default. To define a rectangular vent, supply `min_m` and `max_m` in the two room-local axes tangent to the face, in X/Y/Z order with the face's normal axis omitted. For an X face these pairs are Y/Z.

```json
{
  "neutrino_tool": "scene_medium",
  "method": "cfd",
  "boundary": "no_slip",
  "resolution": 12,
  "boundaries": {
    "x_min": {"type": "inflow", "speed_m_s": 0.5, "tracer": 0},
    "x_max": {"type": "outflow"}
  }
}
```

An inlet prescribes inward normal speed and upstream tracer concentration. An outlet prescribes outward normal flow. Supply `speed_m_s` for a fixed exhaust or omit it to balance the remaining inlet flow over the available exhaust area. The solver uses uncovered face cells for the balance and rejects unresolved openings or unbalanced, disconnected compartments. Exterior no-slip enforces zero normal flow and zero tangential velocity at stationary walls. Internal voxel solids use the slip stencil. Ventilated tracer transport accounts for tracer entering and leaving by advection. The trace reports flow rates in m³/s and the tracer balance over the step.

Replace the example outlet with `{"type":"pressure","pressure_pa":0,"backflow_tracer":0}` to solve for its flow. Pressure is exterior gauge pressure in pascals. Each opening cell can independently admit or discharge air, and an entering cell uses the exterior `backflow_tracer` concentration. The incompressible CPU projection uses a reference air density of 1.2 kg/m³. Pair pressure openings with other pressure openings, prescribed inlets, or fixed-speed exhausts. An automatically balanced exhaust requires a room without pressure openings. The `flow.backflow_exceeds` predicate measures total inward flow through pressure openings. The [pressure-opening fixtures](../benchmarks/pressure-openings.json) exercise discharge and reversal.

CPU worker groups divide CFD grids, ray batches, recursive sound and light path evaluation, rigid collision searches, spill updates, runoff grids, and soft-body node integration. The automatic scheduler sizes each group from the workload and available CPUs. It uses grid cells for CFD, ray–object intersections for raycasts, candidate paths and medium sampling for reflections, potential pairs for collision searches, grid cells and spill sources for runoff, and nodes for soft bodies. Small jobs can run on the calling thread. Set `NEUTRINO_PHYSICS_WORKERS` to a number from 1 to 16 to request that many workers, including the caller. It uses fewer when there are fewer independent work units or worker creation fails. Set it to `auto` or unset it to restore automatic selection. Pressure iterations, collision resolution, soft-body spring projection, and scene commits run in order.

With `solid_geometry:true`, the default `"raster":"center"` marks cells whose centers fall inside geometry and reports any boxes that occupy zero cells. Select `"raster":"conservative"` to mark every cell with positive-volume overlap. This resolves thin walls at coarse resolution and can widen a wall or close a sub-cell opening. The [sealed-room fixture](../benchmarks/closed-cfd.json) exercises both exterior modes and both raster rules.

### Character conversations

`/talk CHARACTER | MESSAGE` and `@CHARACTER: MESSAGE` send the addressed character a separate inference request. Share branch-local knowledge using `/knowledge CHARACTER | FACT`. The request receives the addressed conversation, that character's grants and their recorded observations. The narrator keeps its broader view. Character requests have no tools. Grants represent hearsay and retain their source and story time.

### Recursive sound and light

`scene_acoustic`, `acoustic` and `scene_light` accept `max_bounces` from 0 to 4. The default is 1 for measurement tools. The recursive solver mirrors the source across candidate face sequences and checks the reconstructed path against the finite room and object faces. Each segment accounts for distance, air and field loss, transmissive material and occlusion. Each bounce applies the surface's reflectivity. Audio levels and optical power are relative to the source at one metre.

Use `wall_reflectivity` on `scene_light` to set wall power reflectivity, or use `/sim` with a `tune` action to set acoustic wall amplitude reflectivity. `min_level_db` controls the acoustic cutoff and `min_power` controls the optical cutoff. Recursive paths use the CPU. Direct and legacy room paths can still use supported GPU backends. The search considers up to 8,192 recursive candidates and the trace reports when it reaches that limit. This is specular geometry with planar box faces. It models neither diffraction nor diffuse scattering.

Define a receiver for an existing character in the current room.

```json
{"neutrino_tool":"scene_sensor","name":"Mara","listener":[8,2,2],"min_level_db":-45,"min_power":0.00001}
```

To simulate an actual fictional event, use `scene_emit` with a coordinate or `source_entity`.

```json
{"neutrino_tool":"scene_emit","source":[2,2,2],"sound":true,"light":true,"max_bounces":2,"wall_reflectivity":0.8}
```

Emission checks the configured receivers belonging to characters still in the current room. The default is three bounces. Only a detected signal produces a record. Each record retains its branch, recipient, story time, first qualifying arrival and strength. The character learns that they heard a sound or saw light, and whether the arrival was reflected. The character receives no source identity or geometry. `/knowledge CHARACTER` lists their observations with timing. A hypothetical plan can preview detections, and an accepted emission saves them with the turn. A cancelled or failed plan leaves the records untouched. Receiver positions are fixed until changed with `scene_sensor`, and `off:true` removes one.

| Predicate | Operands |
| --- | --- |
| entity.exists | entity by name in the active layout |
| entity.overlaps | entity and target. AABB interiors must overlap |
| entity.moved | entity. Net translation in the last rigid step |
| entity.rotated | entity. Angular path length in radians during the last rigid step |
| entity.contacts | entity and target. Reports the greatest normal impact energy and its time for that target in the last rigid step. Use @room for the room boundary |
| entity.structural_failure | entity. Whether it fractured and the recorded fracture energy and time |
| fluid.leaves_container | entity. Cumulative water volume released by fracture |
| fluid.spill_deposited | source entity. Landed and airborne water, with the deposited volume's centroid and footprint bounds |
| fluid.deposits_on | source entity and target surface. Same evidence, with volume and footprint restricted to that surface |
| fluid.pools_on | entity, target, and threshold in litres. Reports the source's current grid volume on the named surface |
| fluid.depth_exceeds | entity, optional target, and threshold in metres. Reports the deepest cell occupied by that source and its coordinates |
| fluid.volume_exceeds | step and threshold in litres drained |
| fluid.level_exceeds | step and threshold in metres of remaining fluid |
| fluid.concentration_exceeds | step and relative concentration threshold at a scene_flow probe |
| signal.reaches_receiver | step with a supported acoustic or optical path above cutoff |
| signal.arrival_before | step and threshold in milliseconds |
| signal.power_exceeds | step and threshold as strongest received optical power relative to a source at 1 m |
| signal.direct_blocked | step of a sound or light query. Tests geometric blockage of the direct path |
| signal.recursive_reaches_receiver | step of a sound or light query. Tests whether any received path uses at least two reflections |
| flow.porous_speed_exceeds | step with scene_flow, threshold in m/s. Reports mean speed across resolved porous cells and the modeled pore volume |
| flow.time_exceeds | step with scene_flow, threshold in seconds. Reports elapsed time in a saved field |
| flow.inflow_exceeds / flow.outflow_exceeds | step with scene_flow, threshold in m³/s. Reports the resolved inlet or exhaust rate |
| flow.backflow_exceeds | step with scene_flow, threshold in m³/s. Reports total inward flow through pressure openings |
| mass_transfer.transferred_exceeds | step with a mass-transfer tool, threshold in kg. Positive is uptake, negative is release |
| mass_transfer.final_exceeds | step with a mass-transfer tool, threshold in kg. Reports final target mass |

## Shared spatial data

The scene_material tool defines reusable acoustic and optical profiles in the branch. Store acoustic loss_db_per_m and reflectivity, optical transmission, reflectivity, and loss_db_per_m, or both profiles. The scene_place tool can reference that registry with material_id. Existing workspaces and inline properties work without migration. Replacing a registry entry updates subsequent queries of the objects that reference it.

The scene_air tool selects a registered material_id for uniform attenuation in the current room. Use an empty string to clear it. An acoustic query can supply an explicit medium for that calculation. The scene_translate tool moves a named box by a metre offset vector. Acoustic and optical queries take coordinates or the center of named boxes through source_entity and receiver_entity. The endpoint boxes are omitted from the blocker set for that measurement.

## Rigid bodies and brittle fracture

Attach a rigid body to an existing object with scene_body, name, and mass_kg. Optional fields are liquid_l, restitution and velocity in metres per second. Supply dry mass. The model adds 1 kg per litre of carried water and rejects water above the box's exterior volume. Apply scene_constraint with mode fixed, free or rail. A rail requires axis x, y or z and removes velocity on the other axes. Use scene_step with seconds to advance all bodies in the active layout under its configured gravity.

The solver uses 240 Hz substeps with swept AABB pairs, static room boundaries, and normal impulses. It rejects initial overlaps involving moving bodies. Limits are 128 registered boxes, 10 seconds per step and 24 collision events per substep. Solver failures leave the step unchanged. A room's mechanics clock is measured in seconds independently of the story clock. Branch reconstruction replays mechanical events when a suitable checkpoint is unavailable or precedes those events.

The scene_impulse tool takes name, impulse_ns in newton-seconds, and optional offset_m from the body's center in world coordinates. An offset on or inside the body applies angular impulse as well as linear impulse. Free bodies carry world-space angular momentum and a quaternion orientation. The solver uses the rectangular-prism inertia tensor and advances the orientation at each substep. Fixed mounts and rails lock rotation. The shared geometry uses the axis-aligned hull of the oriented prism, and a hull overlap blocks rotation inelastically. Swept contact impulses still act along the translational normal through the center. Corner torque and rolling friction are outside the model.

An object's registered material may specify mechanical.fracture_j. A normal collision with enough relative kinetic energy splits the current hull in two along its longest horizontal axis. The original name identifies the first piece; the second uses the suffix #2. The pieces divide dry mass equally and inherit the post-impact linear velocity. Fracture is persistent and inelastic. Fragments start without spin.

At fracture, 25 equal-volume water parcels sample the jug's oriented footprint and inherit its post-impact linear and tangential velocities. Gravity carries the parcels to upward static surfaces. Side contacts stop the corresponding velocity component. The parcels and their landing surfaces are stored in the room on the same branch as the fracture. The scene_spill tool accepts entity and optional target to inspect the initial landing volume, centroid and footprint.

The [original falling container](../benchmarks/falling-container.json) fixture remains a regression control. The [rotating container](../benchmarks/rotating-container.json) adds balanced off-center impulses and two named floor sections. Both fixtures measure optical and acoustic paths from the final impact location. These paths test propagation to receiver coordinates. Detection, source identification and private knowledge require additional character models.

End the current player turn with Simulation setup: followed by a complete hypothetical sim_plan to attach a long experiment. The model can select it with the short object {"neutrino_tool":"sim_plan"}. The simulator validates and runs the supplied actions. Stage-mode plans cannot be attached this way. The raw trace records the selection and the executed actions.

## Surface runoff and pooling

Use scene_runoff to enable a bounded surface grid before or after water lands. Supply min_xy and max_xy in world metres and cells as two integer dimensions from 1 to 64. conductivity_m2_s controls the relaxation rate and defaults to 0.025. Optional slope gives elevation change per horizontal metre on each axis. The grade affects runoff rather than the ballistic collision geometry. Each cell uses the highest static surface beneath its center, or the room floor. The grid uses impermeable edges. Grid dimensions and terrain grade remain fixed once water has entered the grid.

During scene_step, landed parcels enter the grid once on their recorded surface. At a grid seam, the model can route a parcel to an adjacent cell on the matching surface. Parcels outside the grid or below its sampled surface remain accounted for as unrouted. The solver moves water between four-connected cells using the difference in bed elevation plus water depth. Each face transfers an equal volume out of one cell and into the next. The solver limits outflow to the water available and subdivides the timestep for stability. Multiple spill sources share the same pressure head and retain their source volumes as they mix. Depth is total water volume divided by cell area.

The scene_puddles tool accepts entity and optional target. It reports released, airborne, unrouted, grid and selected volumes, the deepest cell and its depth, and wet area above the reported cutoff. The fluid.deposits_on predicate continues to describe initial landing. Use fluid.pools_on or fluid.depth_exceeds for the current state.

This is a mass-conserving heightfield approximation. It models gravity-driven movement toward low cells and depth equalization. Its conductivity is an adjustable model parameter. It omits surface momentum, multiple overlapping floor levels, absorption, drains, moving-fragment coupling and post-landing splash.

## Voxel continuum

Set method to cfd and solid_geometry to true in scene_medium to use the room's boxes as zero-normal-flux voxel obstacles. The CPU solver closes adjacent velocity faces and uses obstacle-aware pressure and diffusion stencils. The outer numerical domain remains periodic. Boxes are rasterized by cell center, so thin geometry below grid resolution can be missed. An entirely unresolved interface is reported as unavailable. The scene_flow tool reads the configured medium at a world-space probe. Sound and light queries with gas losses can use the same solved field. Each query currently solves the field again.

## Porous flow and material uptake

Each model accepts explicit parameters with SI units. Run the [material transport fixture](../benchmarks/material-transport.json) through /sim to exercise all four models and inspect their typed results.

| Tool | Parameters and model |
| --- | --- |
| Flow3D or scene_medium with porous | Set method to cfd and supply positive kinematic viscosity via viscosity in m²/s. The porous object takes permeability_m2 and porosity, plus optional room-relative min and max in metres and drive_m_s2. The default region fills the domain. Velocity evolves under additional linear Darcy drag with coefficient viscosity divided by permeability. Porosity determines reported pore volume. |
| gas_liquid_absorption | Supply gas_volume_m3, liquid_volume_m3, gas_kg_m3, optional liquid_kg_m3, partition, kla_s_inv, and seconds. The partition is equilibrium liquid concentration divided by gas concentration. The two well-mixed compartments conserve dilute solute and approach the Henry equilibrium exponentially. |
| vapor_sorption | Supply dry_mass_kg, relative_humidity as a fraction from 0 to 1, capacity_kg_per_kg, affinity, rate_s_inv, optional initial_kg_per_kg, and seconds. Uses a Langmuir isotherm with first-order approach to equilibrium at fixed humidity and temperature. The vapor source has prescribed humidity. |
| polymer_moisture_uptake | Supply dry_mass_kg, full thickness_m, diffusivity_m2_s, surface_kg_per_kg, optional initial_kg_per_kg, and seconds. Uses the average Fickian uptake of a flat sheet held at the same fixed moisture loading on both faces. Diffusivity and sheet dimensions remain fixed. |

Porous CFD uses the existing incompressible pressure solver with a voxelized linear drag term. It does not resolve individual pores, porosity-weighted pressure or tracer storage, saturation, capillary pressure, or nonlinear drag. Regions thinner than the grid can be unavailable. Porous queries report that failure explicitly. The analytical mass-transfer tools report signed transfer and equilibrium as well as final mass. They run as independent experiments with prescribed inputs. They do not automatically change a room's humidity or tracking ledger.

The model conventions follow [Brinkman momentum-drag theory](https://doc.comsol.com/6.4/doc/com.comsol.help.edecm/edecm_ug_fluidflow.09.128.html), [IUPAC's dimensionless Henry solubility definition](https://goldbook.iupac.org/terms/view/09018/plain), [Langmuir's adsorption model](https://doi.org/10.1021/ja02242a004), and [Crank's plane-sheet diffusion solutions](https://people.bath.ac.uk/ensdasr/PAPERS/Crank-The-Mathematics-of-Diffusion.pdf).

## Capability and validation scope

| Area | Status |
| --- | --- |
| State and orchestration | Shared branch-local AABB geometry and propagation materials, ordered and conditional transactions, analytical and numerical queries |
| Mechanics | Quaternion rotation, off-center impulse, swept AABB translation, fixed mounts, rails and persistent two-piece brittle fracture. Contact torque, friction, joint networks and compound shapes are unfinished |
| Propagation | Existing CPU, compute and hardware RT visibility paths. Acoustic and optical queries share geometry, material profiles and named endpoints. The supported paths are direct and first-bounce with material attenuation and cutoffs |
| Continuum | Ballistic water deposition, heightfield runoff, pooling, periodic CFD, voxel obstacles, linear porous drag, and analytical mass transfer. Momentum-resolving free-surface simulation, phase transitions, and two-way fluid-solid coupling are unfinished |
| Character perception | Signal reception predicates describe the solver's receiver. Character knowledge records, access controls and isolated character inference are unfinished |
| Runtime scaling | Existing GPU and Windows mapped-buffer paths. Obstacle pressure projection runs on CPU. The plan enforces action and query counts. Global time and byte budgeting, persistent solver fields and out-of-core tiled scheduling are unfinished |

The C regression suite checks geometric flight time, analytical drainage, conditional execution, transaction failure, branch anchors, and a voxel diffusion case with zero velocity and conserved tracer. A detected signal means a modeled path crossed the solver threshold. Wave diffraction, intelligibility, and finite-element accuracy are outside the model.

## Run and measure

    cmake --build BUILD --config Release
    ctest --test-dir BUILD -C Release --output-on-failure
    python benchmarks/multiphysics.py BUILD/neutrino --fixtures benchmarks/multiphysics-scenarios.json --out results.json
    BUILD/scene_physics_test --rigid-timing

Use the Windows executable path on Windows. Add --provider http://127.0.0.1:PORT/v1 and --model MODEL for an isolated live narration test. Use --fixtures benchmarks/falling-container.json and --live-plan "falling water container" to reproduce the attached-plan test. The benchmark records raw timings, exact reports and persistence checks. The existing scene_physics_test --rt and scene_physics_test --cfd-gpu modes check supported hardware paths against CPU results.

For the spin and water deposition run, use --fixtures benchmarks/rotating-container.json and --live-plan "falling water container with spin".

For the runoff run, use --fixtures benchmarks/runoff-container.json and --live-plan "falling water container with runoff".
