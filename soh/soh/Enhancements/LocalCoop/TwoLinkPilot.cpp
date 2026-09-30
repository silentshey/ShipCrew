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
void Player_DetachHeldActor(PlayState* play, Player* player);
void Player_SetInvulnerability(Player* player, s32 timer);
LinkAnimationHeader* ShipCrewPlayer_GetGroupAnimation(Player* player, s32 group);
f32 ShipCrewPlayer_GetRunSpeedLimit(void);
void ShipCrewPlayer_ApplyNativeRunMotion(Player* player, f32 speedTarget, s16 yawTarget);
f32 ShipCrewPlayer_CalcGroundSpeedTarget(f32 magnitude, f32 speedCap, s16 floorPitch, s32 curved);
f32 ShipCrewPlayer_CalcNativeAnalogSpeed(Player* player, f32 magnitude, s32 curved);
void ShipCrewPlayer_ApplyNativeAirMotion(Player* player, f32 speedTarget, s16 yawTarget);
void ShipCrewPlayer_ResetNativeGravity(Player* player);
s32 ShipCrewPlayer_ShouldEnterFallAnimation(Player* player, s32 zeroStage, f32 fallDistance);
s32 ShipCrewPlayer_ApplyNativeIdleBrake(Player* player);
LinkAnimationHeader* ShipCrewPlayer_SelectNativeAutoJump(Player* player, f32* verticalSpeed);
s32 ShipCrewPlayer_ShouldNativeAutoJump(Player* player, s32 prevFloorProperty, f32 yDistToFloor, s16 yawDelta);
void ShipCrewPlayer_StartNativeRollClip(PlayState* play, Player* player, f32 waterSpeedFactor);
LinkAnimationHeader* ShipCrewPlayer_SelectNativeDodge(s32 direction, s32 landing);
f32 ShipCrewPlayer_NativeDodgeVerticalSpeed(s32 direction);
f32 ShipCrewPlayer_NativeDodgeHorizontalSpeed(s32 direction);
void ShipCrewPlayer_QueueNativeAnimMovement(PlayState* play, Player* player);
void ShipCrewPlayer_BeginNativeClimb(PlayState* play, Player* player, const Vec3f* entry, s16 entryYaw, s32 fromTop,
                                     s32 freeClimb);
s32 ShipCrewPlayer_IsNativeClimbAction(Player* player);
s32 ShipCrewPlayer_UpdateNativeClimbForPilot(PlayState* play, Player* player, Input* input);
void ShipCrewPlayer_CancelNativeClimbForPilot(PlayState* play, Player* player);
s32 ShipCrewPlayer_QueryLedge(PlayState* play, Player* player, f32* rise, Vec3f* stand, s16* facing);
s32 ShipCrewPlayer_QueryLadder(PlayState* play, Player* player, s32 fromTop, Vec3f* anchor, s16* facing, f32* bottomY,
                               f32* topY, s16 approachYaw);
s32 ShipCrewPlayer_QueryCrawlspace(PlayState* play, Player* player, Vec3f* center);
s32 ShipCrewPlayer_QueryNativeVine(PlayState* play, Player* player, s16 approachYaw, Vec3f* anchor, s16* facing,
                                   f32* bottomY, f32* topY);
s32 ShipCrewPlayer_CanLiftContextActor(Actor* actor);
s32 Player_CanThrowCarriedActor(Player* player, Actor* actor);
void ShipCrewPlayer_BeginNativeCrawl(PlayState* play, Player* player, const Vec3f* center);
s32 ShipCrewPlayer_IsNativeCrawlAction(Player* player);
s32 ShipCrewPlayer_UpdateNativeCrawlForPilot(PlayState* play, Player* player, Input* input);
s32 ShipCrewPlayer_TryNativeLedgeForPilot(PlayState* play, Player* player, Input* input);
s32 ShipCrewPlayer_IsNativeLedgeAction(Player* player);
s32 ShipCrewPlayer_UpdateNativeLedgeForPilot(PlayState* play, Player* player, Input* input);
s32 ShipCrewPlayer_HandlePilotSceneExit(PlayState* play, Player* player);
s32 ShipCrewPlayer_UpdateNativePilotCore(PlayState* play, Player* player, Input* input);
}

// The experimental switch lives in the existing Controls settings screen.
// Leave disabled by default so the already validated single-player build is unchanged.
#define SHIPCREW_PILOT_CVAR CVAR_SETTING("ShipCrew.TwoLinkPilot")
#define SHIPCREW_SPLIT_CVAR CVAR_SETTING("ShipCrew.SplitScreenPilot")
#define SHIPCREW_NATIVE_LOCOMOTION_CVAR CVAR_SETTING("ShipCrew.P2NativeLocomotion")
#define SHIPCREW_NATIVE_TRAVERSAL_CVAR CVAR_SETTING("ShipCrew.P2NativeTraversal")

namespace {

constexpr f32 kMaxStickValue = 80.0f;
constexpr f32 kRunSpeed = 4.2f;
constexpr f32 kWalkThreshold = 0.57f;
constexpr f32 kAcceleration = 0.6f;
constexpr f32 kDeceleration = 0.8f;
constexpr f32 kRollSpeed = 6.0f;
constexpr int kRollFrames = 14;
constexpr int kNativeRollFrames = 20;
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
enum class PilotTraversal { None, AutoJump, HighStepWindup, Hanging, Climbing };
enum class PilotLadder { None, Active };
enum class PilotLockMove { None, Forward, Back, Left, Right };
enum class PilotDodge { None, SideLeft, Backflip, SideRight };
enum class PilotCrawl { None, Enter, Move, Exit };
struct PilotRuntime {
    Actor* actor = nullptr;
    Actor* heldBomb = nullptr;
    Actor* lockedTarget = nullptr;
    Actor* pickupCandidate = nullptr;
    Actor* carriedProp = nullptr;
    bool pickupAttached = false;
    int contextCooldown = 0;
    PilotCrawl crawl = PilotCrawl::None;
    s16 crawlYaw = 0;
    f32 crawlDistance = 0.0f;
    PilotLockMove lockMove = PilotLockMove::None;
    bool parallelTargeting = false;
    int parallelRecenterFrames = 0;
    s16 parallelFacing = 0; // Independent P2 no-enemy Z facing, never P1's global parallelYaw.
    // Derive rising edges from port 2's current buttons. Some controller
    // mappings can repeatedly report press bits while a button is held;
    // a button must become fully released before another item action.
    u32 previousButtons = 0;
    int itemDebounceFrames = 0;
    int rollFrames = 0;
    bool nativeRoll = false;
    int rollRecoveryFrames = 0;
    PilotDodge dodge = PilotDodge::None;
    bool dodgeLanding = false;
    LinkAnimationHeader* nativeAutoJumpAnim = nullptr;
    s16 dodgeYaw = 0;
    bool rollInvulnStarted = false;
    f32 rollSpeed = 0.0f;
    s16 rollYaw = 0;
    bool nativeMovementPreviouslyEnabled = false;
    bool longLanding = false;
    int landingFrames = 0;
    int ledgeProbeFrames = 0;
    int ledgeProbeType = 0;
    int ledgeCooldownFrames = 0;
    f32 takeoffY = 0.0f;
    f32 ledgeRise = 0.0f;
    Vec3f ledgeStand = {};
    Vec3f hangAnchor = {};
    Vec3f climbStart = {};
    s16 ledgeFacing = 0;
    PilotTraversal traversal = PilotTraversal::None;
    PilotLadder ladder = PilotLadder::None;

    int ladderCooldown = 0;
    int ladderExitGraceFrames = 0;
    Vec3f ladderExitPosition = {};
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

void Pilot_ClearTraversal(Actor* actor, Player* player) {
    sPilot.traversal = PilotTraversal::None;
    sPilot.ledgeProbeFrames = 0;
    player->stateFlags1 &= ~(PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_JUMPING);
    if (CVarGetInteger(SHIPCREW_NATIVE_LOCOMOTION_CVAR, 0) != 0)
        ShipCrewPlayer_ResetNativeGravity(player);
    else
        actor->gravity = kPilotGravity;
    actor->shape.yOffset = 0.0f;
}

void Pilot_BeginClimb(Player* player, PlayState* play, s32 type, bool fromHang) {
    Actor* actor = &player->actor;
    if (player->ageProperties == nullptr)
        return;

    // P1 climbs with animation movement. Starting P2 at the FINAL ledge
    // position was instantly teleporting the actor and its native camera,
    // while the visual model was held down with a huge shape.yOffset.
    // Keep both at the real starting anchor and advance through the animation.
    sPilot.climbStart = actor->world.pos;
    SPDLOG_INFO("[ShipCrew] P2 climb: fromHang={} startY={} targetY={}", fromHang, sPilot.climbStart.y,
                sPilot.ledgeStand.y);
    actor->prevPos = actor->world.pos;
    actor->velocity.y = 0.0f;
    actor->speedXZ = player->linearVelocity = 0.0f;
    actor->gravity = 0.0f;
    actor->world.rot.y = actor->shape.rot.y = player->yaw = sPilot.ledgeFacing;
    actor->bgCheckFlags |= BGCHECKFLAG_GROUND;
    player->stateFlags1 &= ~PLAYER_STATE1_HANGING_OFF_LEDGE;
    player->stateFlags1 |= PLAYER_STATE1_CLIMBING_LEDGE | PLAYER_STATE1_JUMPING;

    actor->shape.yOffset = 0.0f;
    LinkAnimationHeader* animation;
    if (fromHang)
        animation = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_jump_climb_up);
    else if (type == PLAYER_LEDGE_CLIMB_3)
        animation = Pilot_Animation(gPlayerAnim_link_normal_150step_up);
    else
        animation = Pilot_Animation(gPlayerAnim_link_normal_100step_up);

    sPilot.traversal = PilotTraversal::Climbing;
    sPilot.ledgeCooldownFrames = 18;
    LinkAnimation_Change(play, &player->skelAnime, animation, 1.3f, 0.0f, Animation_GetLastFrame(animation),
                         ANIMMODE_ONCE, -3.0f);
}

void Pilot_BeginJump(Player* player, f32 verticalSpeed) {
    Actor* actor = &player->actor;
    // P1 initializes vertical motion with its runtime gravity register and
    // a -20 terminal velocity; hostile Z falls use P1's -1.2 override.
    if (CVarGetInteger(SHIPCREW_NATIVE_LOCOMOTION_CVAR, 0) != 0) {
        ShipCrewPlayer_ResetNativeGravity(player);
        if (sPilot.lockedTarget != nullptr)
            actor->gravity = -1.2f;
    } else {
        actor->gravity = kPilotGravity;
    }
    actor->velocity.y = verticalSpeed;
    actor->bgCheckFlags &= ~BGCHECKFLAG_GROUND;
    player->stateFlags1 |= PLAYER_STATE1_JUMPING;
    player->stateFlags1 &= ~(PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE);
    sPilot.traversal = PilotTraversal::AutoJump;
    sPilot.nativeAutoJumpAnim = nullptr;
    sPilot.takeoffY = actor->world.pos.y;
    sPilot.landingFrames = 0;
    sPilot.ledgeCooldownFrames = 12;
}

// Native assets match P1's D_80853D4C directional hop table.
LinkAnimationHeader* Pilot_DodgeAnim(PilotDodge dodge, bool landing) {
    // Exact P1 fighter dodge assets, including left/right/back landing poses.
    return ShipCrewPlayer_SelectNativeDodge(static_cast<s32>(dodge), landing);
}
void Pilot_BeginHang(Player* player, PlayState* play, f32 rise, const Vec3f& stand, s16 facing) {
    Actor* actor = &player->actor;
    sPilot.ledgeStand = stand;
    sPilot.ledgeFacing = facing;
    sPilot.ledgeRise = rise;
    sPilot.traversal = PilotTraversal::Hanging;
    sPilot.ledgeCooldownFrames = 12;
    // 'stand' is the TOP of the wall, not the hanging location. Preserve
    // physical hanging height instead of snapping P2/camera upwards.
    actor->world.pos.x = stand.x;
    actor->world.pos.z = stand.z;
    sPilot.hangAnchor = actor->world.pos;
    actor->prevPos = actor->world.pos;
    actor->shape.rot.y = actor->world.rot.y = player->yaw = facing;
    actor->velocity.y = actor->speedXZ = player->linearVelocity = 0.0f;
    actor->gravity = 0.0f;
    player->stateFlags1 &= ~PLAYER_STATE1_JUMPING;
    player->stateFlags1 |= PLAYER_STATE1_HANGING_OFF_LEDGE;
    LinkAnimationHeader* anim = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_jump_climb_hold);
    LinkAnimation_Change(play, &player->skelAnime, anim, 1.0f, 0.0f, Animation_GetLastFrame(anim), ANIMMODE_LOOP,
                         -3.0f);
}

// Traversal owns P2 input/animation while hanging, stepping up or winding up
// a tall-ledge jump. It never updates P1 or the global Player action function.
bool Pilot_UpdateTraversal(Player* player, PlayState* play, const OSContPad& pad, u32 pressed, bool canAct) {
    Actor* actor = &player->actor;
    if (sPilot.traversal != PilotTraversal::Hanging && sPilot.traversal != PilotTraversal::Climbing &&
        sPilot.traversal != PilotTraversal::HighStepWindup) {
        return false;
    }

    if (!canAct) {
        Pilot_ClearTraversal(actor, player);
        return false;
    }

    actor->speedXZ = player->linearVelocity = 0.0f;
    actor->velocity.y = 0.0f;
    actor->gravity = 0.0f;
    if (sPilot.traversal == PilotTraversal::Hanging) {
        actor->world.pos = sPilot.hangAnchor;
        actor->prevPos = actor->world.pos;
        if (pad.stick_y > 25 || (pressed & BTN_A)) {
            Pilot_BeginClimb(player, play, PLAYER_LEDGE_CLIMB_3, true);
        } else if (pad.stick_y < -25) {
            // Drop outward from the wall rather than falling inside it.
            const f32 direction = static_cast<f32>(sPilot.ledgeFacing) / kRadiansToN64Angle;
            actor->world.pos.x -= std::sin(direction) * 12.0f;
            actor->world.pos.z -= std::cos(direction) * 12.0f;
            actor->world.pos.y -= 7.0f;
            actor->bgCheckFlags &= ~BGCHECKFLAG_GROUND;
            Pilot_ClearTraversal(actor, player);
            sPilot.ledgeCooldownFrames = 20;
            return false;
        } else {
            LinkAnimation_Update(play, &player->skelAnime);
        }
    }

    if (sPilot.traversal == PilotTraversal::HighStepWindup) {
        const bool finished = LinkAnimation_Update(play, &player->skelAnime) != 0;
        if (player->skelAnime.curFrame >= 8.0f || finished) {
            const f32 height = std::min(sPilot.ledgeRise, player->ageProperties->unk_0C);
            const f32 speed = height * 0.072f + (gSaveContext.linkAge != LINK_AGE_ADULT ? 1.0f : 0.0f);
            Pilot_BeginJump(player, speed);
            actor->speedXZ = player->linearVelocity = 1.0f;
            actor->world.rot.y = player->yaw = sPilot.ledgeFacing;
            return false;
        }
    } else if (sPilot.traversal == PilotTraversal::Climbing) {
        const bool finished = LinkAnimation_Update(play, &player->skelAnime) != 0;
        const f32 end = std::max(1.0f, player->skelAnime.endFrame);
        const f32 t = finished ? 1.0f : std::clamp(player->skelAnime.curFrame / end, 0.0f, 1.0f);
        // Advance the actual actor/camera over the climbing animation rather
        // than placing the actor at the destination before the climb begins.
        // A smooth start/stop avoids a second spike at the transition.
        const f32 progress = t * t * (3.0f - 2.0f * t);
        actor->prevPos = actor->world.pos;
        actor->world.pos.x = sPilot.climbStart.x + (sPilot.ledgeStand.x - sPilot.climbStart.x) * progress;
        actor->world.pos.y = sPilot.climbStart.y + (sPilot.ledgeStand.y - sPilot.climbStart.y) * progress;
        actor->world.pos.z = sPilot.climbStart.z + (sPilot.ledgeStand.z - sPilot.climbStart.z) * progress;
        // The skeleton has baked-in root translation. During isolated P2
        // movement the world actor, not both world + root, owns displacement.
        player->skelAnime.jointTable[0] = player->skelAnime.baseTransl;
        if (finished) {
            actor->world.pos = sPilot.ledgeStand;
            Pilot_ClearTraversal(actor, player);
            sPilot.landingFrames = kLandingFrames;
            sPilot.ledgeCooldownFrames = 20;
            // Don't integrate a SECOND gravity/movement step on the frame
            // the climb completes: normal physics resumes next update.
            Actor_SetFocus(actor, 40.0f);
            return true;
        }
    }

    // Skip ordinary physics, roll and item actions until the native animation
    // stage finishes; keep the current actor focus updated for both cameras.
    Actor_SetFocus(actor, 40.0f);
    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
    return true;
}

// The ladder action is independently owned by P2. Original P1 ladder
// animation assets and input-dependent playback rates are reused, but its
// file-static control state and GET_PLAYER() action machine are not.
void Pilot_ClearLadder(Player* player) {
    sPilot.ladder = PilotLadder::None;
    player->skelAnime.movementFlags = 0;
    sPilot.ladderCooldown = 16;
    player->stateFlags1 &= ~PLAYER_STATE1_CLIMBING_LADDER;
    player->stateFlags2 &= ~PLAYER_STATE2_STATIONARY_LADDER;
    if (CVarGetInteger(SHIPCREW_NATIVE_LOCOMOTION_CVAR, 0) != 0)
        ShipCrewPlayer_ResetNativeGravity(player);
    else
        player->actor.gravity = kPilotGravity;
    player->actor.shape.yOffset = 0.0f;
}

// P2 now enters the exact native Player_Action_8084BF1C used by P1,
// rather than driving its own entry/rung/boundary/dismount animation machine.
// Only the initial P2 camera-relative collision probe remains pilot-specific:
// P1's detection relies on globals produced by Player_ProcessSceneCollision.
void Pilot_BeginLadder(Player* player, PlayState* play, bool fromTop, const Vec3f& anchor, s16 yaw, f32 bottomY,
                       f32 topY, bool freeClimb = false) {
    Actor* actor = &player->actor;
    sPilot.ladder = PilotLadder::Active;
    SPDLOG_INFO("[ShipCrew] P2 native P1 climb entry: top={} vine={} bottomY={} topY={}", fromTop, freeClimb, bottomY,
                topY);

    // P1's native top/bottom entry clips have their own root translation.
    // Do not interpolate the actor independently or pre-warp its camera.
    Vec3f entry = anchor;
    if (fromTop)
        entry.y = actor->world.pos.y;
    ShipCrewPlayer_BeginNativeClimb(play, player, &entry, yaw, fromTop, freeClimb);
}

// Native action dispatch is responsible for rung progression, climbable-wall
// sideways motion, top/bottom floor decisions and animation-driven dismount.
// Preserve P2's own controller, camera and saved P1 runtime globals.
bool Pilot_UpdateLadder(Player* player, PlayState* play, f32 worldX, f32 worldZ, bool canAct) {
    if (sPilot.ladder == PilotLadder::None)
        return false;
    (void)worldX;
    (void)worldZ;
    if (!canAct || player->ageProperties == nullptr) {
        ShipCrewPlayer_CancelNativeClimbForPilot(play, player);
        Pilot_ClearLadder(player);
        return false;
    }

    if (!ShipCrewPlayer_IsNativeClimbAction(player)) {
        // The native action itself decided to dismount, climb a ledge or
        // detach. Clear pilot ownership only AFTER that last native tick,
        // so a queued root-motion frame is never cancelled mid-update.
        Actor* actor = &player->actor;
        Actor_UpdateBgCheckInfo(play, actor, 26.0f, player->ageProperties->wallCheckRadius,
                                player->ageProperties->ceilingCheckHeight, 0x3F);
        sPilot.ladderExitGraceFrames = 55;
        sPilot.ladderExitPosition = actor->world.pos;
        sPilot.ledgeCooldownFrames = std::max(sPilot.ledgeCooldownFrames, 18);
        SPDLOG_INFO("[ShipCrew] P2 native climb action exited at ({}, {}, {})", actor->world.pos.x, actor->world.pos.y,
                    actor->world.pos.z);
        Pilot_ClearLadder(player);
        return false;
    }

    ShipCrewPlayer_UpdateNativeClimbForPilot(play, player, &play->state.input[1]);
    Actor_SetFocus(&player->actor, 40.0f);
    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
    return true;
}

bool Pilot_TryLadder(Player* player, PlayState* play, s16 approachYaw, bool enabled, bool canAct, bool moving,
                     bool carryingBomb) {
    if (!enabled || !canAct || carryingBomb || !moving || sPilot.ledgeCooldownFrames > 0 || sPilot.ladderCooldown > 0 ||
        sPilot.rollFrames > 0 || sPilot.itemFrames > 0 || sPilot.lockedTarget != nullptr ||
        sPilot.traversal != PilotTraversal::None || sPilot.dodge != PilotDodge::None ||
        player->ageProperties == nullptr)
        return false;

    Vec3f anchor = {};
    s16 yaw = 0;
    f32 bottomY = 0.0f;
    f32 topY = 0.0f;
    // Use ACTUAL world movement rather than raw stick_y. Otherwise a
    // sideways camera angle prevents top descent or bottom entry even
    // while P2 is clearly moving straight toward the tagged ladder.
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) &&
        ShipCrewPlayer_QueryLadder(play, player, true, &anchor, &yaw, &bottomY, &topY, approachYaw)) {
        Pilot_BeginLadder(player, play, true, anchor, yaw, bottomY, topY);
        return true;
    }
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) &&
        ShipCrewPlayer_QueryLadder(play, player, false, &anchor, &yaw, &bottomY, &topY, approachYaw)) {
        Pilot_BeginLadder(player, play, false, anchor, yaw, bottomY, topY);
        return true;
    }
    // Free-climb vines share the native rung/root-motion path but select
    // P1's original phase 2/3 clips and use ledge-up rather than ladder
    // dismount at the top. Horizontal shimmy remains a later parity test.
    if (ShipCrewPlayer_QueryNativeVine(play, player, approachYaw, &anchor, &yaw, &bottomY, &topY)) {
        Pilot_BeginLadder(player, play, false, anchor, yaw, bottomY, topY, true);
        return true;
    }
    return false;
}

// A separate P2 traversal transition layer sits on top of the tested P2
// movement/physics. It uses the shared native wall geometry QUERY but owns
// its own probe timer, jump phase, animation and player actor state.
bool Pilot_TryTraversal(Player* player, PlayState* play, bool wasGrounded, bool canAct, bool moving, bool enabled,
                        bool carryingBomb) {
    Actor* actor = &player->actor;
    if (sPilot.ledgeCooldownFrames > 0)
        --sPilot.ledgeCooldownFrames;
    if (!enabled || !canAct || carryingBomb || player->ageProperties == nullptr) {
        sPilot.ledgeProbeFrames = 0;
        sPilot.ledgeProbeType = 0;
        return false;
    }

    const bool grounded = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    const bool falling = actor->velocity.y < 0.0f;
    // P1's native dispatcher owns medium steps. Retain existing autojump
    // and high-ledge behavior until those native actions are fully scoped.
    if (grounded && (player->ledgeClimbType == PLAYER_LEDGE_CLIMB_2 || player->ledgeClimbType == PLAYER_LEDGE_CLIMB_3))
        return false;
    // After a verified ladder landing, don't autojump or re-mantle the
    // adjacent railing/wall during the short transition to normal walking.
    if (sPilot.ladderExitGraceFrames > 0) {
        const f32 dx = actor->world.pos.x - sPilot.ladderExitPosition.x;
        const f32 dz = actor->world.pos.z - sPilot.ladderExitPosition.z;
        if (dx * dx + dz * dz < 55.0f * 55.0f) {
            sPilot.ledgeProbeFrames = sPilot.ledgeProbeType = 0;
            return false;
        }
        sPilot.ladderExitGraceFrames = 0;
    }
    // Player 1's autojump starts on BGCHECKFLAG_GROUND_LEAVE for a real drop,
    // with forward momentum and an unobstructed facing direction. Never
    // assign a manual jump button to P2.
    if (wasGrounded && !grounded && sPilot.traversal != PilotTraversal::AutoJump &&
        ShipCrewPlayer_ShouldNativeAutoJump(player, player->floorProperty, actor->world.pos.y - actor->floorHeight,
                                            static_cast<s16>(player->yaw - actor->shape.rot.y))) {
        f32 jumpSpeed;
        LinkAnimationHeader* nativeAnim = ShipCrewPlayer_SelectNativeAutoJump(player, &jumpSpeed);
        SPDLOG_INFO("[ShipCrew] P2 autojump ground-leave: pos=({}, {}, {}) floor={} speed={}", actor->world.pos.x,
                    actor->world.pos.y, actor->world.pos.z, actor->floorHeight, actor->speedXZ);
        Pilot_BeginJump(player, jumpSpeed);
        sPilot.nativeAutoJumpAnim = nativeAnim;
        return false;
    }

    if (sPilot.ledgeCooldownFrames > 0 || sPilot.rollFrames > 0 || sPilot.itemFrames > 0 ||
        sPilot.dodge != PilotDodge::None)
        return false;

    // The original player probes at head height using the current forward
    // yaw; probe while advancing against a wall, or when descending toward
    // a reachable edge. Do not snap P2 to arbitrary scenery when idle.
    const bool descendingTowardWall =
        !grounded && falling && (sPilot.traversal == PilotTraversal::AutoJump || !wasGrounded) && actor->speedXZ > 0.3f;
    // Native collision slows horizontal speed nearly to zero against a wall,
    // so use the stick rather than speedXZ for grounded climb intent.
    if ((!grounded && !descendingTowardWall) || (grounded && !moving) || sPilot.lockedTarget != nullptr) {
        sPilot.ledgeProbeFrames = 0;
        return false;
    }

    f32 rise = 0.0f;
    Vec3f stand = {};
    s16 facing = 0;
    const s32 type = ShipCrewPlayer_QueryLedge(play, player, &rise, &stand, &facing);
    if (type == PLAYER_LEDGE_CLIMB_NONE) {
        sPilot.ledgeProbeFrames = 0;
        sPilot.ledgeProbeType = 0;
        return false;
    }

    if (descendingTowardWall && type >= PLAYER_LEDGE_CLIMB_2 &&
        ((actor->world.pos.y - actor->floorHeight) + rise) > 70.0f * player->ageProperties->unk_08) {
        Pilot_BeginHang(player, play, rise, stand, facing);
        return true;
    }

    if (!grounded)
        return false;

    if (sPilot.ledgeProbeType == type) {
        sPilot.ledgeProbeFrames = std::min(sPilot.ledgeProbeFrames + 1, 100);
    } else {
        sPilot.ledgeProbeType = type;
        sPilot.ledgeProbeFrames = 1;
    }

    if (type == PLAYER_LEDGE_CLIMB_1 && sPilot.ledgeProbeFrames >= 3) {
        SPDLOG_INFO("[ShipCrew] P2 low-step jump: rise={} pos=({}, {}, {})", rise, actor->world.pos.x,
                    actor->world.pos.y, actor->world.pos.z);
        f32 nativeSpeed;
        LinkAnimationHeader* nativeAnim = ShipCrewPlayer_SelectNativeAutoJump(player, &nativeSpeed);
        Pilot_BeginJump(player, rise * 0.08f + 5.5f);
        sPilot.nativeAutoJumpAnim = nativeAnim;
        actor->speedXZ = player->linearVelocity = 2.5f;
        actor->world.rot.y = player->yaw = facing;
        return false;
    }
    if (sPilot.ledgeProbeFrames < 6)
        return false;

    sPilot.ledgeStand = stand;
    sPilot.ledgeRise = rise;
    sPilot.ledgeFacing = facing;
    sPilot.ledgeProbeFrames = 0;
    if (type == PLAYER_LEDGE_CLIMB_4) {
        SPDLOG_INFO("[ShipCrew] P2 high-step: rise={} pos=({}, {}, {})", rise, actor->world.pos.x, actor->world.pos.y,
                    actor->world.pos.z);
        sPilot.traversal = PilotTraversal::HighStepWindup;
        actor->speedXZ = player->linearVelocity = actor->velocity.y = 0.0f;
        actor->gravity = 0.0f;
        actor->world.rot.y = actor->shape.rot.y = player->yaw = facing;
        LinkAnimationHeader* anim = Pilot_Animation(gPlayerAnim_link_normal_250jump_start);
        LinkAnimation_Change(play, &player->skelAnime, anim, 1.0f, 0.0f, Animation_GetLastFrame(anim), ANIMMODE_ONCE,
                             -3.0f);
    } else {
        Pilot_BeginClimb(player, play, type, false);
    }
    return true;
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
// Environmental props already implement P1's native lift/throw physics.
// P2 only supplies an independent spatial candidate and the original
// parent/child handoff; never spawn a substitute actor or duplicate drops.
Actor* Pilot_LiveProp(PlayState* play, Actor* ptr) {
    if (ptr == nullptr)
        return nullptr;
    for (Actor* a = play->actorCtx.actorLists[ACTORCAT_PROP].head; a != nullptr; a = a->next) {
        if (a == ptr)
            return a->update != nullptr ? a : nullptr;
    }
    return nullptr;
}
Actor* Pilot_FindContextProp(PlayState* play, Player* player) {
    Actor* best = nullptr;
    f32 bestDistance = 100.0f * 100.0f;
    Actor* actor = &player->actor;
    for (Actor* p = play->actorCtx.actorLists[ACTORCAT_PROP].head; p != nullptr; p = p->next) {
        if (p->update == nullptr || p->parent != nullptr || !ShipCrewPlayer_CanLiftContextActor(p))
            continue;
        // Large rocks use P1's separate silver gauntlet cutscene/animation;
        // this first pass deliberately covers small rocks, plants and pots.
        if (p->id == ACTOR_EN_ISHI && (p->params & 0xF) == 1)
            continue;
        const f32 dx = p->world.pos.x - actor->world.pos.x;
        const f32 dz = p->world.pos.z - actor->world.pos.z;
        const f32 ds = dx * dx + dz * dz;
        const f32 nativeReach = p->id == ACTOR_EN_ISHI ? 50.0f : 100.0f;
        if (ds >= bestDistance || ds >= nativeReach * nativeReach ||
            std::fabs(p->world.pos.y - actor->world.pos.y) > 30.0f)
            continue;
        const s16 toward = Math_Atan2S(dx, dz);
        if (ABS((s16)(toward - actor->shape.rot.y)) > 0x3300)
            continue;
        best = p;
        bestDistance = ds;
    }
    return best;
}
void Pilot_DetachProp(Player* player, Actor* prop, bool throwing) {
    const s16 yaw = player->actor.shape.rot.y;
    if (prop != nullptr) {
        const f32 reach = throwing ? 26.0f : 14.0f;
        prop->world.pos.x = player->actor.world.pos.x + Math_SinS(yaw) * reach;
        prop->world.pos.z = player->actor.world.pos.z + Math_CosS(yaw) * reach;
        prop->world.pos.y = player->actor.world.pos.y + (throwing ? 30.0f : 8.0f);
        prop->world.rot.y = prop->shape.rot.y = yaw;
        prop->speedXZ = throwing ? player->linearVelocity + 8.0f : 0.0f;
        prop->velocity.y = throwing ? 12.0f : 0.0f;
        prop->parent = nullptr; // Original prop actor now handles thrown/fall phase.
    }
    if (prop == nullptr || player->actor.child == prop)
        player->actor.child = nullptr;
    player->heldActor = nullptr;
    player->interactRangeActor = nullptr;
    player->stateFlags1 &= ~PLAYER_STATE1_CARRYING_ACTOR;
    sPilot.carriedProp = nullptr;
    sPilot.pickupCandidate = nullptr;
    sPilot.pickupAttached = false;
    sPilot.contextCooldown = 12;
}
void Pilot_PositionProp(Player* player, Actor* prop) {
    const s16 yaw = player->actor.shape.rot.y;
    prop->world.pos.x = player->actor.world.pos.x + Math_SinS(yaw) * 8.0f;
    prop->world.pos.y = player->actor.world.pos.y + 46.0f;
    prop->world.pos.z = player->actor.world.pos.z + Math_CosS(yaw) * 8.0f;
    prop->world.rot.y = prop->shape.rot.y = yaw;
    prop->speedXZ = prop->velocity.y = 0.0f;
}
bool Pilot_StartPropPickup(Player* player, PlayState* play) {
    if (sPilot.contextCooldown != 0 || sPilot.pickupCandidate != nullptr || sPilot.carriedProp != nullptr)
        return false;
    Actor* prop = Pilot_FindContextProp(play, player);
    if (prop == nullptr)
        return false;
    sPilot.pickupCandidate = prop;
    player->interactRangeActor = prop;
    player->actor.speedXZ = player->linearVelocity = 0.0f;
    // P1's Player_Action_80846050 chooses carryB and attaches on frame 4.
    LinkAnimationHeader* anim = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_carryB);
    LinkAnimation_Change(play, &player->skelAnime, anim, 1.0f, 0.0f, Animation_GetLastFrame(anim), ANIMMODE_ONCE, 0.0f);
    SPDLOG_INFO("[ShipCrew] P2 context pickup id={}", prop->id);
    return true;
}
bool Pilot_UpdatePropPickup(Player* player, PlayState* play) {
    if (sPilot.pickupCandidate == nullptr)
        return false;
    Actor* prop = Pilot_LiveProp(play, sPilot.pickupCandidate);
    if (prop == nullptr || (prop->parent != nullptr && prop->parent != &player->actor)) {
        Pilot_DetachProp(player, nullptr, false);
        return false;
    }
    player->actor.speedXZ = player->linearVelocity = 0.0f;
    const bool finished = LinkAnimation_Update(play, &player->skelAnime) != 0;
    if (!sPilot.pickupAttached && (player->skelAnime.curFrame >= 4.0f || finished)) {
        prop->parent = &player->actor;
        player->actor.child = prop;
        player->heldActor = prop;
        player->stateFlags1 |= PLAYER_STATE1_CARRYING_ACTOR;
        prop->bgCheckFlags &= 0xFF00;
        sPilot.carriedProp = prop;
        sPilot.pickupAttached = true;
    }
    if (sPilot.pickupAttached)
        Pilot_PositionProp(player, prop);
    if (finished) {
        sPilot.pickupCandidate = nullptr;
        sPilot.contextCooldown = 12;
    }
    Actor_SetFocus(&player->actor, 40.0f);
    return true;
}

// P1 and P2 use the SAME native crawlspace polygon alignment query.
// P2 owns the animation/action state instead of invoking P1's globals or
// the original OnePointCutscene_Init (which would steal P1's camera).
bool Pilot_TryCrawl(Player* player, PlayState* play, bool enabled, bool canAct, u32 pressed) {
    Actor* actor = &player->actor;
    if (!enabled || !canAct || !(pressed & BTN_A) || LINK_IS_ADULT || sPilot.crawl != PilotCrawl::None ||
        sPilot.contextCooldown != 0 || sPilot.carriedProp != nullptr || sPilot.pickupCandidate != nullptr ||
        sPilot.heldBomb != nullptr || sPilot.ladder != PilotLadder::None || sPilot.traversal != PilotTraversal::None ||
        sPilot.rollFrames > 0 || sPilot.dodge != PilotDodge::None || player->ageProperties == nullptr ||
        !(actor->bgCheckFlags & BGCHECKFLAG_GROUND) || !(actor->bgCheckFlags & BGCHECKFLAG_PLAYER_WALL_INTERACT))
        return false;

    Vec3f center = {};
    if (!ShipCrewPlayer_QueryCrawlspace(play, player, &center))
        return false;

    const s16 forward = actor->wallYaw + 0x8000;
    if (ABS((s16)(actor->shape.rot.y - forward)) >= 0x3000)
        return false;

    // As with P1, release lock-on before crawl and let the private camera
    // return to its normal native follow mode.
    sPilot.lockedTarget = nullptr;
    sPilot.parallelTargeting = false;
    sPilot.parallelRecenterFrames = 0;
    player->focusActor = nullptr;
    player->stateFlags1 &= ~(PLAYER_STATE1_Z_TARGETING | PLAYER_STATE1_PARALLEL);
    sPilot.crawl = PilotCrawl::Enter;
    ShipCrewPlayer_BeginNativeCrawl(play, player, &center);
    SPDLOG_INFO("[ShipCrew] P2 entered native P1 crawl action");
    return true;
}

bool Pilot_UpdateCrawl(Player* player, PlayState* play, const OSContPad& pad) {
    if (sPilot.crawl == PilotCrawl::None)
        return false;

    (void)pad;
    if (!ShipCrewPlayer_IsNativeCrawlAction(player)) {
        sPilot.crawl = PilotCrawl::None;
        sPilot.contextCooldown = 20;
        player->stateFlags2 &= ~PLAYER_STATE2_CRAWLING;
        ShipCrewPlayer_ResetNativeGravity(player);
        return false;
    }

    const bool stillCrawling = ShipCrewPlayer_UpdateNativeCrawlForPilot(play, player, &play->state.input[1]) != 0;
    if (!stillCrawling) {
        sPilot.crawl = PilotCrawl::None;
        sPilot.contextCooldown = 20;
        player->stateFlags2 &= ~PLAYER_STATE2_CRAWLING;
        ShipCrewPlayer_ResetNativeGravity(player);
        SPDLOG_INFO("[ShipCrew] P2 native crawl completed; standing restored");
    } else {
        sPilot.crawl = PilotCrawl::Move;
    }

    Actor_SetFocus(&player->actor, 25.0f);
    return true;
}

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

    // Keep combat effects disabled for this movement-focused pass, but the
    // actor itself now runs the real Link update/action framework.
    Effect_Delete(play, player->meleeWeaponEffectIndex);
    player->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;
    play->func_11D54(player, play);
    actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    actor->colChkInfo.mass = 50;
    ShipCrewPlayer_ResetNativeGravity(player);

    sPilot = {};
    sPilot.actor = actor;
    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
    NameTag_RegisterForActorWithOptions(actor, "P2 PILOT", {});
}

// P2 is still stored in the NPC actor list so GET_PLAYER remains P1, but
// gameplay now comes from the SAME Player_UpdateCommon/actionFunc framework.
void Pilot_Update(Actor* actor, PlayState* play) {
    Player* player = reinterpret_cast<Player*>(actor);
    Player* p1 = GET_PLAYER(play);
    Input input;

    if (p1 == nullptr || p1 == player)
        return;

    if (sPilot.actor != actor) {
        sPilot = {};
        sPilot.actor = actor;
    }

    // Equipment/save ownership is still shared for now. The important part
    // is that equipment affects P2 through the native Player model/action
    // state rather than through a second locomotion animation selector.
    player->currentTunic = p1->currentTunic;
    player->currentBoots = p1->currentBoots;
    player->currentShield = p1->currentShield;

    input = play->state.input[1];
    if ((player->stateFlags1 & (PLAYER_STATE1_INPUT_DISABLED | PLAYER_STATE1_IN_CUTSCENE)) ||
        play->csCtx.state != CS_STATE_IDLE || play->pauseCtx.state != 0 || play->pauseCtx.debugState != 0) {
        memset(&input, 0, sizeof(input));
    } else if (player->textboxBtnCooldownTimer != 0) {
        input.cur.button &= ~(BTN_A | BTN_B | BTN_CUP);
        input.press.button &= ~(BTN_A | BTN_B | BTN_CUP);
    }

    ShipCrewPlayer_UpdateNativePilotCore(play, player, &input);

    // Player_ProcessSceneCollision intentionally sees P2 as a non-primary
    // actor, so use the original exit transaction in one scoped call after
    // P2's native collision state has been updated.
    if (!(player->stateFlags1 & (PLAYER_STATE1_LOADING | PLAYER_STATE1_IN_CUTSCENE)))
        ShipCrewPlayer_HandlePilotSceneExit(play, player);

    Actor_SetFocus(actor, 40.0f);
    if (CVarGetInteger(SHIPCREW_SPLIT_CVAR, 0) == 0)
        sPilot.cameraReady = false;

    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
}

void Pilot_Draw(Actor* actor, PlayState* play) {
    Player_Draw(actor, play);
}

void Pilot_Destroy(Actor* actor, PlayState* play) {
    Player* player = reinterpret_cast<Player*>(actor);
    if (player->heldActor != nullptr && player->heldActor->parent == actor)
        Player_DetachHeldActor(play, player);
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
extern "C" s32 ShipCrewCamera_GetSecondParallel(PlayState* play) {
    if (play == nullptr || sPilot.actor == nullptr || FindPilotActor(play) != sPilot.actor)
        return false;
    Player* p2 = reinterpret_cast<Player*>(sPilot.actor);
    return (p2->stateFlags1 & (PLAYER_STATE1_PARALLEL | PLAYER_STATE1_LOCK_ON_FORCED_TO_RELEASE)) != 0;
}

extern "C" Actor* ShipCrewCamera_GetSecondTarget(PlayState* play) {
    if (play == nullptr || sPilot.actor == nullptr || FindPilotActor(play) != sPilot.actor)
        return nullptr;
    Player* p2 = reinterpret_cast<Player*>(sPilot.actor);
    return Pilot_TargetIsLive(play, p2->focusActor) ? p2->focusActor : nullptr;
}

// Environmental actors can locate P2 even when split-screen rendering is off.
extern "C" Player* ShipCrewPilot_GetInteractionPlayer(PlayState* play) {
    if (play == nullptr || GET_PLAYER(play) == nullptr || CVarGetInteger(SHIPCREW_PILOT_CVAR, 0) == 0 ||
        CVarGetInteger(CVAR_ENHANCEMENT("IvanCoopModeEnabled"), 0) != 0)
        return nullptr;
    Actor* actor = FindPilotActor(play);
    return actor != nullptr && actor == sPilot.actor ? reinterpret_cast<Player*>(actor) : nullptr;
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
