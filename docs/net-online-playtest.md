# Nine-player online crew playtest

This runbook targets the crew preview with authenticated in-battle connection repair.
Use the viewer's Online lobby with the same preview build and the same effective game/mod data
on every machine. The small command-line online kit is useful for a two-player connection check;
the nine-player run uses the viewer. This is a functional cross-network qualification, not a
performance measurement. The [mesh design](net-online-npeer.md) describes the required coverage.

## Before the run

1. The organizer records the preview build, data/mod selection and nine tester names. Label the
   networks A, B and C; place at least three players on each of three independent networks.
   Several processes can share a machine/network for the dry run, each with its own profile.
2. Every tester sets the supplied online service in Network Options, chooses a unique lobby
   name, and keeps their logs for the whole run. Do a two-player check first if that network
   has not connected before. Record whether ordinary or relay-only mode is selected.
3. Keep every process running until the organizer has collected the result. Record the
   real elapsed time when anyone stalls or leaves, and which peers still see play advancing.

## Host

1. Open Online, choose Host Online, and select **9** under **Human players**.
   Each human needs a map row; hosting chooses a fitting loaded map if the previous one is too small.
   Use a nine-row map such as Saleucami. The create dialog has no Teams selector.
   In staging each human gets a separate named row with its own faction, colour and **Team**.
   Set each row's Team to form alliances: for 3 vs 3 vs 3, assign three rows to each of Teams 1, 2
   and 3 on a map with those teams. For an eight-human 4 vs 4, assign four rows to Team 1 and four
   to Team 2. Players on a team must use the same faction; each keeps its own units and credits.
2. Give the crew the room name and, for a private room, its password through the agreed private
   channel. A public room can also be joined with its four-character room code.
3. Admit testers one at a time, waiting for each to appear in staging before the next joins.
   This avoids filling the service's limited outstanding reservations. Open **My connections**
   to inspect the seats; ask every tester to check their own links as well.
4. Confirm every expected human is present on a separate named row, the per-row Team numbers
   match the intended alliances, and every tester has reviewed the map and settings and pressed Accept
   after the last join. Start waits for every pair (28 for eight humans, 36 for nine) and every
   human's current acceptance. **My connections** counts your own links; a host's own open links
   alone are insufficient. Hover over a blocked Start for its reason.
   If a setting changes, have everyone accept the new revision.
5. Play for at least ten minutes, including after the five-minute mark. Ask each tester to
   move units, issue an attack, spend credits and use reinforcements for their own player.
   Record any stalled inputs, unexpected defeats or different results between peers.

## Every tester

1. Join the listed room, entering its password when requested, or enter the public room code.
   Do not choose a separate seat count: the room supplies it.
2. Check your name, faction, colour and Team on your own staging row. Review the map preview and read-only settings.
   Open **My connections**: report every named connecting or failed link, and whether an open
   connection reads **Connected directly**, **Connected through relay**, or just **Connected**.
   Close it to review the setup.
3. Press Accept after reviewing the choices. Team/faction errors explain why acceptance is
   unavailable; the host may keep editing the setup while those errors are present.
4. In battle, report elapsed time, the exact visible message/code and which player's connection
   it names. If removed, report it to the organizer before attempting a rejoin.

## Follow-up cases for the dry run

Run an eight-human 4 vs 4, then an uneven alliance split with two or more teams. A nine-human
match needs a map with nine distinct rows; shared sides belong to galactic conquest.
Exercise a refused newcomer followed by another tester filling that seat, and an ordinary pre-Start leave
and refill. Use a relay-only network where available and record that route in the result.

### Connection repair exercise

For this exercise, every tester joins through the room list. Public-code-only joins currently
lack the canonical room identity needed for credential refresh; their lost pair may report
EAWR-NET-1791 and proceed to the agreed departure instead of repairing. Capture that cause;
rejoining a removed seat is unavailable. With the organizer's agreement,
interrupt one test connection in battle and restore it during the repair grace; wait for the
outcome and check that play resumes together. The named message progresses through refreshing
room credentials, restoring the connection and checking it; gameplay and audio stay paused until
all continuing players confirm the repair. Avoid repeatedly pressing Leave or reconnecting.
Then try a connection that remains absent:
survivors must agree who left, and the removed player must see the named reason. A network split
without an agreed continuing majority ends the match; no group should keep playing by itself.
Host loss before Start closes the room. Do not interpret a loopback nine-process pass as proof
of symmetric NAT, UDP-blocked networks, relay renewal or service-outage behaviour.

## When it goes wrong

Every tester keeps **`user://logs/godot.log`** and the **`lan-desync/`** directory, if one was
created. `user://` is the viewer's user-data directory for that profile; use the platform's
Godot application-data location. Preserve the logs immediately after the failure and before
another launch rotates them. Absence of a diagnostic bundle is useful information: report it.

Send the organizer one archive per tester through the agreed private playtest channel. Include
the full log and diagnostic directory, the build identifier, effective mod/data selection,
tester/network label, host or joiner role, approximate elapsed time, exact on-screen message
and code, who was named, and whether the other peers continued. A screenshot of the My connections
list before Start and of the failure message helps correlate the nine accounts.

The organizer records one timeline for the room, collects all nine reports (including healthy
survivors), and passes the archives to the development coordinator. Missing network lines in
`godot.log` do not establish that no connection failed; retain the visible message and timing
alongside the log so a separate logging investigation can use them.
