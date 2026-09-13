# NEATGD

**NEATGD** teaches itself to play Geometry Dash levels using
[NEAT](https://en.wikipedia.org/wiki/Neuroevolution_of_augmenting_topologies)
(NeuroEvolution of Augmenting Topologies); neural networks that evolve their
own structure and weights from scratch.

Every genome is judged by *actually playing* the level: each one takes over
your player for a fast-forwarded attempt, its network reading the terrain
ahead and deciding when to jump, the exact same engine a human plays
against, so what a genome scores in training is what it replays. Children
inherit their parent's proven inputs up to just before where the parent
died, then explore from that frontier, so progress is never lost to a bad
mutation. The furthest, cleanest runs breed the next generation until
something learns to beat the level.

## How to use

1. Open a level and hit **pause**.
2. Click the auto button in the top left corner of the pause menu.
3. Tune the training settings (each has an info button explaining it) and
   press **Train**. Or pick a **preset** (Fast / Balanced / Thorough /
   Overnight).
4. Watch the population evolve. When training finishes, the best genome plays
   the level back at normal speed.

While training is running, click the button again to open the **control
panel** (live speed, hide-graphics, checkpoints, brain viewer, stop).

## Three training modes

Tap the **mode button** in the training settings to cycle between them:

- **NEAT** - the default above: a population of neural-network brains that read
  the level and learn to react. Generalizes, and trained brains **transfer** to
  other levels.
- **Climber** - no brain. It grows a **single click sequence**, locking every
  part that clears an obstacle and re-trying only the failing tail until it
  passes. If it stalls it **deep-rewinds** to re-search earlier locked clicks.
  Often the fastest way to beat one specific level, but the sequence is
  level-specific (no transfer).
- **Hybrid** - the network **plays**, but the whole population shares one
  **locked click prefix** up to the frontier; the net only solves the unsolved
  part past it, and the prefix grows as the net pushes further. Keeps
  transferable brains like NEAT, while never wasting effort re-solving sections
  that already work.

## When a run gets stuck

If the frontier stops moving, the blocker is usually not the wall itself but the
locked clicks leading into it: the player keeps arriving there in a state from
which the next section is impossible. Climber and Hybrid handle this by
**repairing the tape** - unlocking a bounded window of clicks before the wall and
re-searching just those, keeping everything before intact. Repairs stay small
most of the time and reach further back the longer the stall lasts.

A repaired run that reaches the wall *by different clicks* is kept even when it
gains no distance. That is the actual escape: the arrival state changes, so the
section past the wall becomes reachable. The HUD shows `repair -Nf` and `lat N`
while this is happening.

## Sessions: carry your AI anywhere

Every run is saved as a **session** - the whole evolved population, not just a
replay. Open **Sessions** from the settings popup to browse them:

- **This Level** - sessions trained on the level you're in.
- **Global (All)** - every session, from every level.

Load any session onto the level you're currently in:

- **Same level** -> seamless **resume** (continues exactly where it left off,
  even if the level's ID changed).
- **Different level** -> **transfer**: the evolved brains carry over and keep
  learning on the new level from scratch. Perfect for giving a fresh level a
  head start with a population that already knows how to play.

You can also save a **checkpoint** mid-run from the control panel.

## Two learning modes

By default the AI uses **NEAT** (it evolves a population of neural networks).
A second mode, **Climber**, is available as a toggle in the training settings:

- **NEAT** learns a *general controller* (a brain that reacts to what it senses)
  and can be **transferred to other levels**.
- **Climber** grows a *single click sequence* for one level. Each chunk that
  clears an obstacle is **locked**, and only the failing part is re-searched
  until it passes, then the frontier advances. Since Geometry Dash is
  deterministic, a working prefix replays perfectly, so the climber is often the
  fastest way to **beat one specific level**. A climber run is level-specific
  and runs until the level is beaten (or the Minutes budget runs out).

Both modes save, resume and play back from the same library.

## Tweaking the AI

The **Advanced** panel exposes the full NEAT machinery: mutation rates, weight
power, survival threshold, compatibility distance, champion threshold, jump
penalty, adaptive speciation (target species count), **recurrent networks**,
**tanh** activation, and a fixed **seed** for reproducible runs. Every default
reproduces classic NEATGD, so you only change what you want.

The **brain viewer** draws the current best network: blue senses, yellow hidden
nodes, the pink jump output, and green/red connections weighted by strength.

## Tips

- NEAT improves through **generations**, so a moderate population (~150-500)
  with a high generation cap usually learns faster than a huge one.
- Turn on **Hide graphics** to spend your hardware on physics instead of
  rendering. It lets you push the **Speed** value higher.

---

Made by **Itzar**. v2.0 feature update (transferable sessions, network viewer,
full hyperparameter control) by **Mifu**.
