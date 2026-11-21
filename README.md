# Neutrino

A native C terminal workspace for writing fiction and playing it out. Manuscript and Roleplay share one library, one Story Bible, and one model connection. The interface takes cues from Claude Code and Codex CLI's keyboard-first workflows and uses its own layout, palette, and typography.

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

## Connect a model

Set `/model MODEL_ID`. The default endpoint is Ollama's OpenAI-compatible API at `http://127.0.0.1:11434/v1`. Set `/endpoint URL` for another OpenAI-compatible chat-completions server. The environment variables `NEUTRINO_ENDPOINT` and `NEUTRINO_MODEL` override saved settings. If authentication is required, supply `NEUTRINO_API_KEY` in the process environment. Workspace saves and exports omit known API-key fields.

Foreground generation uses streaming chat completions, with support for regular JSON responses and separate provider reasoning. **Esc** requests cancellation and keeps partial text available for export. Windows may wait for an outstanding WinHTTP read to return.

## Import and export

Use `/import FILE` or `--import FILE` to bring data into a Neutrino workspace.

| Format | Imported data |
| --- | --- |
| Cherenkov v1 / Xenon v2 workspace JSON | Hierarchy, scene text, controls, revisions and Story Bible. A fresh workspace also takes the source configuration and scratchpad. |
| rp-core v1 session JSONL | Cast and persona, messages, branches, active head, message edits, hidden flags and reasoning. |
| Solo SillyTavern JSONL | Messages, selected swipes, alternate replies and hidden flags. An already imported matching card supplies character context. |
| Character cards | JSON CCv1/v2/v3 and PNG cards with uncompressed `chara` or `ccv3` metadata. |

Imports leave the input file intact. Workspace imports with colliding paths or IDs are rejected. `/export FILE` creates a fresh JSON backup. Use the backup on another machine or reimport it into an empty workspace.

Neutrino is a C reimplementation of the combined core workflows. Upstream features outside this port include rp-core's automated Director and Compendium agents, native Anthropic and Gemini dialects, RoleCall ZIP import, and Xenon's persistent job runner, recursive batch generation, and browser appearance features. Roleplay presets use their prompt text, with no upstream slot or sampler emulation.

## Upstream references

- [rp-core](https://github.com/LeviTheWeasel/rp-core) at `02ac77e`. Roleplay, character cards, branches, swipes, and session journals.
- [Cherenkov / Xenon](https://github.com/Colin1337X/cherenkov/tree/codex/xenon) at `917a1b0`. Scene hierarchy, writing operations, review, Story Bible, context and portable workspaces.
