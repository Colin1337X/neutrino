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
| fluid.volume_exceeds | step and threshold in litres drained |
| fluid.level_exceeds | step and threshold in metres of remaining fluid |
| fluid.concentration_exceeds | step and relative concentration threshold at a scene_flow probe |
| signal.reaches_receiver | step with a supported acoustic or optical path above cutoff |
| signal.arrival_before | step and threshold in milliseconds |
| signal.power_exceeds | step and threshold as strongest received optical power relative to a source at 1 m |

## Shared spatial data

The scene_material tool defines reusable acoustic and optical profiles in the branch. Store acoustic loss_db_per_m and reflectivity, optical transmission, reflectivity, and loss_db_per_m, or both profiles. The scene_place tool can reference that registry with material_id. Existing workspaces and inline properties work without migration. Replacing a registry entry updates subsequent queries of the objects that reference it.

The scene_air tool selects a registered material_id for uniform attenuation in the current room. Use an empty string to clear it. An acoustic query can supply an explicit medium for that calculation. The scene_translate tool moves a named box by a metre offset vector. Acoustic and optical queries take coordinates or the center of named boxes through source_entity and receiver_entity. The endpoint boxes are omitted from the blocker set for that measurement.

Set method to cfd and solid_geometry to true in scene_medium to use the room's boxes as zero-normal-flux voxel obstacles. The CPU solver closes adjacent velocity faces and uses obstacle-aware pressure and diffusion stencils. The outer numerical domain remains periodic. Boxes are rasterized by cell center, so thin geometry below grid resolution can be missed. An entirely unresolved interface is reported as unavailable. The scene_flow tool reads the configured medium at a world-space probe. Sound and light queries with gas losses can use the same solved field. Each query currently solves the field again.

## Capability and validation scope

| Area | Status |
| --- | --- |
| State and orchestration | Shared branch-local AABB geometry and propagation materials, ordered and conditional transactions, analytical and numerical queries |
| Mechanics | Box translation and overlap plus existing trajectory and deformation tools. Shared rigid bodies, constraints, compound shapes, and persistent fracture are unfinished |
| Propagation | Existing CPU, compute and hardware RT visibility paths. Acoustic and optical queries share geometry, material profiles and named endpoints. The supported paths are direct and first-bounce with material attenuation and cutoffs |
| Continuum | Periodic CFD and opt-in CPU voxel obstacles. Free-surface simulation, phase transitions, and two-way fluid-solid coupling are unfinished |
| Character perception | Signal reception predicates describe the solver's receiver. Character knowledge records, access controls and isolated character inference are unfinished |
| Runtime scaling | Existing GPU and Windows mapped-buffer paths. Obstacle pressure projection runs on CPU. The plan enforces action and query counts. Global time and byte budgeting, persistent solver fields and out-of-core tiled scheduling are unfinished |

The C regression suite checks geometric flight time, analytical drainage, conditional execution, transaction failure, branch anchors, and a voxel diffusion case with zero velocity and conserved tracer. A detected signal means a modeled path crossed the solver threshold. Wave diffraction, intelligibility, and finite-element accuracy are outside the model.

## Run and measure

    cmake --build BUILD --config Release
    ctest --test-dir BUILD -C Release --output-on-failure
    python benchmarks/multiphysics.py BUILD/neutrino --fixtures benchmarks/multiphysics-scenarios.json --out results.json

Use the Windows executable path on Windows. Add --provider http://127.0.0.1:PORT/v1 and --model MODEL for an isolated live narration test. The benchmark records raw timings, exact reports and persistence checks. The existing scene_physics_test --rt and scene_physics_test --cfd-gpu modes check supported hardware paths against CPU results.
