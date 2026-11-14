# Builds, branch protection and releases

## Automatic builds

`.github/workflows/build.yml` runs on every pull request and every merge to `main`:

| Job | Runner | Result |
|---|---|---|
| Linux | Ubuntu 22.04, GCC 14 | `Unicorn-Overlord-shadPS4-x86_64.AppImage` (runs on glibc 2.35 and newer: Ubuntu 22.04+, Debian 12+, SteamOS 3.5+) |
| Windows | MSYS2 CLANG64 | `Unicorn-Overlord-shadPS4-PC-windows.zip` (the start program, `_internal/`, licenses and help) |
| Publish | main and `v*` tags | main: replaces the `latest` pre-release; a `vX.Y.Z` tag: publishes that release. Both with `SHA256SUMS` |

Every run keeps both files as workflow artifacts (Actions tab → the run → Artifacts), so a pull
request can be tested before it is merged. Both packages store their version (`git describe`) in
`VERSION`; `run.py` writes it at the top of every run log.

The Vulkan headers, glslang, VMA and robin-map versions are pinned in the workflow's `env:`
block. The static libraries (FFmpeg, SDL3, ...) are pinned in `packaging/deps.sh`. Both are
cached; changing a pin rebuilds it.

Both jobs run the C++ tests and platform-specific native tests via
`packaging/test-native.sh`, then Python unittest discovery. Linux also runs the C runtime
and loader tests. The Vulkan cache test runs with Mesa lavapipe on Linux. Windows builds
it but skips execution because hosted runners have no Vulkan ICD; the other native tests
run on both platforms. No game dump or physical GPU is needed. Report skips separately.

Packaging fails if required dependency notices are missing. `packaging/notices.py` collects
vendored, static-library, Python/Tcl/Tk and installed toolchain notices. When bumping header
dependencies, update `packaging/licenses/` too. Inspect both final archives after dependency
changes. The AppImage contains its notices; the Windows ZIP keeps them beside the executable.

Matching native debug symbols (`*-symbols-<commit>.tar.gz`) and source
(`*-source-<commit>.tar.gz`) are retained in CI artifacts and attached to the latest build.
Keep the symbols for each versioned release too: `addr2line -e vwport.debug <module-offset>`
can resolve the native offsets from a crash log. The source archive includes checked-out
submodules and downloaded static dependency archives, with build recipes in the port source.

Locally: `python3 packaging/package.py unicorn` then `bash packaging/linux/appimage.sh "Unicorn Overlord"`
(Linux), or `bash packaging/windows/build.sh unicorn` in MSYS2 CLANG64.

## Branch protection (repository owner, once)

`.github/main-protection.json` is a repository ruleset for the default branch:

- no direct pushes: every change goes through a pull request, admins included
- every change needs a PR and all review threads must be resolved
- the `Linux` and `Windows` build jobs must pass against an up-to-date base
- no force-push, no deletion

Apply it with the GitHub CLI (needs admin rights on the repository):

```bash
gh api -X POST repos/hachi23/unicorn_overlord_pc/rulesets --input .github/main-protection.json
```

Or in the web interface: Settings → Rules → Rulesets → New ruleset → Import a ruleset, and pick
the file.

The status checks only appear after the workflow has run once, so merge the pull request that
adds the workflow first, then apply the ruleset.

**Single maintainer:** the template requires zero formal approving reviews because the owner
and their connected automation cannot approve their own PRs. It still requires a PR, resolved
threads, current passing builds and no bypass actors. The relevance/correctness review and test
evidence in CONTRIBUTING.md remain required. Once an independent reviewer is available, set
`required_approving_review_count` to 1. The JSON is not active until applied in repository settings.

## Making a release

1. Merge to `main`. The `latest` pre-release updates by itself.
2. Play the `latest` build: title, new game, the first battle, save and load, on Linux and
   Windows.
3. Tag the tested commit with an annotated `vMAJOR.MINOR.PATCH` tag and push it. The tag's
   message is the release notes (players see its start in the update dialog):
   ```bash
   git tag -a v0.1.0 <commit>     # first line: a title; then what changed, for players
   git push origin v0.1.0
   ```
   The Build workflow builds that commit (its version is the tag), then publishes the release
   with the AppImage, the Windows ZIP, symbols, source and `SHA256SUMS`.
4. Packaged builds look for releases at start (`scripts/updates.py`, unless `check_updates=0`
   in `vw.ini` or `VW_UPDATES=0`). A build older than the newest `vX.Y.Z` release offers it:
   **Update now** downloads it, checks it against `SHA256SUMS` and installs it, keeping
   `data/` (settings, saves, the installed game); **Skip this version** stops asking until a
   newer one exists. The rolling `latest` build counts as older than every release.
   Never delete or move a published tag: players' updaters compare against it.
