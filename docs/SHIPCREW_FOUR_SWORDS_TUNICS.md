# ShipCrew: Four Swords-inspired per-player tunic selection

Status: **approved feature specification; not yet implemented**. This document records the requested feature independently of multiplayer engine milestones.

## Player experience

- Each joined local player (up to four DualSense controllers) independently selects one of **Green, Red, Blue, Purple** tunics in a co-op player-selection screen.
- **Duplicates are permitted**: two or more players may deliberately choose the same color. Suggested first-use defaults for slots 1–4 are Green / Red / Blue / Purple, but they are not locked.
- Each player's selection appears on *that player's* Link in the world and in their viewport's local player-identification UI. Do not recolor every Link when one player changes their choice.
- Select tunic colors **before** co-op starts. Support changing colors through the co-op pause menu as a later incremental enhancement, without requiring a save reload.
- Choices are cosmetic only. Equipment still controls Goron fire protection, Zora underwater breathing and all normal inventory/equipment behavior. In the UI, show the underlying equipped tunic's effect separately if it differs from appearance.
- Keep the existing single-player tunic colors and Cosmetics Editor behavior unchanged whenever native local co-op is off. Do not convert ordinary single-player saves.
- Store four selection indices (0–3) in ShipCrew-local configuration, initially per **player slot** (not device GUID; identical DualSense hardware may share an identity), with defaults [Green, Red, Blue, Purple]. Confirm persistence across relaunch and reliable remapping after a controller disconnect/rejoin.
- Palette targets classic Four Swords-style saturated Green / Red / Blue / Purple. Treat exact RGB values as tuneable visual parameters; compare against the project renderer on both child and adult Link rather than claiming exact color matching prematurely.
- No requirement to make the four Links different character models or to alter tunic item icons immediately.

## Technical findings from the present 9.2.3 source

1. `soh/src/code/z_player_lib.c` defines `sTunicColors` and chooses an RGB tunic tint inside `Player_DrawImpl`. The existing Cosmetics Editor's `Link.KokiriTunic`, `Link.GoronTunic`, and `Link.ZoraTunic` values are **global** CVars; these alone cannot reliably represent four independent color choices.
2. `Player_DrawImpl` calls `GameInteractor_Should(VB_APPLY_TUNIC_COLOR, true, data, color)` just before it applies `gDPSetEnvColor`. This is a potential **actor-aware** integration point to investigate when local-player identity exists.
3. The gameplay drawing path in `soh/src/overlays/actors/ovl_player_actor/z_player.c` passes the specific `Player* this` as the final argument to `Player_DrawImpl`. Therefore gameplay draw calls can distinguish actors, **but check all call sites before dereferencing the generic `data` parameter**. For example, the pause preview in `z_player_lib.c` passes `&playerSwordAndShield`, not a `Player*`.
4. `soh/soh/Network/Anchor/DummyPlayer.cpp` tracks `AnchorClient.currentTunic` and calls `Player_Draw`, so its current remote representation should not require a global recoloring change for local players. The local co-op actor implementation should independently map each local `Player*` to a slot.
5. Existing tunic color code also handles goron/zora inventory models. Do **not** globally patch shared tunic display lists as a shortcut for individual local Link colors.

## Proposed implementation sequence

1. **After clean CI baseline succeeds**, add a small typed palette + per-slot selection data model. Unit-test color index validation (including bad/missing saved values) and safe defaults.
2. Integrate the data model into co-op's local-player registry once the second playable actor is stable: `Player* -> LocalPlayerSlot -> SelectedTunicColor`.
3. Apply each selection **only during the matching gameplay Link draw**, ideally through the existing tunic-color hook or an equivalently isolated actor-aware rendering path. Ensure the resulting tint does not spill into subsequent actors and respects render-state scoping.
4. Add controller-operable player-selection UI with 4 color buttons/swatches and a visible selection for every joined local player. Preserve original Cosmetics Editor behavior outside co-op.
5. Test two-player and then four-player rendering, including duplicate color combinations, child/adult Link, tunic equipment changes, cutscenes, pause preview, camera splits, disconnect/reconnect and relaunch.
6. If implementing an individual appearance in the pause preview, pass a **typed context or separate argument**; never pretend all uses of `Player_DrawImpl(data)` receive `Player*`.

## Acceptance checklist

- [ ] Each of four local players can choose any of four colors with their own controller.
- [ ] Same-color selections for multiple Links look correct simultaneously in all split-screen views.
- [ ] Equipped Goron/Zora tunic effects still work without changing a player's chosen appearance.
- [ ] Rejoining and relaunching restore the four saved choices or documented defaults.
- [ ] Switching colors never globally recolors the other three Links.
- [ ] Existing normal single-player visuals, cosmetics options, and game saves remain unaffected.

Do not treat this document as a claim that the feature works yet. Implementation begins after the untouched Windows build and second-player engine milestone.
