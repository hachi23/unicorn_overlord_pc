## What and why

<!-- A few sentences. Link the issue: "Fixes #123". -->

## How it was tested

<!-- Mark irrelevant checks N/A with a reason. Report skipped tests separately.
     Docs-only changes need content/link checks; native changes need affected native tests. -->

- [ ] `bash build.sh`
- [ ] `python -m unittest discover -s tests`
- [ ] `bash build.sh --test`
- [ ] `--check-imports` shows no new missing imports
- [ ] Played in game. Scene(s) and how far:

OS / GPU / driver:

## Before / after

<!-- FPS and stage A/B µs per draw for performance changes (same scene, same machine).
     Include frame times, multiple affected scenes, cold/warm cache state and regressions.
     Screenshots for visual changes. Delete if not relevant. -->

## Risk

<!-- What could break? Do saves, vw.ini keys or the out/ layout change? -->

## Checklist

- [ ] One topic in this PR
- [ ] No game files, saves, keys or dumps (`git status` checked)
- [ ] New logic has a test in `tests/`
- [ ] Docs updated (README, docs/MODS.md, docs/dev/unicorn/HANDOFF.md) if behaviour or options changed
- [ ] Commit subjects follow `Area: what changed`
- [ ] I understand and have validated the change, including any AI-assisted code
