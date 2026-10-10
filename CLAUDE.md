**Where the repository lives.** Development, CI and releases are on
`git.geisten.net/geisten/geist-runtime` (Gitea, private).
`github.com/geisten/geist-runtime` is a push mirror of it: anything pushed to
GitHub directly is overwritten on the next mirror sync, so never push there.
Push branches to the Gitea remote and open PRs with `tea pulls create --login
git.geisten.net --repo geisten/geist-runtime`; merge with a merge commit. CI is
`.gitea/workflows/` (`.github/workflows/` only serves pull requests on the
mirror). A PR opened on GitHub by someone else comes over with
`scripts/import-github-pr.sh <n>` after reading it.
