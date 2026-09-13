# Changelog

## v3.0.0
The softlock release. A hybrid run on Bloodbath sat at 30.74% for **18,431
attempts** without moving; this version fixes why, and gives the AI the senses
it was missing.

### Escaping a softlock (the headline fix)
- **Tape repair replaces the old rewind.** When the shared frontier stalls, the
  problem is almost never *at* the wall - it is in the locked clicks leading
  into it, which deliver the player there in an impossible state. The previous
  build handed the network control at a uniformly random point anywhere in the
  level, which asked it to improvise everything from there and threw away all
  the locked progress after that point; at 18,431 attempts stuck that was 60% of
  every attempt wasted. Now the mod **unlocks a bounded window of clicks before
  the wall and re-searches only those**, keeping the prefix intact, while the
  network still plays the unsolved ground past the frontier.
- **Window sizes are log-uniform over scales**, so small repairs (the clicks
  right before the wall, where the fix usually lives) keep ~38% of attempts
  forever, while deeper ones stay reachable as the stall drags on. Drawing the
  size directly would let the search drift to the largest window and waste
  itself re-discovering hundreds of clicks by chance.
- **Lateral moves.** A repaired tape that reaches the wall *by different clicks*
  is now accepted even though it gained no distance. This is what actually
  breaks a dead-end: the player arrives at the wall in a different state, so the
  section past it becomes reachable where it was not. Applies to Climber too.
- **Plain NEAT mode gets the same treatment**: the hand-over point moves further
  back the longer the population stagnates, instead of always re-playing only
  the last few hundred frames into the same doomed approach.

### The AI can finally see orbs
- The object cache held **solids and hazards only** - every orb, pad and portal
  in the game was invisible to every genome. On an orb-heavy level a network
  could not learn "click on this ring", only "click at this exact moment".
- Added **20 interaction senses**: nearest orb (with jump / gravity / dash
  type), nearest pad, nearest portal (gamemode / gravity / size / dual),
  current speed, mini state, dashing, input rhythm, and
  **"a ring is under you right now"** - the single most actionable bit in GD.
- Input count grows 44 -> 64. **Existing sessions are migrated, not rejected**:
  genomes are re-numbered onto the wider layer and the new senses are wired in
  at weight zero, so a loaded brain behaves *identically* while weight mutation
  can start using the new senses immediately. Verified byte-identical on a real
  200-genome session.

### Fitness
- The jump penalty counted **every** press in the attempt, including the ones
  merely replayed from the locked prefix. On a 401-click hybrid run that was a
  flat -8.0 fitness on every genome, biasing the search against exactly the
  click-heavy solutions an orb level needs. It now counts only the presses the
  network itself decided.

### Robustness
- A session whose saved RNG state cannot be parsed no longer discards the whole
  run. The textual form of `std::mt19937` is implementation-defined (MSVC writes
  624 state words, libstdc++ writes those plus the position), so a session saved
  on Windows could not be loaded on Android or macOS at all. The generator is
  reseeded and the evolved brains are kept - the RNG only drives future draws.
- The live HUD now shows the repair window and accepted lateral moves, so a
  stalled run is diagnosable instead of silent.


## v2.2.0
Adds a third learning mode and fixes the Climber getting stuck.

### Hybrid mode (network plays, working clicks locked)
- **New "Hybrid" mode**, selectable by tapping the mode button in the training
  settings (it now cycles **NEAT -> Climber -> Hybrid**). The neural network
  genuinely **plays** the level, but the whole population shares one **locked
  click prefix** up to the frontier: every genome replays that prefix, and the
  network only has to solve the **unsolved part past it**. When any network
  pushes further, its path becomes the new locked prefix for everyone, so the
  frontier advances and no effort is ever spent re-solving sections that already
  work.
- This is the best of both worlds: unlike the Climber it keeps real,
  **transferable brains** (a trained Hybrid population carries to other levels,
  resetting only the level-specific locked prefix), and unlike plain NEAT it
  never wastes a generation re-learning the opening of the level.
- Hybrid runs save, resume and play back like NEAT runs (the Sessions browser
  tags them `hyb` and allows transfer). The live HUD shows the shared frontier
  and stuck counter alongside the usual generation progress.

### Climber dead-end fix (escaping a stuck frontier)
- The Climber could get **permanently stuck** (e.g. parked at 10% for thousands
  of attempts) when the real problem was an **earlier locked input** that made
  the wall unreachable - the old look-back was capped, so it could never go back
  far enough to fix it. The Climber now does **escalating deep rewinds**: the
  longer the frontier stays stuck, the more often it **unlocks and re-searches
  earlier locked sections** - all the way back to the start if needed - then
  re-locks once it gets past, and repeats if a later section is blocked by the
  newly-locked clicks. Mutation also gets more aggressive the longer a spot
  stays stuck.
- The click generator now samples a **mixture of rhythm styles** per burst -
  rapid taps / hovering, sustained holds, sparse taps, and coasting (no input) -
  instead of one fixed cadence, so tight sections that need a specific or rare
  rhythm are actually reachable.
- The Hybrid mode reuses the same deep-rewind escalation: when its shared
  frontier stalls, the network is handed control further back so it can re-solve
  an earlier section the locked prefix was dooming.


Adds a second learning mode alongside NEAT.

### Climber mode (sequence search)
- **New "Climber mode"** toggle in the NEAT settings popup. Instead of evolving
  a population of neural networks, it grows a **single click sequence**: every
  part that clears an obstacle is **locked**, and only the failing tail is
  re-tried (timing jitter + insert/remove of presses + fresh exploration ahead)
  until it passes - then the frontier advances. The lock-back window widens the
  longer a spot stays stuck, so it can re-tune an earlier input when needed.
- Because Geometry Dash is deterministic, a working prefix replays perfectly, so
  this is often **much faster to beat one specific level** than full
  neuroevolution. In internal tests the climber cleared synthetic deterministic
  levels in a few dozen attempts per obstacle.
- Climber runs **save, resume and play back** through the same library as NEAT
  runs (best sequence stored as a playback; stopped runs are resumable from the
  same level). A climber run is **level-specific** and can't be transferred to a
  different level (the Sessions browser marks these and refuses transfer).
- In Climber mode, Population / Generations / Stagnation are ignored; the run
  continues until the level is beaten or the Minutes budget runs out. The live
  HUD and control panel show the climber's frontier, best %, stuck counter and
  attempts/sec.

### Notes
- NEAT mode is completely unchanged; existing sessions and playbacks stay
  compatible. Session files gained a small mode tag (older files load as NEAT).

## v2.0.0
A big feature update focused on **sessions that travel between levels** and far
more control over the AI.

### Sessions & transfer
- **Global session system.** Every training run can now be saved as a full
  session (population + brains + RNG + settings), and they're all tracked in a
  global index instead of being tied to one level file.
- **New Sessions browser** with two tabs: **This Level** and **Global (All)**.
  Open it from the NEAT settings popup.
- **Resume any session on any level.** On the *same* level it's a seamless full
  resume (frontier tapes kept). On a *different* level the evolved brains are
  **transferred** - the level-specific tapes are dropped and the population
  keeps learning from scratch on the new geometry. Great for carrying a strong
  population to a new level, or when a level's ID changed.
- **Manual checkpoints.** Save the live training state at any moment without
  stopping the run (from the control panel).
- **Finished runs are now transferable too**, not just stopped ones.
- Old single-level resume sessions are automatically adopted into the new
  browser, and all existing playbacks/sessions stay compatible.

### Live control panel
- Clicking the NEAT button while training now opens a **control panel** instead
  of instantly cancelling: adjust **Speed live**, toggle **Hide graphics live**,
  **save a checkpoint**, **view the brain**, or stop.

### AI / NEAT improvements
- **Full hyperparameter control** in a new Advanced panel: add-connection /
  add-node rates, weight mutation power, survival threshold, compatibility
  distance, champion threshold, jump penalty and RNG seed.
- **Adaptive speciation** - optionally target a fixed number of species and the
  compatibility threshold tunes itself.
- **Optional recurrent networks** - let evolution add memory/backward links
  (useful for ship and wave control).
- **tanh activation** option for hidden nodes.
- **Reproducible runs** via a fixed seed (0 = random as before).
- All defaults reproduce the classic NEATGD behaviour exactly.

### Quality of life
- **Network viewer** - see the evolved brain (senses, hidden nodes, jump output,
  green/red weighted connections).
- **Setting presets**: Fast, Balanced, Thorough, Overnight.
- Richer training HUD: species count, current brain size and attempts/second.

## v1.4.4
- Add MacOS intel support

## v1.4.3
- Resume playback button

## v1.4.2
- Editor levels now use unique ID using cvolton's editor ID API, so playbacks no longer collide between local levels
- Hide GFX optimisations

## v1.4.1
- Now runs on macOS, Android, and iOS in addition to Windows
- Fixed a non-portable timestamp call that broke building outside Windows
- Changed playback icon in menu from the refresh button to a folder icon

## v1.4.0
- Training no longer auto-plays its showcase, it pauses and asks first
- Runs are now saved either way, so overnight runs aren't wasted
- New playback library: save, replay, rename, and delete best runs per level

## v1.3.2
- New Max FPS setting to break the speed ceiling
- Speed now actually pays off at high frame rates, especially with Hide gfx on

## v1.3.1
- Less slowdown over long training runs
- Genomes now cap dead connections instead of bloating
- Innovation lookup is now a hash map instead of a linear scan
- Smaller settings info buttons
- Moved the Hide gfx info button so it no longer overlaps text

## v1.3.0
- Major bug fixes and learning optimisations
- Mutations only take control near where the parent died
- Passed sections can no longer be lost to a bad mutation
- Randomized takeover point for better failed-jump fixes

## v1.2.0
- Revamped the entire training system to rely less on approximations and more on using the actual GD game loop
- Training scores now reproduce exactly on the showcase run
- Removed the Batch setting (attempts are sequential now)
- Default Speed raised from 8x to 16x

## v1.1.1
- Your real player icon now plays as the current best genome during training
- Ghost players get distinct semi-transparent colors

## v1.1.0
- Ghosts now shrink and grow through mini portals
- Ghosts stay matched to the real player's speed and gamemode
- Fixed a fitness-sharing bug that stalled the AI at the first obstacle
- Added elitism so the best run is never lost
- AI can now learn to wait through "do nothing" sections
- Info buttons on every training setting
- Added an about page

## v1.0.0
- Initial release
