---
name: skill-router
description: >-
  Find and load the right skill on demand instead of keeping every
  skill description in context. Use when you need a skill but are
  not sure which one, or when the skill catalog is too large to load.
---

# Skill Router

Use the `skillctl` CLI to resolve an intent to a skill, then read
only that skill. Two calls, minimal context:

1. Rank candidates by intent (names + one-line descriptions):

   skillctl find "build a godot UI with shaders"
   skillctl find --json --limit 5 "deploy macos app"

2. Fetch the winner on demand (prints its SKILL.md):

   skillctl show <name>
   skillctl show <name> --meta    # name/description/path only

Rules:

- Do not scan skill directories by hand; `find` searches the whole
  catalog in one call.
- Prefer an exact name from the hit list. `show` accepts a name, a
  unique prefix, or a unit id; on ambiguity it lists the candidates
  and exits 2.
- One intent per call; use --limit to keep output small (default 8).
- If nothing matches, rephrase with the domain vocabulary (engine,
  language, artifact) before concluding no skill exists.
