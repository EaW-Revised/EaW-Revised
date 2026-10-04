# A Windows capture host as a spare build host

A capture host may provide one spare CPU build slot while its graphical lane is
idle. Keep its addresses, account, key location and work directory in ignored
`config/build-hosts.local.json`. Copy the existing table, append the new host after
the virtual build host, and set `gpuHost` to the matching GPU-table entry. Normal
routing still prefers hosts dedicated to builds and excludes a capture host while
a viewer, suite or original-game job holds or waits for its lane.

## Provisioning

Check the host's rig queue before each administrative step. Acquire an exclusive
`other` lease through `tools/rig/Invoke-RigQueue.ps1` before installing software,
changing accounts, permissions, SSH, or firewall rules; recheck the token before
each step. Never stop an existing game, capture or another person's process.
Release the setup lease in `finally`, after installers have finished. Set every
installer to quiet mode and disable automatic restarts.

1. Inventory fixed disks, free memory, existing toolchains, SSH and the build
   account through the normal administrative remoting route. Budget 20 GiB for
   toolchains and installation caches, 25 GiB for a warm source/build tree and
   compiler cache, and leave at least 30 GiB free on each affected disk. If these
   budgets do not fit, stop. Put the work tree and toolchains on the larger disk.
2. Fetch signed installers from their publishers. Record the URL, version,
   SHA-256, signer, exit code and installed directory in a private setup receipt.
   Validate Authenticode signatures before executing them. Install Visual Studio
   **2022** Build Tools for the `Visual Studio 17 2022` presets, the C++ workload
   and its recommended SDK components, plus the LLVM compiler and ClangCL MSBuild
   toolset components. Use `--quiet --wait --norestart --nocache` and an explicit
   installation directory. The compiler-cache helper discovers this VS instance
   with `vswhere`, including its Ninja package. See Microsoft's
   [installer parameters](https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=vs-2022)
   and [Clang support](https://learn.microsoft.com/en-us/cpp/build/clang-support-msbuild).
3. Install Python 3.13, CMake 3.31, Git for Windows and PowerShell 7 for all users
   with no restart. Add their executables, VS's LLVM `x64/bin` and bundled Ninja
   to the machine PATH. Verify the installed versions in a fresh SSH session.
   The repository's pinned sccache is installed by `--iterate`; a full Visual
   Studio preset does not use a compiler launcher.
4. Create a local build account with a generated, unrecorded random password.
   Add it only to the normal Users group; verify it is absent from Administrators.
   Grant it Modify access to the build directory, with Administrators and SYSTEM
   retaining full control. Set the explicit inheritable ACL on the root folder
   alone, without `/T`. Reset children to inherit from that root, after excluding
   all reparse points from traversal; do not apply folder inheritance flags to
   leaf files. Verify executable samples have inherited SYSTEM/Administrators
   full control and build-account Modify access, with no explicit leaf rules.
   Do not grant access to
   the game or capture directories.
5. Install the Windows OpenSSH Server optional capability. If servicing refuses
   administrative remoting, run only that capability install in a one-shot SYSTEM
   scheduled task created for this setup; wait for its result and remove that
   task after it exits, while keeping the exclusive setup lease. Do not change
   remoting security or stop unrelated tasks. Stop without rebooting
   if Windows reports that a restart is required. Configure the build user's SSH
   shell as Windows PowerShell, public-key authentication, password authentication
   disabled, `AuthenticationMethods publickey`, and no agent forwarding, TCP
   forwarding or PTY. Authorize only the
   existing workstation public key, with `from="<workstation-address>"` and the
   corresponding `no-*` restrictions. Use a user-specific authorized-key file
   readable only by that user, Administrators and SYSTEM. Validate with `sshd -t`
   before starting the service. See Microsoft's
   [OpenSSH configuration](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh-server-configuration).
6. Disable the broad SSH rule introduced by the capability install; add a TCP 22
   inbound rule restricted to the workstation address. Start SSH automatically.
   Test public-key login and effective password/forwarding restrictions. Preserve
   existing unrelated services, firewall rules and Defender exclusions. Add only
   the build work directory to Defender exclusions if the existing build-host
   setup documents that practice; otherwise leave exclusions unchanged.

## Game priority and bounded waiting

Set `slots: 1`, initially `defaultJobs: 4`, `maxJobs: 4`, and
`maxRunSeconds: 1800`. The default can be raised only after measuring available
physical memory throughout a cold build; keep at least 2 GiB available. Builds
already run below normal priority and use the shared rig queue's `build` kind.
An original session is exclusive in either arrival order. Once a build reaches
the head of the shared queue, a waiting original job goes ahead of the queued
build backlog; active holders and older graphical/manual jobs still finish first.
The owner's explicit pause of original-game jobs remains in force.

Allow the active build to finish, with a 30-minute host-side limit, rather than
interrupting every build as soon as a game job queues. This preserves useful
progress and keeps the wait bounded. The existing Windows supervisor snapshots
`maxRunSeconds` from its client lease before starting the job, and enforces it
with a monotonic clock even while the client keeps sending heartbeats. At the
limit it terminates only its own job object and waits for every descendant to
exit. The client releases the shared rig lease only after the supervisor emits
`EAWR-STATUS build tree stopped`; a time-limited job fails with a transport result.

If SSH loses the completion proof, the client retains the rig lease until its
expiry instead of allowing a capture alongside an unconfirmed build. That lease
lasts the runtime cap plus 30 minutes to cover bounded transport, staging and
cleanup; the host supervisor still stops the build at its own limit. An
unreachable host therefore delays the game queue longer, but cannot start a
capture over a known running build. Never break another worker's lease.

## Acceptance before enabling the host

Run the CI offload Python suites in one test process. Measure the same full
`windows-msvc` job, cold and warm, on the candidate and the virtual build host;
save stage timings, minimum available memory, peak committed memory and queue
waits under ignored `out/`. Compare waiting with and without the extra candidate
using the real router. Retain the entry only when it improves queue completion
without violating the memory or game-exclusion gates. Test both lease arrival
orders, timeout containment and loss of completion proof. Validate a real
`windows_build.py --host <candidate-alias>` job, `--host vm`, Linux offload,
clean-room checks and documentation references before making the PR ready.
