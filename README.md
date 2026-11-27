# Neutrino

A native C workspace for writing fiction and playing it out in a terminal or browser. Manuscript and Roleplay share one library, one Story Bible, and one inference server. Each roleplay story also has its own branch-aware Compendium. The terminal interface takes cues from Claude Code and Codex CLI's keyboard-first workflows and uses its own layout, palette, and typography.

## Build and open

Use a UTF-8, VT-capable terminal. Build with CMake 3.25 or later and a compiler that reports final C23 support, such as GCC 15.2 or the Clang driver in Zig 0.15.2. Release and RelWithDebInfo use `-O3`. All builds need HTTPS-capable libcurl development files. Windows uses WinHTTP for model inference. Linux and macOS use POSIX threads. cJSON is bundled under its own [license](third_party/cjson/LICENSE).

```powershell
# Windows, with Zig 0.15.2 and Ninja on PATH and the x64 curl development archive extracted
$curl = 'C:\path\to\curl-win64-mingw'
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=$((Get-Command zig).Source)" -DCMAKE_C_COMPILER_ARG1=cc -DCMAKE_C_COMPILER_TARGET=x86_64-windows-gnu -DCURL_NO_CURL_CMAKE=ON "-DCURL_INCLUDE_DIR=$curl/include" "-DCURL_LIBRARY=$curl/lib/libcurl.dll.a"
cmake --build build
Copy-Item "$curl/bin/libcurl-x64.dll" build/
.\build\neutrino.exe --demo
```

```sh
# Linux or macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/neutrino --demo
```

The demo opens an original manuscript and a connected roleplay story. It runs in memory. For persistent work, start without `--demo`. Neutrino saves to `.neutrino.json` in the working directory by default. Use `--workspace FILE` or `NEUTRINO_WORKSPACE` for a different file. A sidecar lock prevents two Neutrino processes from writing to the same workspace. `--snapshot` prints a static ANSI frame for inspection.

### Use Neutrino in a browser

Start `neutrino --web --demo` and open [the local browser interface](http://127.0.0.1:7000). Use `neutrino --web --workspace FILE` for persistent work. The browser opens a Manuscript and Roleplay library, composer, editor, and searchable command palette. Generation streams into the same story or candidate review as in the terminal. The **Files** menu uploads cards, workspaces, lorebooks, and presets, and downloads a workspace backup. The trace and candidate panels offer their own downloads.

Start the inference gateway separately as described under **Connect the inference server**. Use `--server URL` if the gateway uses a nondefault address. The web process owns one workspace and shares its current selection and generation across tabs. Close the terminal session for that workspace before opening it in the web process.

The web listener defaults to IPv4 loopback and port 7000. Use `--port PORT` to override the port. To use a phone or tablet over HTTPS, forward a private hostname to the loopback listener through a TLS reverse proxy. Preserve the public `Host` header, forward using HTTP/1.1 with a content length for uploads, and set `NEUTRINO_WEB_HOST` to the exact host (including the port if the browser sends one). Set `NEUTRINO_WEB_TOKEN` to a secret of at least 16 characters and enter it in the browser. `NEUTRINO_WEB_BIND` can change the listener's IPv4 address when a separate proxy host needs to reach it. The browser assets are embedded in the executable.

### GitHub release builds

Push a new `vMAJOR.MINOR.PATCH` or `build-NNN` tag from a commit that contains the release workflow. The workflow runs CTest and a demo smoke check on Windows x64, Linux x64 and ARM64, and macOS Intel and Apple Silicon. After every build passes, GitHub publishes the five archives and `SHA256SUMS.txt` on the tagged release. A tag such as `v0.16.0-rc.1` publishes as a prerelease.

For a build without publishing, run the **Release packages** workflow manually from the Actions tab and download its `release-packages-and-checksums` artifact. The Windows ZIP bundles libcurl. The Linux archives use system libcurl on a glibc distribution, and the macOS archives target macOS 15 or later.

The TUI centers a reading column and reflows when the terminal resizes. Wider terminals show the library and context beside it. On narrow or short terminals, **F3** opens the library in place of the reading pane. The composer expands to three lines when there is room and scrolls on one line in compact terminals. Use `/focus` to hide the side panes at any width.

## Use the workspace

| Key | Action |
| --- | --- |
| F1 | Help and command reference |
| F2 | Switch between Manuscript and Roleplay |
| F3 | Browse the library with arrows and Enter |
| F4 | Edit the scene or scratchpad |
| F5 | Continue the scene or ask the narrator for another turn |
| F6 | Inspect context |
| F7 / F8 / F9 | Open / accept / reject a candidate |
| F10 | Open the Story Bible |
| Ctrl+P or `/` | Open the command palette |
| Ctrl+Q | Save and quit |

Create a scene with `/scene /Folder/Book/Chapter/Scene`. In the editor, use **Ctrl+S** to save and **Ctrl+Space** to anchor a selection. Move the caret to extend the selection, then save. The palette also accepts `/select all` or a pair of UTF-8 byte offsets. Saved scene edits and accepted candidates create revisions. Use `/replace OLD | NEW` for literal, case-sensitive replacement throughout the current scene. An empty replacement removes the matches. Use `/revisions` and `/restore NUMBER` to inspect and restore saved drafts. Use `/rewrite-preset` to list Xenon-inspired revision presets or apply one to the selection or scene with a direction such as `/rewrite-preset pacing Keep Iona's voice clipped`. [Xenon writing tools](https://github.com/Colin1337X/cherenkov/blob/codex/xenon/README.md#first-scene-and-writing-tools)

`/compare NUMBER` shows the changed span of a saved draft beside the current text in the review panel. It identifies the starting line and keeps the scene available for `/restore NUMBER`.

`/batch /folder/book/chapter` opens the scenes under that path together in a read-only panel. Use `/batch /` for the whole workspace or a narrower path for large manuscripts.

`/append TEXT` adds a paragraph to the current manuscript scene or scratchpad. For a scene, it saves the previous draft in revision history.

`/cat` reads the current scene in a panel. `/cat /folder/book/chapter/scene` reads another scene while keeping the active editor selection.

In Manuscript, plain input supplies a direction for Continue. The writing palette has Continue, Rewrite, Expand, Describe, Draft, Brainstorm, Critique, Continuity, and Bible extraction. Up to three candidate requests run on worker threads. Review shows each candidate's status and keeps their original order as they finish. Compare them with the left and right arrows, then accept or reject. Describe offers selectable senses and multiple variants per sense via `/senses` and `/count`. Use `/insert` to add a detail after the target, or `/preview-save FILE` to keep a candidate separately. If the source has changed since generation, Neutrino blocks applying the candidate. Accepted extracts land as Bible drafts for manual promotion.

Use `/play` from a scene to start a linked roleplay. The narrator receives the linked manuscript and shared story sources. Use `/story TITLE` to start independently, or `/import FILE` to start from a character card. A roleplay turn saves the player message, then streams the reply. `/swipe` generates a sibling reply. `/previous`, `/next`, and `/branch ID` navigate the tree. Use `/persona NAME | DESCRIPTION` for a player persona, `/lore FILE` for keyword-triggered sources, and `/preset FILE` for enabled prompt text. `/transcript /Folder/Book/Chapter/Scene` copies the active branch into a new manuscript scene.

Use `/rewind` to return to the previous narrator reply, `/rewind ID` to return to an ancestor, or `/rewind 0` to return to the start. Your next turn grows a branch from that point. `/search TEXT` finds literal, case-sensitive matches on the active visible path, newest first. Use `/branch ID` to open a match or revisit the original continuation. [rp-core roleplay guide](https://github.com/LeviTheWeasel/rp-core/blob/main/docs/USER-GUIDE.md)

`/tails` lists visible branch ends with a short excerpt and their message IDs. Use `/branch ID` to open one.

`/lore-test TEXT` checks the triggers and source budget for a proposed next player message. It uses the current branch and leaves the transcript alone.

`/note TEXT` sets a story-wide author's note in the narrator's system prompt. `/note` displays it and `/note off` clears it. The note controls narration and does not become filing evidence.

When a narrator draft appears to stop mid-sentence, Neutrino requests one shorter draft. If the shorter draft also stops early, Neutrino saves through the last complete sentence when one is available. Otherwise the turn stays open for `/narrate`. The decision appears in `/trace`.

## Rooms, inventory, and characters

The roleplay world takes inspiration from Neko Agent's rooms, carried items, environment, and addressed character replies. Create a room with `/room NAME | DESCRIPTION`. The first room is the starting point. Use `/connect ROOM | ROOM` to link rooms in both directions and `/go ROOM` to move through a link. Use `/item NAME | DESCRIPTION` to place a prop here, `/take NAME` to carry it, and `/drop NAME` to leave it in the current room. `/weather TEXT` sets the branch's conditions manually.

Use `/npc NAME | DESCRIPTION` to place a character in the current room. `@Name MESSAGE` asks that character to respond through the configured narrator model. A character has to be present. `/say TEXT` records speech in the room without calling a model. `/world` shows the room, exits, inventory, and characters. `/world log` shows the branch's recent actions. New rooms, exits, movements, inventory, weather, and character placement follow the active branch. Existing workspace room and exit definitions remain the shared starting map. [Neko Agent module reference](https://github.com/GameDevFox/neko-agent/blob/0c5bc71/readme/MODULES.md)

`/map` shows the current room, links, and unreachable rooms. `/map ROOM` shows the shortest route and the first `/go` command for that route. `/inspect` shows the current room's description, and `/inspect NAME` looks at a nearby item, carried item, or present character on the active branch. [Neko Agent play view](https://github.com/GameDevFox/neko-agent/blob/0c5bc71/readme/PLAYING.md)

Use `/wear ITEM` for a carried article and `/remove ITEM` to take it off and keep it in your inventory. The active branch's worn items appear in `/world`, `/inspect`, and the narrator's world context.

`/inventory` shows items on you and in reach. Use `/inventory held`, `/inventory worn`, or `/inventory nearby` to focus the list.

`/who` lists the characters currently in the room and the names you can address.

`/exits` lists the destinations reachable from the current room, their descriptions, and the matching `/go` commands.

### RPG commands and party control

`/character create NAME | DESCRIPTION` creates a controllable party member in the current room. Use `/party` to see everyone and `/character switch NAME` to act as that character. Each character keeps their own location and belongings. `/character switch @default` returns to the original player, including their existing location and possessions. The transcript identifies which character spoke or acted.

| Action | Command |
| --- | --- |
| Look around or inspect | `/look`, `/examine TARGET` |
| Move through a connected exit | `/go ROOM` |
| Manage possessions | `/inventory`, `/take ITEM`, `/drop ITEM`, `/wear ITEM`, `/remove ITEM` |
| Hand over an item in the same room | `/give ITEM | CHARACTER` |
| Talk to someone or use an available item | `/talk CHARACTER | MESSAGE`, `/use ITEM [| TARGET]` |

Uppercase bare commands such as `LOOK`, `GO Loft`, and `TAKE Lantern` also work. Bare `look`, `inventory`, `exits`, `who`, and `party` accept either case. Talking and using items ask the narrator for a reply. Movement, inventory changes, and character switching apply directly.

With `/stats on`, the narrator can create or switch characters with the `character` tool. `world` and `inventory` accept an optional `actor` name so it can act for a party member while keeping the current selection. Narrator edits save on the accepted reply's branch.

## Scene statistics and narrator tools

`/stats` shows the active branch's location, clock, character locations, needs, mood, injuries and directional relationships. Player location follows `/go`. Character location comes from their room entry or an explicit tracked location. This also lets you track characters off-scene. Needs range from 0 to 100 as urgency increases. Mood ranges from 0 to 100 from low to high. Injuries are records by character, body part and condition, with four severity levels and untreated, treated or healed states.

Use `/time set 2 21:30` for a calendar jump, `/time auto 10` to advance ten minutes per accepted reply, `/time auto off` for a manual clock, or `/time advance 30` for elapsed time. Elapsed time raises hunger, thirst, bladder and bowel urgency for tracked characters. Setting the clock leaves needs alone. Actor state and clock edits follow the active branch.

Use `/sim` for the available tool arguments. Player edits work with narrator tools disabled. Examples:

```text
/sim {"neutrino_tool":"stat","actor":"Iona","metric":"thirst","value":35}
/sim {"neutrino_tool":"stat","actor":"Mara","metric":"location","location":"West Pier"}
/sim {"neutrino_tool":"bond","actor":"Iona","other":"Mara","value":40}
/sim {"neutrino_tool":"injury","actor":"Mara","part":"left hand","kind":"cut","severity":"moderate"}
/sim {"neutrino_tool":"treat","actor":"Mara","part":"left hand","kind":"cut"}
/sim {"neutrino_tool":"inventory","action":"edit","name":"signal lamp","description":"Brass lamp with a cracked lens."}
```

`/stats on` lets the narrator choose tools when the results affect the scene, even when the player describes an action without naming a tool. The narrator can use up to 12 operations before prose, or group a larger setup into one ordered plan. Neutrino validates the requests and returns observations to the narrator. Edits follow the reply's branch and save with accepted prose. Cancelling the reply discards staged edits. `/trace` includes requests and results. With tools off, Neutrino omits tracked state from the prompt and pauses automatic clock advancement.

The narrator can create, edit, and remove map features and entities with `world`, manage carried and worn objects with `inventory`, and set or advance time with `set_time`, `auto_time`, and `advance`. `world_inspect` lists rooms, exits, characters, and items in pages. Saved scenarios can grow across replies. The tool budget, context window, and individual solver bounds still govern each calculation.

For dependent narrator actions, use an object with an `actions` array. Operations run in order against a staged copy of the scene. Add `"continue":true` beside `actions` to let the narrator use the results to select another tool. If a step fails, the sequence is discarded and `/trace` identifies the failed step.

Use `sim_plan` for a hypothetical experiment or an ordered set of staged changes with typed consequence queries. The terminal, web composer, and narrator share the same executor. See the [multiphysics guide](docs/MULTIPHYSICS.md) for runnable examples, shared materials, and voxel CFD.

To run the same sequence directly, paste its JSON after `/sim`. The panel shows each result, and a valid sequence applies its changes to the current branch immediately.

### Headless 3D room

The narrator can create multiple named 3D layouts with `scene_room`, list them using `scene_catalog`, and switch layouts with `scene_select`. Geometry uses axis-aligned boxes in metres with Z up. Reusing an object's name updates it within the active layout. Each branch inherits its ancestor's edits. `scene_inspect` pages through saved objects, and room raycasts traverse all saved objects in batches. For sound and light queries in layouts with more than 16 objects, supply `include` with the object names to evaluate. The 3D layouts are separate from the story's navigation map.

```text
/sim {"neutrino_tool":"scene_room","min":[0,0,0],"max":[8,5,4]}
/sim {"neutrino_tool":"scene_place","name":"Divider","kind":"wall","min":[2.5,1,0],"max":[3,4,4]}
/sim {"neutrino_tool":"scene_rays","origin":[1,2,2],"count":4096,"backend":"cpu"}
/sim {"neutrino_tool":"scene_acoustic","source":[1,2,2],"listener":[5,2,2]}
```

Raycasts report the first surface each ray hits. Use `directions` for specific rays or `count` for a spherical sample. Acoustic queries reuse the saved geometry. Geometry blocks sound by default. Set an object's `material` with `loss_db_per_m` and `reflectivity` to model attenuation, transmission, and reflection. The room supports geometric queries without a renderer or rigid-body dynamics.

### 3D physics evaluation

| Tool | Model |
| --- | --- |
| `elastic3d` | Homogeneous cuboid under three normal loads, or a slender cantilever with axial and transverse load. Reports small deformation and stress. |
| `softbody3d` | Spring-linked 3D lattice at a requested node count. Supports a pinned base or free motion, initial velocity, a gravity vector, damping, and compliant spring projection. |
| `trajectory3d` | Free flight in 3D with scene gravity, constant wind, and optional linear drag. |
| `thermal3d` | Heating and cooling, optional fixed-volume ideal gas pressure, and optional analytical 3D conduction modes in a cuboid. |
| `ignition3d` | Fictional ignition and cooldown at a 3D site. Reports activation and reset using dimensionless values and simulation ticks. |
| `flow3d` | A 3D liquid tracer or low-speed gas concentration. Select the reduced model or a bounded incompressible Navier–Stokes calculation. |
| `scene_light` | Direct and single-bounce paths, inverse-square spreading, material transmission, gas extinction, and arrival time. |

`flow3d` defaults to `"method":"reduced"`. It moves a Gaussian tracer through a constant wind and estimates diffusion. Select `"method":"navier_stokes"` or `"method":"cfd"` for a periodic grid with velocity and tracer advection, viscosity, diffusion, and pressure projection. The grid uses 6–24 cells per axis and up to 96 time steps. The same options work with `fluid` and `gas` when a 3D `size` is supplied. Their original tank-drain and ideal-gas tools remain available. The grid uses periodic boundaries, independent of room walls. The gas solver models low-speed, constant-density flow and transports a passive concentration. The structural solvers cover small elastic deformation and a pinned spring network. [Stable Fluids reference](https://doi.org/10.1145/311535.311548)

Use `"backend":"gpu"` to run pressure projection on an OpenCL GPU. Other CFD steps use system memory. OpenCL failure falls back to CPU projection. If CFD exceeds its memory or timestep budget, the tool reports the fallback to reduced flow. Windows allocates pageable system memory first and can use the configured pagefile. If allocation fails, it tries a temporary file mapping. Use `"memory":"mapped"` to select the file-backed allocation on Windows. Neutrino leaves Windows pagefile settings unchanged.

`scene_medium` stores a branch-local gas configuration. `sound_loss` and `light_loss` multiply the integrated concentration along the air portion of a ray. They are model coefficients expressed in dB per concentration-metre. `scene_place` accepts a separate `optical` material with power `transmission`, power `reflectivity`, and `loss_db_per_m`. Choose `"backend":"rt"` for hardware occlusion of direct and wall-reflected light when the RT build is available. Material attenuation and object-reflection checks run on the CPU.

`softbody3d` accepts an exact `nodes` count of at least 8, or a scalar or three-axis `resolution` for a complete lattice. An exact count can leave the top layer partially filled. Dynamic runs use pageable memory or a Windows temporary file mapping and report allocation failure without reducing the requested count. Memory and runtime depend on lattice size and duration. Use `sample_node` to inspect a zero-based node index, or set `seconds` to zero to query the initial geometry without allocating the lattice.

```text
/sim {"neutrino_tool":"flow3d","medium":"liquid","size":[6,6,4],"method":"navier_stokes","vortex_m_s":0.4,"seconds":0.5}
/sim {"neutrino_tool":"scene_room","min":[0,0,0],"max":[6,5,4]}
/sim {"neutrino_tool":"scene_medium","method":"cfd","wind":[0.3,0.1,0],"seconds":0.5,"sound_loss":2,"light_loss":4}
/sim {"neutrino_tool":"scene_light","source":[1,2,2],"listener":[5,2,2],"wall_reflectivity":0.3}
/sim {"neutrino_tool":"ignition3d","source":[1,2,3],"site":[1,2,3],"pulse_ticks":2,"cool_ticks":2}
```

Run `ctest --test-dir build --output-on-failure` for the numerical checks. Run `scene_physics_test --cfd-gpu` to compare OpenCL pressure projection with the CPU, and `scene_physics_test --rt` to check the separate hardware ray path.

```json
{"actions":[{"neutrino_tool":"tune","setting":"gravity","value":1},{"neutrino_tool":"fluid","height":1,"tank_area":2,"outlet_area":0.003,"seconds":10}]}
```

The included calculators are geometric ray–box intersection, a draining tank estimate and an ideal-gas room with a constant leak. They use SI inputs and execute locally. The narrator can stage a `tune` call to change the scene's gravity, tank outlet coefficient, or default gas temperature. Later calculations in the same reply use the staged settings. Accepted changes follow the reply's branch and show up in `/stats`. Players can use the same tool through `/sim`.

`/sim` also provides batched ray–box intersections and geometric acoustics. Acoustics traces a direct path and one specular reflection per visible room wall or material surface. A blocker without a material is opaque. Give a blocker a `material` with `loss_db_per_m` and amplitude `reflectivity` (0–1) to split the sound at its surfaces. The transmitted branch loses energy at entry and exit and attenuates over the distance traveled inside. The reflected branch uses the surface's reflectivity. Losses add where materials overlap.

Set `medium.loss_db_per_m` for air or another room gas. This loss applies to the part of the path outside solids. Supply `medium.speed_m_s` to set the propagation speed, which otherwise comes from ambient air temperature. Delays use that speed along straight segments. The model uses single-bounce geometry and does not calculate refraction, diffraction, or frequency bands.

When the latest player action supplies one matching acoustic request, the narrator can select it with `{"neutrino_tool":"acoustic"}`. Neutrino uses the supplied geometry and material values. For a turn devoted to that reading, end the action with `Acoustic setup:` followed by the request object. The pinned llama.cpp gateway constrains the first response to the short selector, then the narrator continues the scene. `/trace` shows both the selection and the complete arguments used for the calculation.

Levels are relative to the direct source at one metre. In material mode `min_level_db` sets the modeled cutoff (default −120 dB), and audibility is the remaining margin in dB after geometric spreading and losses. A path stops at zero margin. This cutoff is a scene parameter. Use an appropriate source reference and threshold if you want to model a listener's hearing.

```text
/sim {"neutrino_tool":"tune","setting":"gravity","value":1.62}
/sim {"neutrino_tool":"tune","setting":"wall_reflectivity","value":0.45}
/sim {"neutrino_tool":"acoustic","min":[0,0,0],"max":[10,8,4],"source":[2,2,2],"listener":[5,2,2],"blockers":[{"min":[3,1,1],"max":[4,3,3]}]}
/sim {"neutrino_tool":"acoustic","min":[0,0,0],"max":[10,8,4],"source":[2,2,2],"listener":[5,2,2],"medium":{"loss_db_per_m":0.02,"speed_m_s":343},"min_level_db":-45,"blockers":[{"min":[3,1,1],"max":[4,3,3],"material":{"loss_db_per_m":6,"reflectivity":0.5}}]}
/sim {"neutrino_tool":"rays","min":[2,-1,-1],"max":[3,1,1],"rays":[{"origin":[0,0,0],"direction":[1,0,0]}],"backend":"gpu"}
/sim {"neutrino_tool":"prop_load","strength":"fragile","load":"heavy"}
```

The ray batch and acoustic visibility tests accept `backend` values `auto`, `cpu`, `gpu` (OpenCL), and `vulkan`. Material crossings and attenuation run on the CPU. Automatic mode uses the CPU for small batches. At 32 or more intersection tests it tries a Vulkan hardware device when built in, then OpenCL, then CPU. Explicit Vulkan requests also work on software Vulkan implementations, which the result identifies as such.

Build the optional Vulkan compute backend on Windows or Linux with `-DNEUTRINO_VULKAN=ON`. It needs Vulkan headers and `glslangValidator` when building and loads the installed Vulkan runtime on demand. The default build keeps its existing OpenCL and CPU backends. Run `scene_physics_test --vulkan` for a runtime ray and acoustics check. [Khronos Vulkan compute guide](https://docs.vulkan.org/guide/latest/compute_shaders.html)

Add `-DNEUTRINO_VULKAN_RT=ON` to enable hardware ray queries on a Vulkan 1.2 GPU and driver that support `VK_KHR_ray_query` and acceleration structures. Select `"backend":"rt"` for `rays`, `scene_rays`, or acoustic occlusion. Results and `/trace` identify a successful dispatch as **Vulkan hardware RT**. If explicitly selected RT is unavailable, the tool reports it. The existing automatic backend order still applies when `backend` is omitted. [Khronos Vulkan ray queries](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_query.html)

The RT path keeps one scene cache. It reuses unchanged geometry, refits moved boxes in place when the box count is unchanged, and reuses acceleration-structure storage when rebuilding within capacity. The command pool, descriptors, and mapped ray buffer are also reused between batches. Acoustic material transmission and attenuation still use the CPU. Use `scene_physics_test --rt` to verify hardware execution. [Vulkan acceleration structure update rules](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html)

`/memory items` shows the active branch's filed props and their evidence. Multiple named props can be kept from a single sentence when the narrator establishes their actions or locations. Use `/inventory` for the player-controlled world inventory.

`prop_load` gives a qualitative result for fictional props and scenery under load. It uses three descriptive levels of strength and load, with outcomes of holds, gives slightly, or breaks.

## Headless web research

The narrator automatically checks factual questions for recent news, named real-world references, and word definitions. It searches before drafting the reply and attaches verified source URLs inline. `/trace` records the lookup decision, query, supplied evidence, and how each citation was added. Automatic lookups use the latest player message and expire after that reply. A manual lookup takes precedence. Set `NEUTRINO_AUTO_RESEARCH=0` in the TUI or SSH session environment to turn automatic lookups off.

Dictionary lookups use the English definitions from Wiktionary’s structured API. For an automatic definition, the narrator selects a published sense and Neutrino writes its text with an inline entry link. If the selection is invalid, the first available sense is used. Etymology and quotations stay outside the definition evidence. [Wikimedia definition endpoint](https://www.mediawiki.org/wiki/Mobile_Content_Service)

`/research <query>` combines Google News RSS, Hacker News, Wikipedia, and Wiktionary. Use `/research wiktionary <word>` for dictionary-only results from English Wiktionary. Add `hour`, `day`, `week`, `month`, or `year` before a general query for time-sensitive lookups. Dated searches omit encyclopedia and dictionary results. The hour filter requires a source timestamp within the last hour. The other date ranges use the search engines' filters. Coverage depends on what the engines have indexed.

```text
/research hour Seoul transit announcements
/research Steamed Hams origin
/research wiktionary serendipity
/read https://en.wikipedia.org/wiki/Steamed_Hams
/trace
/research clear
```

The gateway can also query an optional SearXNG instance. Set `NEUTRINO_SEARXNG_URL` on the gateway to its complete `/search` URL and enable JSON output on that instance. The built-in engines work without a SearXNG service. [SearXNG Search API](https://docs.searxng.org/dev/search_api.html) · [Hacker News Search API](https://hn.algolia.com/api) · [MediaWiki OpenSearch](https://www.mediawiki.org/wiki/API:Opensearch)

The C headless reader fetches up to three direct pages per search and extracts readable text from HTML or plain text. It has no browser window and does not execute JavaScript. Google News links are presented as feed metadata. `/read` can fetch an available publisher URL directly. The public reader uses HTTPS, checks the remote address on each connection, and limits redirects and response size. An administrator can set `NEUTRINO_CA_BUNDLE` for libcurl builds that need an explicit CA bundle.

Sources are shown with URLs, available dates, and excerpts. The latest successful lookup is available to the narrator in the same story or writing scene until replaced or cleared. It is session-local. Each reply uses the set of sources available when generation began. If a saved roleplay reply contains no URL from that set, Neutrino adds up to three links under **Sources supplied for this reply**. `/trace` distinguishes links written by the narrator from links added on save. Search terms go to the configured engines. The gateway performs the network requests, including in SSH sessions.

## Serve the TUI over SSH

Install the C executable on the SSH host and start the inference server there. Configure a separate OpenSSH listener with [the example configuration](docs/sshd_config.example). Replace the account and paths with your deployment values, generate a host key, and install the intended user's public key in the configured authorized-keys file. Restrict those files to the serving account and administrator as appropriate. Configure a non-root account with write access to its workspace directory. To use a gateway token, supply it to the gateway and expose the same value to the forced command through the private OpenSSH service configuration.

Validate the separate configuration with `sshd -t -f /path/to/sshd_config` before starting it with `sshd -f /path/to/sshd_config`. The example listens on loopback, and the client connects with a PTY:

```sh
ssh -tt -p 2222 neutrino@127.0.0.1
```

OpenSSH handles authentication and encryption. A forced command launches Neutrino on the host without a remote shell, SFTP, or forwarding. The SSH session disables arbitrary filename commands and changing the inference server. `/trace` remains available in the TUI. A workspace permits one writer, so a second connection to the same workspace reports a lock until the first closes. Use distinct workspace paths and OS accounts for separate users. The tested deployment uses Linux OpenSSH in WSL. [OpenSSH forced-command documentation](https://man.openbsd.org/sshd_config.5#ForceCommand)

The TUI adapts to terminal resizes and caps its rendered viewport at 240 columns by 120 rows.

See the [dependency audit](docs/DEPENDENCIES.md) for the build, runtime, and deployment inventories.

## Connect the inference server

Start the gateway on the machine that can reach the provider. Use a preset for local Ollama or llama.cpp, or give the full OpenAI-compatible API URL and model ID.

```powershell
.\build\neutrino.exe --serve --provider ollama --model 'your-installed-model-id'
.\build\neutrino.exe --serve --provider http://127.0.0.1:8080/v1 --model 'your-model-id'
```

For a hosted provider, put its key in the gateway process environment and give the variable name. The `openai` preset defaults to `OPENAI_API_KEY`.

```powershell
.\build\neutrino.exe --serve --provider 'https://provider.example/v1' --model 'model-id' --api-key-env MY_PROVIDER_KEY
```

An explicitly chosen endpoint uses the named key variable or the preset's default. Existing environment-only launches still use `NEUTRINO_ENDPOINT`, `NEUTRINO_MODEL`, and `NEUTRINO_API_KEY`.

### Switch providers with /model

Set `NEUTRINO_PROVIDER_ADMIN_TOKEN` to the same secret in the gateway and in each trusted TUI or web process that will manage it. Use at least 16 characters and a different value from `NEUTRINO_SERVER_TOKEN`. A gateway started with an admin token can accept its first provider through `/model`.

Open `/model` to choose a provider and edit its model ID, key variable, and streaming usage setting. In the terminal, use Tab to move, the arrow keys to change choices, and Enter on Apply. Choosing **Current provider** keeps its endpoint and gateway credential. Without admin access, the menu can select the workspace narrator model.

```text
/model provider ollama | your-installed-model-id
/model provider https://provider.example/v1 | model-id | NEUTRINO_PROVIDER_KEY_HOSTED
/model api
/model api usage on
```

Set custom key variables such as `NEUTRINO_PROVIDER_KEY_HOSTED` in the gateway environment. The command takes the variable's name. The `openai` preset uses the gateway's `OPENAI_API_KEY` when the key variable is omitted. `/model api usage off` disables the optional stream-usage request. Provider settings apply to everyone using that gateway and last until it restarts. Start a web process with the admin token only when its users should have permission to manage the gateway.

Open the TUI in a second terminal. Its default server address is `http://127.0.0.1:4783/v1`. The TUI sends prompts to Neutrino's server, which opens the upstream provider connection and streams the reply back.

```powershell
.\build\neutrino.exe --demo
```

| Server setting | Purpose |
| --- | --- |
| `NEUTRINO_ENDPOINT` | OpenAI-compatible provider base URL. Defaults to `http://127.0.0.1:11434/v1`. |
| `NEUTRINO_MODEL` | Narrator and manuscript model ID when `--model` is omitted. |
| `NEUTRINO_API_KEY` | Upstream bearer credential, when required. Keep it in the server process. |
| `NEUTRINO_AGENT_MODEL` | Filing and historian model. Defaults to the narrator model. |
| `NEUTRINO_RECALL_MODEL` | Recall model. Defaults to the agent model. |
| `NEUTRINO_RECALL_ENDPOINT`, `NEUTRINO_RECALL_API_KEY` | Optional dedicated recall provider and its bearer credential. |
| `NEUTRINO_WORKERS` | Parallel upstream requests for the default provider when unpinned. Set 1–4, default 2. A pinned llama.cpp slot or a dedicated recall endpoint runs one request at a time. |
| `NEUTRINO_PHYSICS_WORKERS` | Startup scheduler setting. Defaults to `auto`. Use `/sim workers auto` or `/sim workers 1–16` to change it for the current TUI or web process. A manual count includes the caller and is limited by independent work units. |
| `NEUTRINO_STREAM_USAGE` | Optional. Set to `1` if the default upstream provider supports OpenAI-compatible `stream_options.include_usage` and only returns token counts when asked. llama.cpp timing metrics work without this setting. |
| `NEUTRINO_SERVER_TOKEN` | Optional credential between the TUI and Neutrino's server. Set it in both processes when enabled. |
| `NEUTRINO_SERVER_BIND` | Inference gateway IPv4 listen address. Defaults to `127.0.0.1`. |
| `NEUTRINO_SERVER_HOST` | Allowed non-loopback HTTP Host header, including the port when sent by the client. |

The gateway listens on loopback by default. To reach it through SSH, use `ssh -N -L 4783:127.0.0.1:4783 user@server`. To listen on all IPv4 interfaces, set `NEUTRINO_SERVER_BIND=0.0.0.0`, set `NEUTRINO_SERVER_HOST` to the exact address and port that remote clients use, and set `NEUTRINO_SERVER_TOKEN` to a secret of at least 16 characters. For the browser listener, use the corresponding `NEUTRINO_WEB_BIND`, `NEUTRINO_WEB_HOST`, and `NEUTRINO_WEB_TOKEN` variables. The browser process also needs the gateway token when connecting to an authenticated gateway.

Use `--serve --port PORT` to change the gateway port and `--server URL`, `NEUTRINO_SERVER`, or `/server URL` to change the TUI's client address. The slash command applies to the current TUI session. A reverse proxy must pass HTTP/1.1 with a loopback or configured Host header and stream responses without buffering.

The gateway gives clients five seconds to finish sending headers and twelve more seconds to finish sending the body after header validation. Unfinished headers and bodies each have a four-connection budget, separate from the sixteen requests that may be queued or running.

Use `/neofetch` in a local or SSH session to see gateway availability, provider activity, model, routing, and active and queued requests. The dashboard refreshes every few seconds while open. Gateway status comes from the authenticated `GET /v1/status` endpoint. The provider row shows the outcome and age of the last observed request on the default provider lane.

The dashboard reports median tokens per second and median upstream time to first token (TTFT) for up to 32 completed foreground generations in the current session. Throughput uses provider timing where available or the provider's completion token count and the observed time between the first and last streamed output. The display shows how many samples contributed to each median and lists the latest five completed generations. Changing server or narrator clears the session sample window.

**Esc** requests cancellation and keeps partial foreground text available for export. Windows may wait for an outstanding WinHTTP read to return. The TUI owns the workspace and its memory queue. Closing it cancels active requests; unfinished filing picks up when the workspace reopens.

## Compendium and prompt trace

Each completed narrator reply queues filing. The filing agent writes typed facts such as characters, places, items, events, and notes onto the reply that established them. Neutrino checks entries across types for verbatim repetitions and equivalent location, stranding, and concealment claims. It keeps unrecognized wording, conflicting state, and novel sentences alongside established facts. The historian compacts older turns into a cumulative recap. Recall combines relevant active entries with recent dialogue, the recap, and other story sources. Switching or swiping a branch changes the reply history and the memory entries visible to the narrator.

The filing agent receives the latest narrator reply and relevant entries for deduplication. Each proposed filing must quote the latest reply and name an entity that appears in the supporting text. Short quotations expand to their source sentences so qualifications remain visible. If no opening filings pass the check, Neutrino looks for explicit object placements, stranded characters, and opening-time rules in the narrator's prose. On later turns, direct evidence can also file a new placement or attachment of an item the narrator already mentioned. Neutrino checks the last observed placement on the active branch and keeps the original sentence as `reply-evidence`.

Before accepting a filing, Neutrino can correct an item name when the narrator explicitly identifies a different concealed object. Handling or placement in the supporting prose can also correct a physical item mislabeled as a character. It files a named, titled person as a character if the reply supplies no explicit creature identity. Source sentences that show an observed flare can be filed directly if the agent misses them. A single narrator sentence can support both a flare and a bell event. Use `/memory refile ID` to run the current filing rules on an earlier reply.

| Command | Action |
| --- | --- |
| `/memory` | List active entries and their IDs. |
| `/memory show ID` | Read the full active entry for this branch. |
| `/memory new TYPE \| NAME \| DESCRIPTION` | Add an entry on this branch. |
| `/memory edit ID \| DESCRIPTION` and `/memory delete ID` | Edit or hide an entry from this point on the branch. |
| `/memory mode [MODE]` | Show or set recall to `auto`, `prefetch`, `sync`, `lexical`, or `off`. |
| `/model recall recall:MODEL` | Use the configured recall model and connection. `default:MODEL` selects the server's narrator model for recall. `default` clears the override. |
| `/memory status`, `/memory retry`, `/memory refile ID`, `/memory undo ID` | Inspect, retry, redo, or remove background output. |
| `/memory off` and `/memory on` | Pause or resume the story's Compendium. |
| `/trace [FILE]` | Inspect or export the last foreground prompt from this TUI session. |

`auto` and `prefetch` use a completed recall result from the active path when available, with lexical matching as a fallback. `sync` waits for model recall on the current turn. `lexical` and recall mode `off` use keyword matching without a recall model call. Model recall failures fall back to keyword matching.

`/trace` includes the complete ordered messages as JSON, the chosen memory entries and recap coverage, and the reason each lore entry was included or skipped. Lore matching covers constant entries, primary keywords, selective secondary keywords, and the source budget. When the narrator echoes its previous reply, Neutrino discards the draft and tries once with a fresh-response prompt. The trace labels the retry. If the model echoes the reply again, the turn stays open for `/narrate`. Exported traces contain story text.

`/budget` sets an estimated context-token target. The narrator keeps the current turn, selected lore and Compendium entries, and any recap. It fills the remaining space with the newest uncovered exchanges. `/trace` shows the estimate and how many older messages were omitted. The model's tokenizer determines the actual count, and required context can exceed the target on its own.

### llama.cpp cache isolation

Start llama.cpp with at least two separate cache slots (`-np 2 --no-kv-unified`) and set `NEUTRINO_LLAMA_SLOTS=1` in Neutrino's server environment. Narration uses slot 0. Filing, history, and same-provider recall use slot 1. In this mode llama.cpp constrains filing responses to the Compendium JSON schema. A separately configured recall provider manages its own cache. Enable this setting only for a llama.cpp instance configured with separate slots. [rp-core guide on local server caching](https://github.com/LeviTheWeasel/rp-core/blob/main/docs/USER-GUIDE.md#local-servers-speed-and-caching) · [llama.cpp server documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md#post-v1chatcompletions-openai-compatible-chat-completions-api)

Run `neutrino bench ttft [ROUNDS]` against the running Neutrino server to compare cold, warm, and post-agent time to first token using synthetic prompts. The reported ratio and verdict use timing. Compendium prompts and the benchmark are based on the [rp-core Compendium workflow](https://github.com/LeviTheWeasel/rp-core/blob/main/docs/USER-GUIDE.md#the-compendium).

## Import and export

Use `/import FILE` or `--import FILE` to bring data into a Neutrino workspace.

| Format | Imported data |
| --- | --- |
| Cherenkov v1 / Xenon v2 workspace JSON | Hierarchy, scene text, controls, revisions and Story Bible. A fresh workspace also takes the source configuration and scratchpad. |
| rp-core v1 session JSONL | Cast and persona, messages, branches, active head, message edits, hidden flags and reasoning. |
| Solo SillyTavern JSONL | Messages, selected swipes, alternate replies and hidden flags. An already imported matching card supplies character context. |
| Character cards | SillyTavern/Character Card v1–v3 JSON and PNG with uncompressed `chara` or `ccv3` metadata. Includes greetings, alternate greetings and embedded lore. |
| SillyTavern World Info JSON | Use `/lore FILE` to attach a book to the current story or `/import FILE` to start a story with that book. Accepts both keyed objects and arrays of entries. |

Card text, lore and enabled prompt blocks support `{{char}}` and `{{user}}`. The character resolves to the card nickname when present. Imported greetings use the default player name, and later prompts use the active persona. World Info supports literal primary and secondary keys, all four secondary matching modes, case and whole-word options, per-entry scan depth, and constant and disabled entries. `/lore-test TEXT` previews activation. Regex, timing, recursive activation, and SillyTavern-specific extensions and scripts are outside this compatibility layer. [SillyTavern World Info](https://docs.sillytavern.app/usage/core-concepts/worldinfo/)

Imports leave the input file intact. Workspace imports with colliding paths or IDs are rejected. `/export FILE` creates a fresh JSON backup. Use the backup on another machine or reimport it into an empty workspace.

Neutrino is a C reimplementation of the combined core workflows. Upstream features outside this port include rp-core's automated Director, native Anthropic and Gemini dialects, RoleCall ZIP import, and Xenon's persistent job runner, recursive batch generation, and browser appearance features. Roleplay presets use their enabled prompt text.

## Upstream references

- [rp-core](https://github.com/LeviTheWeasel/rp-core) at `02ac77e`. Roleplay, character cards, branches, swipes, and session journals.
- [Cherenkov / Xenon](https://github.com/Colin1337X/cherenkov/tree/codex/xenon) at `917a1b0`. Scene hierarchy, writing operations, review, Story Bible, context and portable workspaces.
