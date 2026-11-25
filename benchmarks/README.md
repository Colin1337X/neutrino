# Neutrino and SillyTavern benchmark

This harness exercises the native terminal, two Neutrino web configurations, and two SillyTavern browser profiles. Child servers listen on isolated loopback ports with independent workspaces and user data. The inference proxy sends scripted output in synthetic mode and forwards requests to an existing OpenAI-compatible provider in live mode.

## Prepare on Windows

1. Build the current Neutrino commit twice in Release mode with CMake, once with CPU physics and once with Vulkan enabled. Provide the executables with the runner flags if they live outside the default paths below.
2. Fetch SillyTavern 1.19.1 with the command below, then run npm ci --omit=dev in its checkout. The release commit used in the result is 82b64a45c34facbc98ff4b9f33c067d3ccab6a17.
3. Install Python 3.10 or newer, Node.js supported by the pinned SillyTavern release, Playwright, and a matching Chromium. psutil and pywinpty are development dependencies of the harness.
4. For live inference, use a stable local model, keep other clients idle, and free enough context for the test. Record any sessions already running on the machine.

    git clone --depth 1 --branch 1.19.1 https://github.com/SillyTavern/SillyTavern.git build-comparison/SillyTavern
    python -m pip install --target build-comparison/python psutil pywinpty
    npm install --prefix build-comparison/browser playwright-core
    npm exec --prefix build-comparison/browser -- playwright-core install chromium

Set PYTHONPATH to the absolute build-comparison/python path. Set BENCH_PLAYWRIGHT to build-comparison/browser/node_modules/playwright-core, BENCH_CHROME to the Chromium executable, and BENCH_PROVIDER_PID to the model backend process ID if provider memory should be recorded. The benchmark never starts or stops the inference backend.

## Run

From the repository root, with the paths configured above:

    node benchmarks/runner.mjs discover
    node benchmarks/runner.mjs smoke
    node benchmarks/runner.mjs run --profile=standard --mode=synthetic
    node benchmarks/runner.mjs run --profile=standard --mode=live --provider=http://[::1]:8080/v1
    node benchmarks/runner.mjs run --profile=standard --mode=live --track=recommended
    node benchmarks/runner.mjs validate
    node benchmarks/runner.mjs report

Use --neutrino and --vulkan to point to other build artifacts. The defaults are build-ram/windows-standard/neutrino.exe and build-vulkan/windows/neutrino.exe. --config, --sizes, and --reps narrow a run. --seed controls randomized ordering. --out chooses the evidence directory, and --timeout takes milliseconds. The default results location is build-comparison/results. Re-running skips completed IDs and retains failed attempts in the evidence.

Use a separate output directory when changing binaries or inference parameters such as --tokens. --require-complete makes the runner return an error if any requested inference trial fails to persist a reply.

The smaller gateway regression needs only the Neutrino executable and Python. It runs a seeded 500-turn chat through the web server and gateway and verifies that the captured prompt ends in the submitted cue and the next reply is persisted:

    python benchmarks/gateway_history.py build/neutrino
    python benchmarks/gateway_history.py build/neutrino --fault 429
    python benchmarks/gateway_history.py build/neutrino --branches branch-evidence.json

Synthetic output controls generation speed and lets the suite check UI delivery and disk persistence. Live runs use one upstream model and one common sampling policy. The controlled track disables Neutrino Compendium and leaves optional SillyTavern summarization inactive. The recommended track adds keyword lore to both products and enables the bundled Main API summarizer for ST-EXTENDED. The latter setting is insufficient to benchmark vector retrieval or long-term agent maintenance.

The basic correctness suite submits ray-slab, fluid, gas, rollback, and acoustic commands through the Neutrino web API. Reported ray timing includes HTTP and app overhead. The tested ray counts fit the manual simulation command input limit. It is separate from inference timing.

For the direct backend benchmark, build the optional ray_scaling_bench target. Run it with cpu, vulkan, or opencl to produce CSV for batches of 1 through 4,096 rays. It checks known intersection distances independently, uses warmed repetitions, reports first-dispatch latency, and fails if the requested hardware backend is unavailable.

    cmake --build build --target ray_scaling_bench

The optional live tool-selection case describes the relay chamber in prose and records the resulting Neutrino /trace and saved reply:

    node benchmarks/runner.mjs run --mode=live --scenario=relay-physics-prose --config=N-WEB-VK --sizes=10 --tokens=384 --timeout=65000 --out=build-comparison/narrator-physics

The report contains per-component sampled working sets and separate timing boundaries. Raw prompt payloads contain deterministic fictional fixture data. Treat missing metrics as unavailable. This benchmark currently covers the seeded delayed recall fixture and basic direct physics, with broader scenario and stability coverage pending.

## Reproduce the reported comparison

Run these from the prepared checkout. Use the original executables for the first two commands and separate Release binaries that contain the gateway fix for the third and fourth commands. Replace the executable paths if needed.

    node benchmarks/runner.mjs run --profile=standard --mode=synthetic --sizes=10 --reps=2 --out=build-comparison/fair-short --timeout=11000 --require-complete
    node benchmarks/runner.mjs run --profile=standard --mode=live --sizes=10 --reps=1 --tokens=256 --out=build-comparison/fair-live --timeout=20000 --require-complete
    node benchmarks/runner.mjs run --profile=standard --mode=synthetic --sizes=100,500,1000,10000 --reps=1 --config=N-TUI,N-WEB-CPU,N-WEB-VK --out=build-comparison/post-fix --neutrino=build-comparison/fixed-cpu/neutrino.exe --vulkan=build-comparison/fixed-vk/neutrino.exe --timeout=15000 --require-complete
    node benchmarks/runner.mjs run --profile=standard --mode=live --sizes=1000 --reps=1 --tokens=384 --track=recommended --config=N-WEB-CPU,ST-EXTENDED --out=build-comparison/recommended-384 --neutrino=build-comparison/fixed-cpu/neutrino.exe --timeout=55000 --skip-physics

Run the report command with each output path after its trials finish.

## RPG command latency

Measure the player-facing command path in an isolated local web process:

    python benchmarks/rpg_actions.py build-comparison/fixed-cpu/neutrino.exe --out build-comparison/rpg-actions.json

The default workload starts with 2, 25, or 100 controllable characters and 10 scripted history turns. It times 30 measured requests per command after five warmups and saves the observations as milliseconds. Create appends a new member each time. The measured path includes loopback HTTP and workspace saves for mutations. The commands run without a model provider.
