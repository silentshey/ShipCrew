# ShipCrew OMNIBUS: native Player 2 systems
Experimental branch: `shipcrew/omnibus-native-p2-experiment` (based on PR #33).

## One-switch setup
Open **Configure Controller > Link (P1)** and enable **ShipCrew: OMNIBUS Native P2 Systems (DANGEROUS TEST)**.
Map a second gamepad under **Link (P2)**. The omnibus switch activates P2 and split-screen/native
movement without changing the saved values of older pilot switches. Existing pilot options remain
available if omnibus is disabled.

**Back up your save files first.** Never test this on the only copy of a save.

## What this experiment wires together
- P2 runs the original `Player_UpdateCommon` / action function engine with controller port 2.
- P2's independently rendered camera supplies its own camera-relative input yaw and targeting candidate.
- Native B/C item selection, equip changes and original item actions are enabled for P2; the previous
  hard-coded bomb/nut simulation is bypassed on this path.
- Real P2 sword blur and original sword/shield collider objects are retained from native initialization.
- P2's original native body collider handles OC and AC without a duplicate registration on the update exit.
- P2 gets a nearby, facing-filtered NPC talk candidate and the native talk initialization targets P2
  while its update is active (one global MessageContext remains).
- P2's C HUD icons mirror the shared equipped C slots rather than falsely showing bomb/nut defaults.
- Shared floor scene exits, native crawl/ledge/ladder handling and prop offers are inherited from PR #33.
- Cleanup releases P2's original colliders and sword effect without calling Player_Destroy, which would
  reset global save/magic state.

## Test matrix (record PASS, FAIL, CRASH and replay steps for each)
1. **Smoke:** launch a throwaway new save, toggle omnibus, spawn P2, move/rotate both cameras.
2. **Locomotion:** idle/start/stop, walking, running, rolling, backflips/side-hops, slopes, autojump.
3. **Traversal:** crawl the Kokiri hole forward/back and stand; climb ladders top/bottom; mantle fences/ledges.
4. **Combat:** equipped Kokiri/Master/Biggoron sword B attacks, Z-target strafing, shield R, blocks,
   collisions against enemies, spin attack, sword trail, damaged enemy reactions.
5. **Items:** put bomb, nut, slingshot/bow, boomerang, hookshot, stick, bottles, and ocarina on native C
   slots. Verify one shared ammo debit, projectile owner/origin and changing/putting away items.
6. **Inventory:** open pause menu; switch C equipment; verify it updates P2's displayed icons and input.
7. **Props:** P2-only grass/rock/pot pickup/carry/throw without P1 nearby; check duplicate pickup.
8. **Dialogue:** P2 A to talk with a nearby NPC while P1 is far away; test text advance and exit.
9. **World:** P2 floor exit, door/chest interactions, indoor NPCs, prerendered rooms, room unload.
10. **Regression:** P1 never loses input; disable omnibus mid-scene; return to P1 solo; respawn P2.
11. **Stress:** two players use items simultaneously, sword swings and enemy collision in one scene.

## KNOWN DESIGN LIMITS, NOT CLAIMED COMPLETE
- **Shared inventory, health, magic, equipped items and save flags.** No per-player inventories.
- **One global PlayState and MessageContext**: P2 NPC dialogue is a high-risk prototype and
  cutscenes, shops and quest rewards may control P1, softlock or overwrite shared state.
- **Door/chest/get-item scripting, projectile ownership and NPC AI targeting** are not isolated for P2.
- Single-player pause/cutscene fallbacks and split HUD are inherited; full separate menus aren't implemented.
- 3/4-player slots, unique inventory, player-specific quest progress and four cameras aren't implemented.
- The experimental NPC scanner only considers attention-enabled NPCs with a nonzero text ID.
- **Build success != gameplay success.** Do not merge until manual P1/P2 regressions pass.

## Rollback
Uncheck the omnibus option. Existing pilot controls remain unchanged. A saved game affected by
quest/cutscene global changes may need to be restored from backup.
