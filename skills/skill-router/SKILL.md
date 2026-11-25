---
name: skill-router
description: >-
  Find and load the right skill on demand instead of keeping every
  skill description in context. Use when you need a skill but are
  not sure which one, or when the skill catalog is too large to load.
---

# Skill Router

The skill catalog on this machine is indexed by `skillctl`. Resolve an
intent to a skill, then read only that skill. Two calls, minimal context.

## 1. Rank candidates by intent

   skillctl find "build a godot UI with shaders"
   skillctl find --json --limit 5 "deploy macos app"
   skillctl find --mode bm25 "exact wording I expect in the body"

Modes: auto (default: BM25F over skill bodies plus the metadata
list), bm25, hybrid (adds the dense list), vector. --explain adds
per-list ranks so a wrong hit is diagnosable. Output is one line
per hit, capped at 8 by default.

auto deliberately stays lexical: a dense list only helps when the
encoder actually reads the corpus. Use --mode hybrid after installing
a bilingual model (see README).

## 2. Fetch the winner on demand

   skillctl show <name>            # prints its SKILL.md
   skillctl show <name> --meta     # name/description/path only

## Hard rules

- This workflow is mandatory: find first, then show. Never walk the
  skill directories yourself, never read a SKILL.md path directly,
  never list a skill root by hand. Those paths are an implementation
  detail of the store and bypass the index, which is what makes
  selection reproducible.
- `show` accepts a name, a unique prefix, or a unit id. On a duplicate
  name it prints the candidates with their ids and exits 2; pick one
  id rather than guessing.
- One intent per call. Use --limit to keep output small.
- If nothing matches, rephrase with the domain vocabulary the skill
  itself would use (engine, language, artifact) before concluding that
  no skill exists.
- A stale index is still usable; `skillctl index` refreshes it.
