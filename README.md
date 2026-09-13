# NEATGD

A [Geode](https://geode-sdk.org) mod that trains an AI to play Geometry Dash
levels for you, using NEAT (NeuroEvolution of Augmenting Topologies).

## Features

- **Three modes**: NEAT (evolving neural-network brains, transferable between
  levels), Climber (a single evolving click sequence, fast but level-specific),
  and Hybrid (a network that plays behind a shared locked click prefix).
- **Softlock repair**: stuck runs unlock and re-search a bounded window of
  earlier clicks instead of stalling forever.
- **64 sensors**, including nearest orb/pad/portal detection.
- Transferable/resumable sessions, session browser, network viewer.

See [`about.md`](about.md) for the in-game guide and
[`changelog.md`](changelog.md) for version history.

## Credits

Originally created by **Itzar**. NEAT rewrite, Climber/Hybrid modes and the
v3.0.0 sensing/softlock overhaul by **Mifu**.
