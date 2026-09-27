# ShipCrew: Phase 0 — reproducible Windows baseline

Status: **source baseline prepared; CI build and local playtest not yet verified.**

This is the starting checkpoint for **native four-player local Ocarina of Time co-op in a single process**. It deliberately makes **no gameplay changes** until a reproducible Windows build succeeds.

## Pinned baseline

- Repository: `silentshey/ShipCrew` (fork of `HarbourMasters/Shipwright`)
- Starting commit: `ca1e4c22505a7c2101ca816c31023daaf4e2638e` from `develop`
- CMake project version: `9.2.3` / Ackbar Delta
- Primary test platform: Windows x64
- Display target: **3440 x 1440** (21:9 ultrawide)
- Final local-play target: **four DualSense controllers; four playable Links in one game session**
- Final layout: **2 x 2**, nominal **1720 x 720** per player
- Compatibility priority: preserve original single-player behavior and saves.

Do **not** upload copyrighted game ROMs, extracted `oot.o2r` / `oot-mq.o2r`, or personal saves to GitHub or CI. Game extraction and normal launch happen privately on the player's computer.

## Build verification — already configured upstream

The inherited `.github/workflows/generate-builds.yml` defines:
1. Linux `generate-soh-otr`: generates free project resources (`soh.o2r`) from source.
2. Windows `build-windows`: downloads those project resources, compiles with CMake/Ninja/MSVC and publishes a `soh-windows` artifact.

No new compiler installation on the player's PC should be needed if GitHub Actions builds succeed. The artifact is **not** a packaged copy of copyrighted Ocarina of Time game assets.

### Baseline gate

- [ ] Enable Actions for this newly created fork if GitHub requests it.
- [ ] Observe the `generate-builds` workflow on the initial phase-0 branch / draft PR.
- [ ] Confirm `generate-soh-otr` succeeds.
- [ ] Confirm `build-windows` succeeds; investigate upstream/dependency issues before changing gameplay.
- [ ] Download `soh-windows` from **this fork's** workflow run.
- [ ] Extract to a **new folder**, not over the existing working Ackbar Delta installation.
- [ ] Run with legally obtained game assets generated locally according to the project's official instructions.
- [ ] Play-test single-player: new test save, basic movement, entering a second area, camera, pause/inventory, quitting/reopening.
- [ ] Record any baseline crashes, visual defects, audio glitches or controller-mapping issues.

**Actions page:** https://github.com/silentshey/ShipCrew/actions

## Source audit: what Anchor gives us (and what it doesn't)

The integrated code currently lives at:
- `soh/soh/Network/Anchor/DummyPlayer.cpp`
- `soh/soh/Network/Anchor/Anchor.h`
- `soh/soh/Network/Anchor/HookHandlers.cpp`

Anchor can initialize and draw another Link-like actor and has network representations for other players' positions, animations and equipment. The dummy's update copies **remote client data** into an actor; it is **not** a second locally controlled player. Anchor also temporarily changes `gSaveContext.linkAge` during dummy initialization, so save/global-state assumptions will require special care.

Other starting points:
- `soh/src/overlays/actors/ovl_player_actor/z_player.c` — primary player logic
- `soh/src/code/z_camera.c` — native camera behavior
- `soh/soh/Enhancements/controls/` and the `libultraship` submodule — inspect controller input before implementation
- `soh/soh/Network/Anchor/HookHandlers.cpp` — inspect actor-spawn interception before reusing Anchor internals.

**Important:** Networked dummy animation is not sufficient for true co-op. The first engineering task is to identify and isolate single-player assumptions in input, player state, actor interactions, camera state, global save data, and rendering.

## Phase 1 after the baseline passes

1. **Compile-guarded opt-in:** add a local co-op feature flag defaulting **off**, and verify unchanged single-player mode.
2. **Second local actor:** spawn a second Link in an isolated test scene without requiring Anchor networking; establish stable initialization/teardown and collisions before allowing items or scene transitions.
3. **Independent input:** create per-player input state and bind two separate controllers to independently movable Links.
4. **Camera proof:** render two independent game cameras with isolated per-viewport UI. Verify single-player view remains unaffected.
5. **Gameplay proof:** test attacking, taking damage, doors, inventory, cutscenes, respawns, scene transitions, saves.
6. **Expand to four:** only after the two-player system survives repeated level changes, boss fights, and restart testing. Target 2 x 2 at 3440 x 1440.

Shared-world gameplay policy for early prototypes: same scene, one campaign save/shared progression, individual health, player 1 owns menu-driven scene transitions and cutscenes. These are provisional engineering constraints, not final design commitments.

## Definition of "ready for a playable four-player build"

A successful **CI compile alone is not a gameplay stability claim**. Require four individually mapped controllers, independent functional cameras, no regressions in one-player mode, consistent actor interactions, safe disconnection/scene-transition behavior and repeated real Windows playtests before calling it stable.
