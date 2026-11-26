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

The executor reconstructs branch-local physics and stats from visible events. It applies the current reply's previously staged edits and executes the plan in order. Physics-only plans work on scratch state without copying the entire story. Inventory and world edits acquire a temporary copy when first needed.

- Hypothetical mode discards all edits. Each benchmark fixture verifies that the workspace bytes stay unchanged.
- Stage mode retains validated mutations for the accepted narrator reply. A manual /sim stage explicitly commits the plan to the active branch. Its precommit report identifies pending edits and the UI then confirms the commit.
- A failed action or unevaluable action condition discards the plan. Its predicates report SIMULATION_FAILED.
- Optional max_actions sets a per-plan limit between 1 and 32. Plans accept at most 16 queries. Individual solvers keep their existing resource limits.
- Optional when on an action evaluates a predicate against the current scratch geometry or an earlier step. Use the same predicate fields as a query without id. Set expect to false to run on a false result. A nonmatching condition skips the action while preserving step numbers.
- Optional continue set to true lets the narrator use the resulting report to select another tool when its tool budget allows it. Otherwise the narrator proceeds to prose.

Reports distinguish TRUE, FALSE, INSUFFICIENT_DATA, UNSUPPORTED, and SIMULATION_FAILED, with evidence where available. A query referencing a skipped step returns insufficient data. Coordinates are metres with Z up. Step references are one-based.

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

## Shared spatial data

The scene_material tool defines reusable acoustic and optical profiles in the branch. Store acoustic loss_db_per_m and reflectivity, optical transmission, reflectivity, and loss_db_per_m, or both profiles. The scene_place tool can reference that registry with material_id. Existing workspaces and inline properties work without migration. Replacing a registry entry updates subsequent queries of the objects that reference it.

The scene_air tool selects a registered material_id for uniform attenuation in the current room. Use an empty string to clear it. An acoustic query can supply an explicit medium for that calculation. The scene_translate tool moves a named box by a metre offset vector. Acoustic and optical queries take coordinates or the center of named boxes through source_entity and receiver_entity. The endpoint boxes are omitted from the blocker set for that measurement.

## Rigid bodies and brittle fracture

Attach a rigid body to an existing object with scene_body, name, and mass_kg. Optional fields are liquid_l, restitution and velocity in metres per second. Supply dry mass. The model adds 1 kg per litre of carried water and rejects water above the box's exterior volume. Apply scene_constraint with mode fixed, free or rail. A rail requires axis x, y or z and removes velocity on the other axes. Use scene_step with seconds to advance all bodies in the active layout under its configured gravity.

The solver uses 240 Hz substeps with swept AABB pairs, static room boundaries, and normal impulses. It rejects initial overlaps involving moving bodies. Limits are 128 registered boxes, 10 seconds per step and 24 collision events per substep. Solver failures leave the step unchanged. A room's mechanics clock is measured in seconds independently of the story clock. The current implementation replays committed steps when reconstructing branch state.

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

## Capability and validation scope

| Area | Status |
| --- | --- |
| State and orchestration | Shared branch-local AABB geometry and propagation materials, ordered and conditional transactions, analytical and numerical queries |
| Mechanics | Quaternion rotation, off-center impulse, swept AABB translation, fixed mounts, rails and persistent two-piece brittle fracture. Contact torque, friction, joint networks and compound shapes are unfinished |
| Propagation | Existing CPU, compute and hardware RT visibility paths. Acoustic and optical queries share geometry, material profiles and named endpoints. The supported paths are direct and first-bounce with material attenuation and cutoffs |
| Continuum | Ballistic water deposition, heightfield runoff and pooling, periodic CFD and opt-in CPU voxel obstacles. Momentum-resolving free-surface simulation, phase transitions, and two-way fluid-solid coupling are unfinished |
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
