**Where the repository lives.** Development, CI and releases are on
`git.geisten.net/geisten/geist-runtime` (Gitea, private).
`github.com/geisten/geist-runtime` is a push mirror of it: anything pushed to
GitHub directly is overwritten on the next mirror sync, so never push there.
Push branches to the Gitea remote and open PRs with `tea pulls create --login
git.geisten.net --repo geisten/geist-runtime`; merge with a merge commit. CI is
`.gitea/workflows/` (`.github/workflows/` only serves pull requests on the
mirror). A PR opened on GitHub by someone else comes over with
`scripts/import-github-pr.sh <n>` after reading it.

**Agent heads wait for a human.** Agents (local Claude/Codex sessions,
temporary environments; no model runs in CI) claim an issue with
`geisten/orga` `bin/agent-claim <n>` and work on `agent/issue-<n>`
(`geisten/orga` `AGENTS.md`). CI runs nothing of an `agent/*` head until an
approver read the commit and commented exactly `/ci-ok <sha>` on the PR
(ci.yml `hold`, `scripts/agent_gate.py ci-hold`; ci-ok.yml reruns the held
run): job containers reach the docker socket, the Mac legs run on the
host, and jobs see GEISTLIB_TOKEN. Each new push needs its own `/ci-ok`.
