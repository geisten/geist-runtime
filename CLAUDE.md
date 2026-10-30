**Where the repository lives.** Development, CI and releases are on
`git.geisten.net/geisten/geist-runtime` (Gitea, private).
`github.com/geisten/geist-runtime` is a push mirror of it: anything pushed to
GitHub directly is overwritten on the next mirror sync, so never push there.
Push branches to the Gitea remote and open PRs with `tea pulls create --login
git.geisten.net --repo geisten/geist-runtime`; merge with a merge commit. CI is
`.gitea/workflows/` (`.github/workflows/` only serves pull requests on the
mirror). A PR opened on GitHub by someone else comes over with
`scripts/import-github-pr.sh <n>` after reading it.

**Agent pipeline.** Issues and pull requests move through status labels
(`.gitea/workflows/agent.yml` has the details, geistlib's `AGENT.md` §9 the
same pipeline; how all agents share work: `geisten/orga` `AGENTS.md`):
`agent` on an issue: CI's Claude Code implements it and the bot opens the
PR; `status:validate` on a PR: Opus reviews it against the issue;
`status:accepted`: merges itself once the required CI is green;
`status:fix`: the agent works the review or the CI failure; `status:stuck`:
a human's turn. Working on your own: claim the issue with
`orga/bin/agent-claim <n>`, open the PR from `agent/issue-<n>` with
`Closes #<n>` and a `## Assumptions` section, label it `status:validate`.
CI on an `agent/*` head waits until an approver read the commit and
commented exactly `/ci-ok <sha>` (ci.yml `hold`): job containers reach the
docker socket, the Mac legs run on the host, and jobs see GEISTLIB_TOKEN.
A patch touching `.gitea/`, `.github/`, `scripts/`, `Makefile`,
`packaging/`, `install.sh`, `CLAUDE.md`, `CHANGELOG.md` or any `.git*` file
is refused (`scripts/agent_gate.py`).
