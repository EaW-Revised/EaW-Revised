# The release signing host

Releases are built and packaged unsigned by the build pool. Their Windows executables are then signed by Azure
Artifact Signing, on one small dedicated host, by the `Sign release` workflow. This folder is the setup for that host.
The flow and the verification are in [docs/releasing.md](../../../docs/releasing.md#code-signing).

## What the host is, and is not

- One small Linux container or VM (1 core, 1 GB, 8 GB disk is plenty) that does nothing else. Not a build worker, not
  the maintainer's PC, no agent sessions, no shared credentials, no other services.
- It holds **no secret at rest**: no password, key, certificate, client secret or Azure login. The signing keys stay in
  Azure. The workflow gets a short-lived Azure token from the job's own GitHub OIDC identity (a federated credential
  bound to the `release-signing` environment), uses it for the minutes the run takes and discards it.
- A maintainer drops the unsigned archives on it through an SFTP-only account that can write the inbox and read the
  outbox, and nothing else. That key cannot sign: signing needs the workflow, which needs the repository owner's
  approval in GitHub.

## Set it up (maintainer, once)

1. Create the container or VM (Debian 12 or Ubuntu 24.04, unprivileged, no bind mounts, no nested features). Give it
   outbound HTTPS only: `github.com`, `token.actions.githubusercontent.com`, `login.microsoftonline.com`,
   `*.codesigning.azure.net`, `timestamp.acs.microsoft.com` (plain HTTP), and the distribution mirrors for updates. If
   the network can restrict egress by name, restrict it to these.
2. Copy a checkout of this repository onto it and run, as root:

   ```text
   OPERATOR_CIDR=<network allowed to reach ssh> DROP_PUBKEY_FILE=<ssh public key file of the release operator> \
   RUNNER_VERSION=<actions runner version> RUNNER_SHA256=<sha256 shown on that runner release page> \
   bash tools/release/signing_host/setup-signing-host.sh
   ```

   It installs Java 17, Python, `osslsigncode`, a key-only sshd, an inbound firewall (ssh from the named network only),
   the pinned `jsign` jar (checked against `config/signing-tools.json`) and the runner files. It creates two users:
   `eawr-drop` (SFTP only, chrooted to the drop folder) and `eawr-signer` (runs the runner, no sudo, no login
   password). It never asks for or stores a credential. `bash setup-signing-host.sh selfcheck` prints the state later.
3. Keep the administrator login to the host out of band (the hypervisor console). Do not add other accounts.

## Register the runner (repository owner, once)

The registration token is a credential: only the owner types it, on the host's console, never in chat, mail, an
issue or a file.

1. GitHub: repository Settings, Actions, Runners, New self-hosted runner, Linux x64. Copy the token it shows (it
   expires after an hour).
2. On the host console, as root:

   ```text
   runuser -u eawr-signer -- bash -c 'cd /opt/actions-runner && ./config.sh --url <repository url> --name eawr-signer --labels eawr-signing --work _work'
   ```

   Paste the token at the prompt. Then `cd /opt/actions-runner && ./svc.sh install eawr-signer && ./svc.sh start`.
3. Settings, Actions, General: set approval for workflows from outside collaborators to the strictest option. The
   runner belongs to this repository only; never let pull request workflows use it.
4. Settings, Environments, `release-signing`: required reviewer is the owner, and **deployment branches is set to `main`
   only**. Check both before the first real run.

## The job guard (what keeps the signing runner on `main`)

`workflow_dispatch` runs the workflow file of whichever branch was picked, so anyone who can push a branch could copy
`sign-release.yml` there, edit it and start it. A condition inside that file cannot stop its own edited copy. Three layers
stop it, and these are the only ones that count:

1. **The environment's deployment branch policy** (GitHub setting, step 4 above): a job in `release-signing` from any
   other branch is refused by GitHub, so it never gets the environment variables or the OIDC credential. This is the
   enforcement against a modified workflow.
2. **The job guard on the host** (`job-guard.sh`, installed by the setup script): the runner calls it before every job
   (`ACTIONS_RUNNER_HOOK_JOB_STARTED=/opt/eawr-signing/job-guard.sh` in `/opt/actions-runner/.env`). It exits 1, and the
   job fails before any step runs, unless `GITHUB_REPOSITORY` is the one repository, `GITHUB_WORKFLOW_REF` is this
   workflow file at `refs/heads/main`, `GITHUB_EVENT_NAME` is `workflow_dispatch` and `GITHUB_REF` is `refs/heads/main`. It
   prints one line naming the variable, never the value. The script is root-owned (0755) and lives outside the runner's
   work area, so a workflow cannot change or skip it. This also covers the runner picking up any other repository's or
   branch's job.
3. **The workflow itself** (`if:` on the job plus a first step that fails loudly): protects against a mistake by a
   maintainer, not against an edited copy.

`bash setup-signing-host.sh selfcheck` reports the guard: installed root-owned 0755, same as the repository copy, and the
hook line present in the runner's `.env`. After a rebuild, or after changing `job-guard.sh`, rerun the setup script and
restart the runner service (`./svc.sh stop && ./svc.sh start` in `/opt/actions-runner`) so it reads the `.env` again.

## Prove the route with a throwaway binary (before the first release, and after any change to the flow)

The account has one certificate profile (the real one), so the test uses it on a clearly named throwaway file that is never
a release artifact: a copy of any small unsigned Windows program, named exactly `eawr-signing-test.exe`. The signature is
real and timestamped, and counts as one signing in the monthly quota.

1. Upload it as `inbox/signing-test/eawr-signing-test.exe`.
2. `gh workflow run sign-release.yml --repo <repository> -f mode=test` and have the owner approve.
3. Fetch `outbox/signing-test/` (the signed file, `signing-test-receipt.json`, `osslsigncode-verify.txt`), then on Windows run
   `Get-AuthenticodeSignature .\eawr-signing-test.exe | Format-List` (status `Valid`, a timestamp, the expected signer).

## Sign a release (release operator)

1. Cut and package the release as usual (unsigned), so that a release directory holds the archives,
   `SHA256SUMS.txt` and `update-manifest.json`, and the annotated tag is pushed.
2. Drop the directory: `sftp eawr-drop@<signing host>` and upload it as `inbox/<tag>/`. Compute
   `sha256sum SHA256SUMS.txt` locally for the request.
3. Start the workflow: `gh workflow run sign-release.yml --repo <repository> -f mode=release -f tag=<tag> -f sums_sha256=<that hash>`.
   The owner approves the run in GitHub (every run).
4. When it succeeds, fetch `outbox/<tag>/` (signed archives, rewritten `SHA256SUMS.txt` and `update-manifest.json`,
   `signing-receipt.json`). Then check it again on your own machine, which needs no credential:

   ```text
   python tools/release/sign_release.py verify <release dir> --final <fetched outbox dir> --subject <legal name on the certificate>
   ```

   (it needs `--subject <legal name on the certificate>` or `SIGNING_SUBJECT`, and fails without one), and run the
   fresh-user gate and `tools/release/verify_signatures.ps1` on the final bytes (Windows).
5. Upload the final bytes to the draft release as usual. The unsigned pool bytes and the pool provenance stay with
   the release evidence; the receipt links each unsigned file to its signed one.

## Hardening that stays true

- No sudo for the runner user, no Azure CLI, nothing under `~/.azure`; `selfcheck` reports both.
- The job guard is root-owned and read-only too (above); the runner user can change neither it nor the jar.
- The jsign jar is root-owned and read-only; `sign_release.py` refuses any jar that does not match the pin.
- The workflow reads only `inbox/<tag>/`, and only when its `SHA256SUMS.txt` has the SHA-256 named in the request and
  the update manifest is for that tag, which must exist in the repository as an annotated tag.
- Update the host with `apt` (unattended security updates are on). To move to another jsign version, change
  `config/signing-tools.json` in a reviewed change and rerun the setup script.
- If the host is ever in doubt, destroy it and rebuild it from this folder. There is nothing on it to recover. Then
  remove the runner in the repository settings and delete the federated credential in Azure until it is rebuilt.
