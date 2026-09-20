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
