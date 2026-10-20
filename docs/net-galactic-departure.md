# Galactic multiplayer departure

GCMP-09 implements owner decisions D5 and D8 for the first Galactic multiplayer release.

The network installs `SeatLeave` at its agreed cutoff tick. The galaxy applies the configured seat-control policy
([WGS-P02/P04](behaviour/walks/gc-session-save.md)): the lead moves to the lowest active human seat, and the last
human leaving a faction activates its configured AI/passive fallback. Controller configuration and persistent state
are retained. The campaign continues; the remaining human can keep playing or quit.

`GalacticDeparturePolicy::configured_fallback` names that outcome. Galactic LAN sessions select
`SessionOptions::unconfirmed_departure = UnconfirmedDeparture::continue_alone`. This option applies only when
there is no other surviving voter. With multiple survivors, departure still requires their existing frontier,
recovery and acknowledgement agreement. The default `connection_loss` policy, including tactical play, is unchanged.

A two-peer link split cannot distinguish which peer permanently left. Each side can therefore continue alone,
with its own human faction and configured AI fallback for the other faction. These become separate campaigns;
they are not reconciled. A departed seat stays departed. Galactic rejoin is disabled until checkpoint-based rejoin
in GCMP-15 (legacy EAWR-4368), and a `SeatRejoin` command is refused explicitly. This is a project policy, not a retail claim.

The LAN view reports the departed seat and agreed tick. A local leave ends local play; a remote departure keeps
gameplay enabled. Missing seat-control policy fails before installing departure inputs, so the network cannot
remove a voter while the galaxy silently refuses its leave.

The option adds no wire fields or message kinds and does not change peer wire revision 3.
