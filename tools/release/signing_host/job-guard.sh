#!/usr/bin/env bash
# Job guard of the release signing host (tools/release/signing_host/README.md, "The job guard").
#
# The runner calls this script before every job it accepts (ACTIONS_RUNNER_HOOK_JOB_STARTED in the runner's .env).
# A non-zero exit fails the job before any step of the workflow runs. The script is installed root-owned and not
# writable by the runner user, and it does not come from the repository checkout of the run, so a workflow file edited
# on another branch cannot change or skip it. It allows exactly one thing: the `Sign release` workflow of this
# repository, started by hand (workflow_dispatch) from main. The reason it prints names the variable, never its value.
set -u

WANT_REPOSITORY="EaW-Revised/EaW-Revised"
WANT_WORKFLOW_REF="EaW-Revised/EaW-Revised/.github/workflows/sign-release.yml@refs/heads/main"
WANT_EVENT="workflow_dispatch"
WANT_REF="refs/heads/main"

refuse() { echo "eawr job guard: refused: $1 is not the allowed value" >&2; exit 1; }

[ "${GITHUB_REPOSITORY:-}" = "$WANT_REPOSITORY" ] || refuse GITHUB_REPOSITORY
[ "${GITHUB_WORKFLOW_REF:-}" = "$WANT_WORKFLOW_REF" ] || refuse GITHUB_WORKFLOW_REF
[ "${GITHUB_EVENT_NAME:-}" = "$WANT_EVENT" ] || refuse GITHUB_EVENT_NAME
[ "${GITHUB_REF:-}" = "$WANT_REF" ] || refuse GITHUB_REF
exit 0
