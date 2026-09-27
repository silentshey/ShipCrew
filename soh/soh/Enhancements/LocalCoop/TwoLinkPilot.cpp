// Opt-in, deliberately limited first integration test for a second local Link.
// Experimental single-process P2 movement + shared-inventory item integration.
// P2 remains a separate NPC-category Link actor, not the global GET_PLAYER.

#include <algorithm>
#include <cmath>

#include <libultraship/bridge/consolevariablebridge.h>
#include <spdlog/spdlog.h>

#include "soh/Enhancements/enhancementTypes.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "objects/gameplay_keep/gameplay_keep.h"
#include <overlays/actors/ovl_En_Arrow/z_en_arrow.h>

extern PlayState* gPlayState;
void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

// The experimental switch lives in the existing Controls settings screen.
// Leave disabled by default so the already validated single-player build is unchanged.
#define SHIPCREW_PILOT_CVAR CVAR_SETTING("ShipCrew.TwoLinkPilot")
#define SHIPCREW_SPLIT_CVAR CVAR_SETTING("ShipCrew.SplitScreenPilot")

namespace {

constexpr f32 kMaxStickValue = 80.0f;
constexpr f32 kRunSpeed = 4.2f;
constexpr f32 kWalkThreshold = 0.57f;
constexpr f32 kAcceleration = 0.6f;
constexpr f32 kDeceleration = 0.8f;
constexpr f32 kRollSpeed = 6.0f;
constexpr int kRollFrames = 14;
constexpr int kLandingFrames = 6;
constexpr int kItemFrames = 11;
constexpr int kItemDebounceFrames = 12;
constexpr int kMaxWorldBombs = 3;
constexpr f32 kItemDropDistance = 27.0f;
constexpr f32 kSpawnSeparation = 70.0f;
constexpr f32 kPilotGravity = -1.0f;
constexpr f32 kPilotTerminalVelocity = -18.0f;
// World collisions only; player/NPC combat and interactions are later milestones.
constexpr f32 kWallCheckHeight = 50.0f;
constexpr f32 kWallCheckRadius = 22.0f;
constexpr f32 kCeilingCheckHeight = 55.0f;
constexpr f32 kRadiansToN64Angle = 32768.0f / 3.14159265358979323846f;

bool sSpawningLocalPilot = false;
bool sSpawnAttempted = false;
// Render-pass flag used by z_view.c to keep independent interpolation history.
bool sRenderingSecondCamera = false;

// This runtime belongs to the one experimental P2 actor; never borrow P1's
// action state or animation tables. When expanded to 4 players, move this
// runtime into the common per-player component keyed by local player slot.
enum class PilotItemPose { None, BombPickup, BombThrow, Nut };
struct PilotRuntime {
    Actor* actor = nullptr;
    Actor* heldBomb = nullptr;
    Actor* lockedTarget = nullptr;
    // Derive rising edges from port 2's current buttons. Some controller
    // mappings can repeatedly report press bits while a button is held;
    // a button must become fully released before another item action.
    u32 previousButtons = 0;
    int itemDebounceFrames = 0;
    int rollFrames = 0;
    int landingFrames = 0;
    int itemFrames = 0;
    // Track shared ammo between P2 updates. A changed value outside a P2
    // item transaction comes from P1, a save edit, or an external sync.
    int lastObservedBombAmmo = -1;
    int lastObservedNutAmmo = -1;
    bool cameraReady = false;
    Vec3f cameraAt = {};
    Vec3f cameraEye = {};
    Vec3f cameraUp = { 0.0f, 1.0f, 0.0f };
    f32 cameraFov = 60.0f;
    PilotItemPose itemPose = PilotItemPose::None;
};
PilotRuntime sPilot;

// Check retained targets against the live enemy list before dereferencing.
bool Pilot_TargetIsLive(PlayState* play, Actor* target) {
    if (target == nullptr)
        return false;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
        if (a == target)
            return a->update != nullptr;
    }
    return false;
}

// Front-facing candidate acquisition on Z press. No P1 targeting globals.
Actor* Pilot_FindTarget(PlayState* play, Actor* pilot, Actor* exclude) {
    Actor* best = nullptr;
    f32 bestScore = 1.0e12f;
    const f32 facing = static_cast<f32>(pilot->shape.rot.y) / kRadiansToN64Angle;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_ENEMY].head; a != nullptr; a = a->next) {
        if (a == exclude || a->update == nullptr)
            continue;
        const f32 dx = a->world.pos.x - pilot->world.pos.x;
        const f32 dy = a->world.pos.y - pilot->world.pos.y;
        const f32 dz = a->world.pos.z - pilot->world.pos.z;
        const f32 distSq = dx * dx + dy * dy + dz * dz;
        const f32 horiz = std::sqrt(dx * dx + dz * dz);
        if (distSq < 1.0f || distSq > 422500.0f || horiz < 1.0f)
            continue;
        const f32 dot = (dx * std::sin(facing) + dz * std::cos(facing)) / horiz;
        if (dot < 0.1f)
            continue;
        const f32 score = distSq * (1.5f - dot);
        if (score < bestScore) {
            best = a;
            bestScore = score;
        }
    }
    return best;
}

LinkAnimationHeader* Pilot_Animation(const char* asset) {
    return reinterpret_cast<LinkAnimationHeader*>(const_cast<char*>(asset));
}

// P1 calls Inventory_ChangeAmmo(item, -1), which caps the *result* to the
// save's upgrade capacity. If a test/edited/shared save has ammunition but no
// matching Bomb Bag/Nut upgrade (capacity 0), even one normal decrement
// silently sets the entire stock to zero. Preserve the actual shared stock's
// exact one-item delta, without creating a separate P2 inventory.
//
// Snapshot BEFORE Actor_Spawn. If an actor-spawn hook already consumed the
// item, do not charge it a second time. Unlike forcing a fixed count after
// each frame, this only reconciles one successfully spawned item transaction.
void Pilot_ConsumeOneSharedAmmo(s16 item, s16 countBeforeSpawn) {
    const s16 expected = countBeforeSpawn - 1;
    const s16 afterSpawn = AMMO(item);
    const s16 capacity = item == ITEM_BOMB ? CUR_CAPACITY(UPG_BOMB_BAG) : CUR_CAPACITY(UPG_NUTS);

    if (afterSpawn == countBeforeSpawn) {
        // Reuse the exact P1 gameplay/statistics path for normal saves.
        Inventory_ChangeAmmo(item, -1);
    } else {
        SPDLOG_WARN("[ShipCrew] P2 item {}: spawn changed shared ammo from {} to {}; avoiding a second debit", item,
                    countBeforeSpawn, afterSpawn);
    }

    const s16 afterDebit = AMMO(item);
    if (afterDebit != expected) {
        SPDLOG_WARN("[ShipCrew] P2 item {}: correcting unexpected ammo change {} -> {} "
                    "(before={}, afterSpawn={}, upgradeCapacity={})",
                    item, afterDebit, expected, countBeforeSpawn, afterSpawn, capacity);
        // Restore ONLY this item's ammo slot. Do not copy/replace inventory or
        // interfere with Anchor's shared SaveContext synchronization.
        AMMO(item) = static_cast<s8>(expected);
    }
    SPDLOG_INFO("[ShipCrew] P2 item {} consumed: {} -> {} (upgradeCapacity={})", item, countBeforeSpawn, AMMO(item),
                capacity);
}

// Do not dereference a bomb pointer until the actual explosive list confirms
// the actor still exists. A held bomb can naturally explode, leave the room,
// or be removed while the pilot is disabled.
Actor* Pilot_FindHeldBomb(Actor* pilot, PlayState* play) {
    if (sPilot.heldBomb == nullptr) {
        return nullptr;
    }
    for (Actor* candidate = play->actorCtx.actorLists[ACTORCAT_EXPLOSIVE].head; candidate != nullptr;
         candidate = candidate->next) {
        if (candidate != sPilot.heldBomb) {
            continue;
        }
        if (candidate->update != nullptr && candidate->parent == pilot && candidate->params == 0) {
            return candidate;
        }
        if (candidate->parent == pilot) {
            candidate->parent = nullptr;
        }
        break;
    }
    if (pilot->child == sPilot.heldBomb) {
        pilot->child = nullptr;
    }
    sPilot.heldBomb = nullptr;
    return nullptr;
}

// First item press picks up exactly one real bomb, just like P1. The vanilla
// bomb actor retains its fuse/explosion/collision behavior while P2 positions
// the carried instance. The next independent A press places or throws it.
void Pilot_UseBomb(Actor* actor, PlayState* play) {
    if (sPilot.heldBomb != nullptr || AMMO(ITEM_BOMB) <= 0 ||
        (play->actorCtx.actorLists[ACTORCAT_EXPLOSIVE].length >= kMaxWorldBombs &&
         CVarGetInteger(CVAR_ENHANCEMENT("RemoveExplosiveLimit"), 0) == 0)) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    const s16 ammoBeforeSpawn = AMMO(ITEM_BOMB);
    const f32 radians = static_cast<f32>(actor->shape.rot.y) / kRadiansToN64Angle;
    Actor* bomb = Actor_SpawnAsChild(&play->actorCtx, actor, play, ACTOR_EN_BOM,
                                     actor->world.pos.x + std::sin(radians) * 10.0f, actor->world.pos.y + 48.0f,
                                     actor->world.pos.z + std::cos(radians) * 10.0f, 0, actor->shape.rot.y, 0, 0);
    if (bomb == nullptr) {
        return; // No actor, no ammo cost.
    }
    if (bomb->parent != actor) {
        // Another game hook prevented attachment. Avoid charging ammo or
        // accidentally placing an uncontrolled bomb in P2's hands.
        Actor_Kill(bomb);
        return;
    }
    bomb->room = -1;
    sPilot.heldBomb = bomb;
    Pilot_ConsumeOneSharedAmmo(ITEM_BOMB, ammoBeforeSpawn);
    sPilot.itemPose = PilotItemPose::BombPickup;
    sPilot.itemFrames = kItemFrames;
    sPilot.itemDebounceFrames = kItemDebounceFrames;
}

void Pilot_PositionHeldBomb(Actor* pilot, Actor* bomb) {
    const f32 facing = static_cast<f32>(pilot->shape.rot.y) / kRadiansToN64Angle;
    bomb->world.pos.x = pilot->world.pos.x + std::sin(facing) * 10.0f;
    bomb->world.pos.y = pilot->world.pos.y + 48.0f;
    bomb->world.pos.z = pilot->world.pos.z + std::cos(facing) * 10.0f;
    bomb->world.rot.y = pilot->shape.rot.y;
    bomb->shape.rot.y = pilot->shape.rot.y;
    bomb->speedXZ = 0.0f;
    bomb->velocity.y = 0.0f;
}

void Pilot_ReleaseBomb(Actor* pilot, Actor* bomb, bool throwForward) {
    const f32 facing = static_cast<f32>(pilot->shape.rot.y) / kRadiansToN64Angle;
    bomb->parent = nullptr;
    if (pilot->child == bomb) {
        pilot->child = nullptr;
    }
    sPilot.heldBomb = nullptr;
    const f32 offset = throwForward ? kItemDropDistance : 16.0f;
    bomb->world.pos.x = pilot->world.pos.x + std::sin(facing) * offset;
    bomb->world.pos.y = pilot->world.pos.y + (throwForward ? 31.0f : 9.0f);
    bomb->world.pos.z = pilot->world.pos.z + std::cos(facing) * offset;
    bomb->world.rot.y = pilot->shape.rot.y;
    bomb->speedXZ = throwForward ? 8.0f : 0.0f;
    bomb->velocity.y = throwForward ? 4.0f : 0.0f;
    sPilot.itemPose = PilotItemPose::BombThrow;
    sPilot.itemFrames = kItemFrames;
    sPilot.itemDebounceFrames = kItemDebounceFrames;
}

// Deku Nuts have no holding phase in OoT. Trigger once per genuine button
// down and only deduct the shared inventory if the nut actor actually spawns.
void Pilot_UseNut(Actor* actor, PlayState* play) {
    if (sPilot.heldBomb != nullptr || AMMO(ITEM_NUT) <= 0) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    const s16 ammoBeforeSpawn = AMMO(ITEM_NUT);
    const f32 radians = static_cast<f32>(actor->shape.rot.y) / kRadiansToN64Angle;
    Actor* nut =
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_ARROW, actor->world.pos.x + std::sin(radians) * kItemDropDistance,
                    actor->world.pos.y + 35.0f, actor->world.pos.z + std::cos(radians) * kItemDropDistance, 0x1000,
                    actor->shape.rot.y, 0, ARROW_NUT);
    if (nut != nullptr) {
        Pilot_ConsumeOneSharedAmmo(ITEM_NUT, ammoBeforeSpawn);
        sPilot.itemPose = PilotItemPose::Nut;
        sPilot.itemFrames = kItemFrames;
        sPilot.itemDebounceFrames = kItemDebounceFrames;
    }
}

void Pilot_Init(Actor* actor, PlayState* play) {
    auto* player = reinterpret_cast<Player*>(actor);
    Player* mainPlayer = GET_PLAYER(play);
    if (mainPlayer == nullptr || mainPlayer == player) {
        Actor_Kill(actor);
        return;
    }

    // Mirror the safe initialization pattern used by Anchor's DummyPlayer.
    // The pilot uses the same age/save equipment as P1; never change the global age.
    actor->room = -1;
    player->itemAction = player->heldItemAction = -1;
    player->heldItemId = ITEM_NONE;
    Player_UseItem(play, player, ITEM_NONE);
    Player_SetModelGroup(player, Player_ActionToModelGroup(player, player->heldItemAction));
    play->playerInit(player, play, gPlayerSkelHeaders[gSaveContext.linkAge]);

    // It is a visual/input probe, not yet a combat-capable player.
    Effect_Delete(play, player->meleeWeaponEffectIndex);
    player->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;
    play->func_11D54(player, play);
    actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    actor->colChkInfo.mass = MASS_IMMOVABLE;
    actor->gravity = kPilotGravity;
    actor->minVelocityY = kPilotTerminalVelocity;

    sPilot = {};
    sPilot.actor = actor;
    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
    NameTag_RegisterForActorWithOptions(actor, "P2 PILOT", {});
}

void Pilot_Update(Actor* actor, PlayState* play) {
    Player* player = reinterpret_cast<Player*>(actor);
    if (GET_PLAYER(play) == nullptr || GET_PLAYER(play) == player) {
        return;
    }
    if (sPilot.actor != actor) {
        sPilot = {};
        sPilot.actor = actor;
    }

    // Track inventory changes that happen outside P2's own one-use handler.
    // These can be expected P1 usage, Anchor synchronization or other hooks;
    // the diagnostic distinguishes them from an incorrect P2 debit.
    if (sPilot.lastObservedBombAmmo >= 0 && sPilot.lastObservedBombAmmo != AMMO(ITEM_BOMB)) {
        SPDLOG_INFO("[ShipCrew] Shared bomb ammo changed outside P2 update: {} -> {}", sPilot.lastObservedBombAmmo,
                    AMMO(ITEM_BOMB));
    }
    if (sPilot.lastObservedNutAmmo >= 0 && sPilot.lastObservedNutAmmo != AMMO(ITEM_NUT)) {
        SPDLOG_INFO("[ShipCrew] Shared nut ammo changed outside P2 update: {} -> {}", sPilot.lastObservedNutAmmo,
                    AMMO(ITEM_NUT));
    }

    // Player 2 reads only port 2. Never redirect GET_PLAYER or input[0]:
    // the save, scene, game clock and inventory remain intentionally shared.
    const auto& pad = play->state.input[1].cur;
    const u32 buttons = pad.button;
    const u32 pressed = buttons & ~sPilot.previousButtons;
    sPilot.previousButtons = buttons;
    if (sPilot.itemDebounceFrames > 0) {
        --sPilot.itemDebounceFrames;
    }
    const f32 x = static_cast<f32>(pad.stick_x) / kMaxStickValue;
    const f32 z = static_cast<f32>(pad.stick_y) / kMaxStickValue;
    const f32 inputLength = std::sqrt(x * x + z * z);
    const bool moving = inputLength > 0.17f;
    // Camera-relative movement must use the exact horizontal orientation of
    // P2's *rendered* camera, not its unsmoothed target yaw. Derive the basis
    // from the same cached eye/at passed to the second world render.
    //
    // Camera forward is eye -> at. Its screen-right vector is
    // (-forward.z, +forward.x), i.e. NOT (+forward.z, -forward.x).
    // The previous implementation had the lateral direction reversed.
    //
    // Preserve the validated world-space movement if split is disabled or
    // the P2 camera is still initializing.
    f32 worldX = x;
    f32 worldZ = z;
    if (CVarGetInteger(SHIPCREW_SPLIT_CVAR, 0) != 0 && sPilot.cameraReady) {
        const f32 forwardX = sPilot.cameraAt.x - sPilot.cameraEye.x;
        const f32 forwardZ = sPilot.cameraAt.z - sPilot.cameraEye.z;
        const f32 horizontalLength = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (horizontalLength > 0.001f) {
            worldX = (z * forwardX - x * forwardZ) / horizontalLength;
            worldZ = (z * forwardZ + x * forwardX) / horizontalLength;
        }
    }
    const bool wasGrounded = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    const bool canAct = !Player_InBlockingCsMode(play, GET_PLAYER(play));
    if (!Pilot_TargetIsLive(play, sPilot.lockedTarget))
        sPilot.lockedTarget = nullptr;
    if (canAct && (pressed & BTN_Z)) {
        // Second press cycles to another visible enemy or releases lock.
        sPilot.lockedTarget = Pilot_FindTarget(play, actor, sPilot.lockedTarget);
    }

    Actor* heldBomb = Pilot_FindHeldBomb(actor, play);
    // The original action button releases a carried bomb first. Otherwise A
    // rolls while running; there is no manual A-button jump.
    if (canAct && (pressed & BTN_A) && heldBomb != nullptr && sPilot.itemDebounceFrames == 0) {
        Pilot_ReleaseBomb(actor, heldBomb, moving);
        heldBomb = nullptr;
    } else if (canAct && (pressed & BTN_A) && heldBomb == nullptr && wasGrounded && moving && sPilot.rollFrames == 0 &&
               sPilot.itemFrames == 0) {
        sPilot.rollFrames = kRollFrames;
        sPilot.landingFrames = 0;
    }

    // Smooth analog acceleration allows walking, full-speed running and
    // deceleration; a roll commits to its original facing and short burst.
    const f32 requestedSpeed = canAct && moving ? kRunSpeed * std::min(inputLength, 1.0f) : 0.0f;
    if (sPilot.rollFrames > 0) {
        actor->speedXZ = kRollSpeed;
    } else {
        if (requestedSpeed > actor->speedXZ) {
            actor->speedXZ = std::min(requestedSpeed, actor->speedXZ + kAcceleration);
        } else {
            actor->speedXZ = std::max(requestedSpeed, actor->speedXZ - kDeceleration);
        }
        if (canAct && moving) {
            actor->world.rot.y = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
            if (sPilot.lockedTarget == nullptr)
                actor->shape.rot.y = actor->world.rot.y;
        }
    }

    if (sPilot.lockedTarget != nullptr && sPilot.rollFrames == 0) {
        const f32 dx = sPilot.lockedTarget->world.pos.x - actor->world.pos.x;
        const f32 dz = sPilot.lockedTarget->world.pos.z - actor->world.pos.z;
        if (dx * dx + dz * dz > 1.0f) {
            actor->shape.rot.y = static_cast<s16>(std::atan2(dx, dz) * kRadiansToN64Angle);
        }
    }
    const bool fallingBeforeMove = actor->velocity.y < -1.0f;
    Actor_MoveXZGravity(actor);
    Actor_UpdateBgCheckInfo(play, actor, kWallCheckHeight, kWallCheckRadius, kCeilingCheckHeight, 0x1D);
    Actor_SetFocus(actor, 40.0f);
    if (CVarGetInteger(SHIPCREW_SPLIT_CVAR, 0) == 0) {
        sPilot.cameraReady = false;
    }
    const bool grounded = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    if (!wasGrounded && grounded && fallingBeforeMove) {
        sPilot.landingFrames = kLandingFrames;
    }

    // Keep the live bomb attached to P2's position until A releases it.
    // Check the actor list before touching the pointer; the fuse still runs.
    heldBomb = Pilot_FindHeldBomb(actor, play);
    if (heldBomb != nullptr) {
        Pilot_PositionHeldBomb(actor, heldBomb);
    }

    // Genuine button-down edges plus a minimum cooldown prevent held-button
    // repeats from draining the entire shared save inventory.
    if (canAct && grounded && heldBomb == nullptr && sPilot.rollFrames == 0 && sPilot.itemFrames == 0 &&
        sPilot.itemDebounceFrames == 0) {
        if (pressed & BTN_CLEFT) {
            Pilot_UseBomb(actor, play);
        } else if (pressed & BTN_CRIGHT) {
            Pilot_UseNut(actor, play);
        }
    }

    // Keep P2's previously verified empty-handed stance. Airborne animations
    // play only when falling from terrain, not from an artificial A-button hop.
    // A separate skelAnime belongs to this actor.
    LinkAnimationHeader* animation = nullptr;
    u8 mode = ANIMMODE_LOOP;
    if (sPilot.rollFrames > 0 && grounded) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_landing_roll_free);
        mode = ANIMMODE_ONCE;
    } else if (!grounded) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_jump);
        sPilot.landingFrames = 0;
    } else if (sPilot.itemFrames > 0) {
        switch (sPilot.itemPose) {
            case PilotItemPose::BombPickup:
                animation = Pilot_Animation(gPlayerAnim_link_normal_free2bom);
                break;
            case PilotItemPose::BombThrow:
                animation = Pilot_Animation(gPlayerAnim_link_normal_throw_free);
                break;
            default:
                animation = Pilot_Animation(gPlayerAnim_link_normal_light_bom);
                break;
        }
        mode = ANIMMODE_ONCE;
    } else if (heldBomb != nullptr) {
        animation = Pilot_Animation(moving ? gPlayerAnim_link_normal_carryB : gPlayerAnim_link_normal_carryB_wait);
    } else if (sPilot.landingFrames > 0) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_short_landing_free);
        mode = ANIMMODE_ONCE;
    } else if (!moving) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_wait_free);
    } else if (inputLength <= kWalkThreshold) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_walk_free);
    } else {
        animation = Pilot_Animation(gPlayerAnim_link_normal_run_free);
    }

    if (player->skelAnime.animation != animation) {
        LinkAnimation_Change(play, &player->skelAnime, animation, 1.0f, 0.0f, Animation_GetLastFrame(animation), mode,
                             -3.0f);
    }
    LinkAnimation_Update(play, &player->skelAnime);

    if (sPilot.rollFrames > 0) {
        --sPilot.rollFrames;
    }
    if (sPilot.landingFrames > 0) {
        --sPilot.landingFrames;
    }
    if (sPilot.itemFrames > 0) {
        --sPilot.itemFrames;
    }
    player->upperLimbRot = { 0, 0, 0 };
    player->currentTunic = GET_PLAYER(play)->currentTunic;
    player->currentBoots = GET_PLAYER(play)->currentBoots;
    player->currentShield = GET_PLAYER(play)->currentShield;
    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
}

void Pilot_Draw(Actor* actor, PlayState* play) {
    Player_Draw(actor, play);
}

void Pilot_Destroy(Actor* actor, PlayState* play) {
    NameTag_RemoveAllForActor(actor);
    if (sPilot.actor == actor) {
        // A carried bomb must be freed before its P2 parent is destroyed.
        // Leave the live bomb in the shared world rather than leaking a
        // pointer into an actor that no longer exists.
        Actor* bomb = Pilot_FindHeldBomb(actor, play);
        if (bomb != nullptr) {
            Pilot_ReleaseBomb(actor, bomb, false);
        }
        sPilot = {};
    }
    // The actor originated as ACTOR_PLAYER, just as in Anchor's dummy path.
    // Restore the ID so ActorDB decrements the correct loaded actor count.
    actor->id = ACTOR_PLAYER;
}

Actor* FindPilotActor(PlayState* play) {
    for (Actor* actor = play->actorCtx.actorLists[ACTORCAT_NPC].head; actor != nullptr; actor = actor->next) {
        if (actor->update == Pilot_Update) {
            return actor;
        }
    }
    return nullptr;
}

bool CanRunPilot(PlayState* play) {
    return play != nullptr && GET_PLAYER(play) != nullptr && gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2 &&
           gSaveContext.gameMode == GAMEMODE_NORMAL;
}

void Pilot_RegisterHooks() {
    COND_HOOK(OnSceneSpawnActors, true, []() { sSpawnAttempted = false; });

    COND_ID_HOOK(ShouldActorInit, ACTOR_PLAYER, true, [](void* actorRef, bool*) {
        if (!sSpawningLocalPilot) {
            return;
        }
        auto* actor = static_cast<Actor*>(actorRef);
        Actor_ChangeCategory(gPlayState, &gPlayState->actorCtx, actor, ACTORCAT_NPC);
        actor->id = ACTOR_EN_OE2;
        actor->category = ACTORCAT_NPC;
        actor->init = Pilot_Init;
        actor->update = Pilot_Update;
        actor->draw = Pilot_Draw;
        actor->destroy = Pilot_Destroy;
    });

    COND_HOOK(OnGameFrameUpdate, true, []() {
        PlayState* play = gPlayState;
        if (!CanRunPilot(play)) {
            return;
        }

        Actor* pilot = FindPilotActor(play);
        // Existing Ivan fairy co-op also consumes controller port 2. Do not
        // compete for that port while our experimental pilot is enabled.
        if (CVarGetInteger(SHIPCREW_PILOT_CVAR, 0) == 0 ||
            CVarGetInteger(CVAR_ENHANCEMENT("IvanCoopModeEnabled"), 0) != 0) {
            if (pilot != nullptr) {
                Actor_Kill(pilot);
            }
            sSpawnAttempted = false;
            return;
        }

        if (pilot != nullptr || sSpawnAttempted) {
            return;
        }

        // A second ACTOR_PLAYER is safely diverted to the NPC category by the
        // ShouldActorInit hook, exactly like the existing Anchor representation.
        sSpawnAttempted = true;
        const Actor* mainActor = &GET_PLAYER(play)->actor;
        sSpawningLocalPilot = true;
        Actor_Spawn(&play->actorCtx, play, ACTOR_PLAYER, mainActor->world.pos.x + kSpawnSeparation,
                    mainActor->world.pos.y, mainActor->world.pos.z, 0, mainActor->shape.rot.y, 0, 0);
        sSpawningLocalPilot = false;
    });
}

static RegisterShipInitFunc sRegisterPilot(Pilot_RegisterHooks);

} // namespace

// P2's independently allocated native camera is updated once AFTER the
// engine's P1 camera, never during actor update or scene rendering.
extern "C" Actor* ShipCrewCamera_GetSecondTarget(PlayState* play) {
    if (play == nullptr || sPilot.actor == nullptr || FindPilotActor(play) != sPilot.actor ||
        !Pilot_TargetIsLive(play, sPilot.lockedTarget))
        return nullptr;
    return sPilot.lockedTarget;
}

extern "C" Player* ShipCrewCamera_GetNativeSecondPlayer(PlayState* play) {
    if (play == nullptr || CVarGetInteger(SHIPCREW_PILOT_CVAR, 0) == 0 || CVarGetInteger(SHIPCREW_SPLIT_CVAR, 0) == 0 ||
        CVarGetInteger(CVAR_ENHANCEMENT("IvanCoopModeEnabled"), 0) != 0 || play->activeCamera != CAM_ID_MAIN ||
        play->pauseCtx.state != 0 || play->pauseCtx.debugState != 0 || play->csCtx.state != CS_STATE_IDLE ||
        R_PAUSE_MENU_MODE != 0 || GET_PLAYER(play) == nullptr || play->roomCtx.curRoom.meshHeader == nullptr ||
        play->roomCtx.curRoom.meshHeader->base.type == 1) {
        return nullptr;
    }
    Actor* pilot = FindPilotActor(play);
    return pilot != nullptr && sPilot.actor == pilot ? reinterpret_cast<Player*>(pilot) : nullptr;
}

extern "C" void ShipCrewCamera_SetNativeSecondView(PlayState* play, const Vec3f* eye, const Vec3f* at, const Vec3f* up,
                                                   f32 fov) {
    if (play == nullptr || eye == nullptr || at == nullptr || up == nullptr || FindPilotActor(play) != sPilot.actor ||
        sPilot.actor == nullptr) {
        return;
    }
    sPilot.cameraEye = *eye;
    sPilot.cameraAt = *at;
    sPilot.cameraUp = *up;
    sPilot.cameraFov = fov;
    sPilot.cameraReady = true;
}

extern "C" s32 ShipCrewCamera_GetSecondView(PlayState* play, Vec3f* eye, Vec3f* at, Vec3f* up, f32* fov) {
    if (eye == nullptr || at == nullptr || up == nullptr || fov == nullptr ||
        ShipCrewCamera_GetNativeSecondPlayer(play) == nullptr || !sPilot.cameraReady) {
        return false;
    }
    *eye = sPilot.cameraEye;
    *at = sPilot.cameraAt;
    *up = sPilot.cameraUp;
    *fov = sPilot.cameraFov;
    return true;
}

extern "C" void ShipCrewCamera_SetSecondaryPass(s32 active) {
    sRenderingSecondCamera = active != 0;
}

extern "C" s32 ShipCrewCamera_IsSecondaryPass(void) {
    return sRenderingSecondCamera;
}
