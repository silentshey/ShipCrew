# ShipCrew: Four Swords-inspired per-player tunic selection

Status: **approved feature specification; not yet implemented**. This document records the requested feature independently of multiplayer engine milestones.

## Player experience

- Each joined local player (up to four DualSense controllers) independently selects one of **nine tunics: Green, Red, Blue, Purple, Yellow, Orange, White, Black, Pink** through an **in-game Co-op -> Change Tunic menu**. No front-end character-selection scene.
- **No duplicate colors across active players.** As soon as a player chooses a color, it becomes unavailable to the others; the selection UI shows it as taken. Suggested first-use defaults for slots 1–4 are Green / Red / Blue / Purple, but all four players can select any available color.
- Each player's selection appears on *that player's* Link in the world and in their viewport's local player-identification UI. Do not recolor every Link when one player changes their choice.
- Allow tunic selection **during gameplay, after a player joins**, with no restart or save reload. For the first implementation, use one shared, controller-navigable in-game co-op panel at a time instead of four simultaneous pause screens. Bind its selection to the controller/player slot that opened it. Handle a second menu-open request gracefully while the panel is occupied.
- Choices are cosmetic only. Equipment still controls Goron fire protection, Zora underwater breathing and all normal inventory/equipment behavior. In the UI, show the underlying equipped tunic's effect separately if it differs from appearance.
- Keep the existing single-player tunic colors and Cosmetics Editor behavior unchanged whenever native local co-op is off. Do not convert ordinary single-player saves.
- Store four selection indices (0–8) in ShipCrew-local configuration, initially per **player slot** (not device GUID; identical DualSense hardware may share an identity), with defaults [Green, Red, Blue, Purple]. Enforce uniqueness when loading saves and rejoining: on conflict, retain the first active slot's choice and offer the next player a new available color. Temporarily reserve a disconnected player's color during a short reconnect window; release it when that slot leaves the session. Confirm persistence across relaunch and reliable remapping after a controller disconnect/rejoin.
- Palette targets classic Four Swords-style saturated Green / Red / Blue / Purple, plus Yellow / Orange / White / Black / Pink. Treat exact RGB values as tuneable visual parameters; verify White, Yellow and Black remain distinguishable in bright/dark environments and compare against the project renderer on both child and adult Link rather than claiming exact color matching prematurely.
- Reuse the **existing Ocarina of Time 3D Link model, skeleton/animation, equipment and pause-menu preview renderer**. The in-game panel shows a single existing child/adult Link 3D model that previews colors live; selecting a color also updates that player's actual in-world Link when confirmed. Do not import new models, create an elaborate scene, or modify tunic item icons for this feature.

## Technical findings from the present 9.2.3 source

1. `soh/src/code/z_player_lib.c` defines `sTunicColors` and chooses an RGB tunic tint inside `Player_DrawImpl`. The existing Cosmetics Editor's `Link.KokiriTunic`, `Link.GoronTunic`, and `Link.ZoraTunic` values are **global** CVars; these alone cannot reliably represent four independent color choices.
2. `Player_DrawImpl` calls `GameInteractor_Should(VB_APPLY_TUNIC_COLOR, true, data, color)` just before it applies `gDPSetEnvColor`. This is a potential **actor-aware** integration point to investigate when local-player identity exists.
3. The gameplay drawing path in `soh/src/overlays/actors/ovl_player_actor/z_player.c` passes the specific `Player* this` as the final argument to `Player_DrawImpl`. Therefore gameplay draw calls can distinguish actors, **but check all call sites before dereferencing the generic `data` parameter**. The existing **pause-menu 3D Link preview** in `z_player_lib.c` already sets up viewport, perspective, camera and `Player_DrawImpl`, but it passes `&playerSwordAndShield`, not a `Player*`. Introduce a typed preview-context / explicit per-slot tint path instead of treating the existing generic pointer as a `Player*`.
4. `soh/soh/Network/Anchor/DummyPlayer.cpp` tracks `AnchorClient.currentTunic` and calls `Player_Draw`, so its current remote representation should not require a global recoloring change for local players. The local co-op actor implementation should independently map each local `Player*` to a slot.
5. Existing tunic color code also handles goron/zora inventory models. Do **not** globally patch shared tunic display lists as a shortcut for individual local Link colors.

## Proposed implementation sequence

1. **After clean CI baseline succeeds**, add a small typed palette + per-slot selection data model. Unit-test color index validation (including bad/missing saved values) and safe defaults.
2. Integrate the data model into co-op's local-player registry once the second playable actor is stable: `Player* -> LocalPlayerSlot -> SelectedTunicColor`.
3. Apply each selection **only during the matching gameplay Link draw**, ideally through the existing tunic-color hook or an equivalently isolated actor-aware rendering path. Ensure the resulting tint does not spill into subsequent actors and respects render-state scoping.
4. Add a compact **in-game Co-op -> Change Tunic** panel reusing the game's existing UI and 3D Link preview: nine simple color swatches / names, selected player's slot/controller indicator, existing 3D child/adult Link model, confirm/back controls. Do **not** create a new character-selection environment, four independent rendered preview scenes or a standalone pre-game menu. Disable colors taken by another active player and show a clear 'taken' indicator; re-enable them after a player changes selection or leaves. Preserve ordinary single-player pause-menu and Cosmetics Editor behavior.
5. Test two-player and then four-player rendering, including all nine palette choices, unique-color enforcement, taking/releasing colors, saved-selection conflicts, child/adult Link, tunic equipment changes, cutscenes, shared in-game panel ownership, 3D preview versus in-world appearance, camera splits, disconnect/reconnect and relaunch.
6. If implementing an individual appearance in the pause preview, pass a **typed context or separate argument**; never pretend all uses of `Player_DrawImpl(data)` receive `Player*`.

## Acceptance checklist

- [ ] Each of four local players can open the in-game co-op tunic panel and choose from nine colors with their own controller; a second controller cannot accidentally operate the same panel.
- [ ] Duplicate colors are prevented across active players, including after controller reconnects and loading stored selections.
- [ ] Equipped Goron/Zora tunic effects still work without changing a player's chosen appearance.
- [ ] Rejoining and relaunching restore the four saved choices or documented defaults.
- [ ] Previewing/changing one player's 3D Link tint never globally recolors the other three Links or other tunic display lists.
- [ ] Existing normal single-player visuals, cosmetics options, and game saves remain unaffected.

Do not treat this document as a claim that the feature works yet. Implementation begins after the untouched Windows build and second-player engine milestone.
