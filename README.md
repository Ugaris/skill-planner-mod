# Skill Planner

A native client mod for [Ugaris](https://ugaris.com): a full build
calculator on top of the client's own cost function. Type `#plan`.

- **Exact costs** — every raisable skill with the cost of its next point
  (identical to the skill window's numbers) and how many points your
  unused experience can buy right now.
- **Targets** — click a skill, set a target with the `[-]` / `[+]` buttons
  (shift = 10 points). Each targeted skill shows the exact total cost,
  green when affordable; the footer sums the whole build.
- **Apply** — one confirmed click raises the entire build for you. Each
  raise waits for the server's acknowledgment before the next is sent, and
  the run stops cleanly if experience runs out. `#plan stop` cancels.
- **Builds as shareable JSON** — `#plan save <name>` writes
  `skill_build_<name>.json` (keyed by skill *names*) into your client
  config dir. Share the file with other players; they `#plan load <name>`
  and work toward the same build. `#plan builds` lists what you have.
- **Draggable** — move the window by its title bar; the position, targets
  and sorting persist.

## Commands

| Command | Effect |
|---------|--------|
| `#plan` | Toggle the planner window |
| `#plan save <name>` | Save current targets as a build |
| `#plan load <name>` | Load a build (skills matched by name) |
| `#plan builds` | List saved builds |
| `#plan clear` | Clear all targets |
| `#plan apply` | Apply the build (asks to confirm) |
| `#plan stop` | Stop an in-progress apply |

## Installing

Install from the Ugaris Launcher: **Mods → Browse → Skill Planner**, or via
*Install from URL* with `Ugaris/skill-planner-mod`.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On Windows, first copy `lib/moac.a` / `lib/moac.lib` from the client
release's `mod-sdk.zip` into `lib/` — mods link against the client's import
library there. Released binaries are built by GitHub Actions from tags.

## License

MIT
