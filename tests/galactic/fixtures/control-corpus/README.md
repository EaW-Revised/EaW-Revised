# Political control corpus

`political-conquest.eawr-gcreplay` is a synthetic opt-in control recording authored
through `GameSession`: movement to a neutral planet, an uncontested transport
landing, pause/resume, and gradual allegiance through the one-point threshold.
It includes authored leaning, route, capture-reward and transition-rate inputs;
existing galaxy and tactical corpus pins are unchanged.

`galactic_control_replay_corpus` compares the new hash pin on 1, 2, 4 and 8
workers. The C++ control contracts additionally compare hashes, events and
snapshots, and test defended admission, the FT-08 land-result adapter, callbacks
and checkpoints inside a supported space child and a half-complete political
transition. Land child entry remains the conflict-admission producer's seam.
