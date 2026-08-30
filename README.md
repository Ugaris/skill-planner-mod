# Skill Planner

A native client mod for [Ugaris](https://ugaris.com) that makes the
raise-cost math visible. Type `#plan`:

- Every raisable skill with its current value and the **exact cost of the
  next point** — the same numbers the skill window shows, computed with the
  client's own cost function.
- **How many points you can afford** with your unused experience, per skill.
- **Targets**: click a skill, set a target value with `[-]` / `[+]`
  (shift = 10 points), and see the exact total cost from where you are —
  green when you can afford it, red with the real number when you can't.
- Sort by table order or cheapest-next-point first.
- Header with your level, unused and spent experience.

Targets and the sort choice persist in `<client config dir>/skill_planner.cfg`.

## Commands

| Command | Effect |
|---------|--------|
| `#plan` | Toggle the planner window |
| `#plan help` | Help |

Mouse: click a skill to select it, `[-]`/`[+]` to move its target, wheel
scrolls, `[x]` closes.

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
