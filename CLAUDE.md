# CLAUDE.md

Project-specific instructions for obs-ptz. See also [AGENTS.md](AGENTS.md) and
[CONTRIBUTING.md](CONTRIBUTING.md).

## Commit attribution

This repo has its own AI-attribution convention (CONTRIBUTING.md, "AI-Assisted
Contributions"). Use it instead of the generic `Co-Authored-By:` trailer:

```
Assisted-by: Claude:claude-sonnet-5
```

Never add a `Signed-off-by:` line — that's a DCO attestation only the human
contributor can make, even though CONTRIBUTING.md's example shows the two
trailers adjacent.

Never open a PR for untested code. Tell the human to test first, and
offer to help them build and run locally.

## Building and running on macOS

Use `scripts/macos-dev.sh` (`setup`, `install`, `run`, `restore`) to configure,
build, sign and run in any checkout, worktrees included, instead of running
`cmake` and `open` by hand. It gives the worktree its own copy-on-write clone
of the main checkout's `.deps` (never a symlink: worktrees sharing one `.deps`
break each other's configure), and it pins the macOS packages when configuring, which a plain
`cmake --preset macos` gets wrong when `.deps` also holds Windows builds.
