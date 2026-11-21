# Neutrino

A native C terminal workspace for writing fiction and playing it out. Manuscript and Roleplay share one library, one Story Bible, and one inference server. Each roleplay story also has its own branch-aware Compendium. The interface takes cues from Claude Code and Codex CLI's keyboard-first workflows and uses its own layout, palette, and typography.

## Build and open

Use a UTF-8, VT-capable terminal. Windows builds use WinHTTP. Linux and macOS builds require the libcurl development package and POSIX threads. CMake 3.18 or later builds the C11 application. cJSON is bundled under its own [license](third_party/cjson/LICENSE).

```powershell
# Windows, with the Visual Studio C++ desktop workload installed
cmake -S . -B build
cmake --build build --config Release
.\build\Release\neutrino.exe --demo
```

```sh
# Linux or macOS
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/neutrino --demo
```

The demo opens an original manuscript and a connected roleplay story. It runs in memory. For persistent work, start without `--demo`. Neutrino saves to `.neutrino.json` in the working directory by default. Use `--workspace FILE` or `NEUTRINO_WORKSPACE` for a different file. A sidecar lock prevents two Neutrino processes from writing to the same workspace. `--snapshot` prints a static ANSI frame for inspection.

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

Create a scene with `/scene /Folder/Book/Chapter/Scene`. In the editor, use **Ctrl+S** to save and **Ctrl+Space** to anchor a selection. Move the caret to extend the selection, then save. The palette also accepts `/select all` or a pair of UTF-8 byte offsets. Saved scene edits and accepted candidates create revisions. Use `/revisions` and `/restore NUMBER` to inspect and restore them.

In Manuscript, plain input supplies a direction for Continue. The writing palette has Continue, Rewrite, Expand, Describe, Draft, Brainstorm, Critique, Continuity, and Bible extraction. Compare streamed candidates with the left and right arrows, then accept or reject. Describe offers selectable senses and multiple variants per sense via `/senses` and `/count`. Use `/insert` to add a detail after the target, or `/preview-save FILE` to keep a candidate separately. If the source has changed since generation, Neutrino blocks applying the candidate. Accepted extracts land as Bible drafts for manual promotion.

Use `/play` from a scene to start a linked roleplay. The narrator receives the linked manuscript and shared story sources. Use `/story TITLE` to start independently, or `/import FILE` to start from a character card. A roleplay turn saves the player message, then streams the reply. `/swipe` generates a sibling reply. `/previous`, `/next`, and `/branch ID` navigate the tree. Use `/persona NAME | DESCRIPTION` for a player persona, `/lore FILE` for keyword-triggered sources, and `/preset FILE` for enabled prompt text. `/transcript /Folder/Book/Chapter/Scene` copies the active branch into a new manuscript scene.

## Connect the inference server

Start Neutrino's server in a terminal on the machine that can reach the provider. For example, in PowerShell with Ollama running locally:

```powershell
$env:NEUTRINO_MODEL = 'your-installed-model-id'
.\build\Release\neutrino.exe --serve
```

Open the TUI in a second terminal. Its default server address is `http://127.0.0.1:4783/v1`. The TUI sends prompts to Neutrino's server, which opens the upstream provider connection and streams the reply back.

```powershell
.\build\Release\neutrino.exe --demo
```

| Server setting | Purpose |
| --- | --- |
| `NEUTRINO_ENDPOINT` | OpenAI-compatible provider base URL. Defaults to `http://127.0.0.1:11434/v1`. |
| `NEUTRINO_MODEL` | Narrator and manuscript model ID. Required. |
| `NEUTRINO_API_KEY` | Upstream bearer credential, when required. Keep it in the server process. |
| `NEUTRINO_AGENT_MODEL` | Filing and historian model. Defaults to the narrator model. |
| `NEUTRINO_RECALL_MODEL` | Recall model. Defaults to the agent model. |
| `NEUTRINO_RECALL_ENDPOINT`, `NEUTRINO_RECALL_API_KEY` | Optional dedicated recall provider and its bearer credential. |
| `NEUTRINO_SERVER_TOKEN` | Optional credential between the TUI and Neutrino's server. Set it in both processes when enabled. |

The server listens on loopback. To use a server on another machine, forward the port over SSH, for example `ssh -N -L 4783:127.0.0.1:4783 user@server`. The TUI still needs a connection to Neutrino's server. It has no provider credentials or provider endpoint to configure. Use `--serve --port PORT` to change the listener and `--server URL`, `NEUTRINO_SERVER`, or `/server URL` to change the client address. The slash command applies to the current TUI session. A reverse proxy must pass HTTP/1.1 with a loopback Host header and stream responses without buffering.

**Esc** requests cancellation and keeps partial foreground text available for export. Windows may wait for an outstanding WinHTTP read to return. The TUI owns the workspace and its memory queue. Closing it cancels active requests; unfinished filing picks up when the workspace reopens.

## Compendium and prompt trace

Each completed narrator reply queues filing. The filing agent writes typed facts such as characters, places, items, events, and notes onto the reply that established them. Neutrino checks entries across types for verbatim repetitions and equivalent location, stranding, and concealment claims. It keeps unrecognized wording, conflicting state, and novel sentences alongside established facts. The historian compacts older turns into a cumulative recap. Recall combines relevant active entries with recent dialogue, the recap, and other story sources. Switching or swiping a branch changes the reply history and the memory entries visible to the narrator.

When a filing pass yields no new entries, Neutrino can capture an explicitly placed, previously unseen object directly from the narrator's sentence. These item records keep the original sentence as evidence and are marked `reply-evidence` in the workspace.

| Command | Action |
| --- | --- |
| `/memory` | List active entries and their IDs. |
| `/memory new TYPE \| NAME \| DESCRIPTION` | Add an entry on this branch. |
| `/memory edit ID \| DESCRIPTION` and `/memory delete ID` | Edit or hide an entry from this point on the branch. |
| `/memory mode [MODE]` | Show or set recall to `auto`, `prefetch`, `sync`, `lexical`, or `off`. |
| `/model recall recall:MODEL` | Use the configured recall model and connection. `default:MODEL` selects the server's narrator model for recall. `default` clears the override. |
| `/memory status`, `/memory retry`, `/memory refile ID`, `/memory undo ID` | Inspect, retry, redo, or remove background output. |
| `/memory off` and `/memory on` | Pause or resume the story's Compendium. |
| `/trace [FILE]` | Inspect or export the last foreground prompt from this TUI session. |

`auto` and `prefetch` use a completed recall result from the active path when available, with lexical matching as a fallback. `sync` waits for model recall on the current turn. `lexical` and recall mode `off` use keyword matching without a recall model call. Model recall failures fall back to keyword matching.

`/trace` includes the complete ordered messages as JSON, the chosen memory entries and recap coverage, and the reason each lore entry was included or skipped. Lore matching covers constant entries, primary keywords, selective secondary keywords, and the source budget. When the narrator echoes its previous reply, Neutrino discards the draft and tries once with a fresh-response prompt. The trace labels the retry. If the model echoes the reply again, the turn stays open for `/narrate`. Exported traces contain story text.

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
| Character cards | JSON CCv1/v2/v3 and PNG cards with uncompressed `chara` or `ccv3` metadata. |

Imports leave the input file intact. Workspace imports with colliding paths or IDs are rejected. `/export FILE` creates a fresh JSON backup. Use the backup on another machine or reimport it into an empty workspace.

Neutrino is a C reimplementation of the combined core workflows. Upstream features outside this port include rp-core's automated Director, native Anthropic and Gemini dialects, RoleCall ZIP import, and Xenon's persistent job runner, recursive batch generation, and browser appearance features. Roleplay presets use their enabled prompt text.

## Upstream references

- [rp-core](https://github.com/LeviTheWeasel/rp-core) at `02ac77e`. Roleplay, character cards, branches, swipes, and session journals.
- [Cherenkov / Xenon](https://github.com/Colin1337X/cherenkov/tree/codex/xenon) at `917a1b0`. Scene hierarchy, writing operations, review, Story Bible, context and portable workspaces.
