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
s32 ShipCrewPlayer_ShouldLeaveCrawlspace(PlayState* play, Player* player, f32 crawlSpeed);
s32 ShipCrewPlayer_TryPilotSharedSceneExit(PlayState* play, Player* p2);
s32 ShipCrewPlayer_BeginNativeLedgeStep(PlayState* play, Player* player, const Vec3f* stand, s16 face, f32 rise,
                                        s32 ledgeType);
s32 ShipCrewPlayer_TickNativeLedgeForPilot(PlayState* play, Player* player, Input* input);
void ShipCrewPlayer_CancelNativeLedgeForPilot(PlayState* play, Player* player);
s32 ShipCrewPlayer_QueryNativeVine(PlayState* play, Player* player, s16 approachYaw, Vec3f* anchor, s16* facing,
                                   f32* bottomY, f32* topY);
s32 ShipCrewPlayer_CanLiftContextActor(Actor* actor);
s32 Player_CanThrowCarriedActor(Player* player, Actor* actor);
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
    bool crawlExitForward = true;
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
    bool nativeLedgeStep = false;
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
    if (sPilot.nativeLedgeStep) {
        ShipCrewPlayer_CancelNativeLedgeForPilot(gPlayState, player);
        sPilot.nativeLedgeStep = false;
    }
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
    // Grounded medium steps now use P1's EXACT setup/shape offset AND P1's
    // Player_Action_80845668. Hanging and high jumps remain isolated pilot
    // actions until their additional native cutscene transitions are scoped.
    if (!fromHang && (type == PLAYER_LEDGE_CLIMB_2 || type == PLAYER_LEDGE_CLIMB_3) &&
        ShipCrewPlayer_BeginNativeLedgeStep(play, player, &sPilot.ledgeStand, sPilot.ledgeFacing, sPilot.ledgeRise,
                                            type)) {
        sPilot.nativeLedgeStep = true;
        sPilot.traversal = PilotTraversal::Climbing;
        sPilot.ledgeCooldownFrames = 18;
        SPDLOG_INFO("[ShipCrew] P2 original P1 ledge action started: type={}", type);
        return;
    }
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

    if (sPilot.traversal == PilotTraversal::Climbing && sPilot.nativeLedgeStep) {
        const bool stillClimbing = ShipCrewPlayer_TickNativeLedgeForPilot(play, player, &play->state.input[1]);
        if (!stillClimbing) {
            Pilot_ClearTraversal(actor, player);
            sPilot.landingFrames = kLandingFrames;
            sPilot.ledgeCooldownFrames = 20;
            SPDLOG_INFO("[ShipCrew] P2 original ledge action completed");
        }
        Actor_SetFocus(actor, 40.0f);
        return true;
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
    f32 bestDistance = 50.0f * 50.0f;
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
        if (ds >= bestDistance || std::fabs(p->world.pos.y - actor->world.pos.y) > 23.0f)
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
    const CollisionPoly* wall = actor->wallPoly;
    const f32 standOff =
        player->distToInteractWall > 0.0f ? player->distToInteractWall : player->ageProperties->wallCheckRadius - 1.0f;
    actor->prevPos = actor->world.pos;
    actor->world.pos.x = center.x + standOff * COLPOLY_GET_NORMAL(wall->normal.x);
    actor->world.pos.z = center.z + standOff * COLPOLY_GET_NORMAL(wall->normal.z);
    sPilot.crawl = PilotCrawl::Enter;
    sPilot.crawlDistance = 0.0f;
    // P1's native crawl action drops ordinary lock-on before entering.
    // Otherwise P2's independent camera can remain in BATTLE/TARGET mode.
    sPilot.lockedTarget = nullptr;
    sPilot.parallelTargeting = false;
    sPilot.parallelRecenterFrames = 0;
    player->focusActor = nullptr;
    player->stateFlags1 &= ~(PLAYER_STATE1_Z_TARGETING | PLAYER_STATE1_PARALLEL);
    sPilot.crawlYaw = forward;
    actor->shape.rot.y = actor->world.rot.y = player->yaw = forward;
    actor->speedXZ = actor->velocity.y = player->linearVelocity = 0.0f;
    actor->gravity = 0.0f;
    player->stateFlags2 |= PLAYER_STATE2_CRAWLING;
    LinkAnimationHeader* animation = Pilot_Animation(gPlayerAnim_link_child_tunnel_start);
    player->skelAnime.movementFlags = 0x9D;
    LinkAnimation_Change(play, &player->skelAnime, animation, 1.0f, 0.0f, Animation_GetLastFrame(animation),
                         ANIMMODE_ONCE, 0.0f);
    SPDLOG_INFO("[ShipCrew] P2 crawlspace entry (shared native P1 polygon alignment)");
    return true;
}
bool Pilot_UpdateCrawl(Player* player, PlayState* play, const OSContPad& pad) {
    if (sPilot.crawl == PilotCrawl::None)
        return false;
    Actor* actor = &player->actor;
    actor->prevPos = actor->world.pos;
    actor->speedXZ = actor->velocity.y = player->linearVelocity = actor->gravity = 0.0f;
    // The native exit clip faces its own exit wall, not the original entry
    // direction. Preserve that yaw for the full animation/root movement.
    if (sPilot.crawl != PilotCrawl::Exit)
        actor->shape.rot.y = actor->world.rot.y = player->yaw = sPilot.crawlYaw;
    player->stateFlags2 |= PLAYER_STATE2_CRAWLING;
    player->stateFlags1 &= ~(PLAYER_STATE1_Z_TARGETING | PLAYER_STATE1_PARALLEL);
    player->focusActor = nullptr;

    if (sPilot.crawl != PilotCrawl::Move) {
        // Reuse the original P1 child tunnel entry/exit clips and the same
        // root-motion queue; isolate P2's animation queue from P1.
        AnimationContext_SetNextQueue(play);
        const bool finished = LinkAnimation_Update(play, &player->skelAnime) != 0;
        ShipCrewPlayer_QueueNativeAnimMovement(play, player);
        AnimationContext_SetNextQueue(play);
        if (finished) {
            player->skelAnime.movementFlags = 0;
            if (sPilot.crawl == PilotCrawl::Enter) {
                sPilot.crawl = PilotCrawl::Move;
            } else {
                sPilot.crawl = PilotCrawl::None;
                sPilot.contextCooldown = 20;
                player->stateFlags2 &= ~PLAYER_STATE2_CRAWLING;
                ShipCrewPlayer_ResetNativeGravity(player);
                Actor_UpdateBgCheckInfo(play, actor, 26.0f, player->ageProperties->wallCheckRadius,
                                        player->ageProperties->ceilingCheckHeight, 0x3F);
                SPDLOG_INFO("[ShipCrew] P2 crawlspace stand-up complete");
            }
        }
    } else {
        // Original P1 tunnel motion is control stick Y * 0.03; it advances
        // horizontally and tests the tagged INTERIOR exit wall on contact.
        const f32 step = pad.stick_y * 0.03f;
        if (std::fabs(step) > 0.15f) {
            const s16 movementYaw = step >= 0.0f ? sPilot.crawlYaw : (s16)(sPilot.crawlYaw + 0x8000);
            actor->world.pos.x += Math_SinS(sPilot.crawlYaw) * step;
            actor->world.pos.z += Math_CosS(sPilot.crawlYaw) * step;
            sPilot.crawlDistance += std::fabs(step);

            // Previously P2 never refreshed its wall collision during crawl.
            // That left wallPoly pointing at the ENTRANCE indefinitely, so
            // the internal exit wall could never be identified.
            // Copy Player_ProcessSceneCollision's EXACT crawl capsule
            // settings: standing-height collision can pin P2 in tunnels.
            Actor_UpdateBgCheckInfo(play, actor, 15.0f, 10.0f, 30.0f, 0x3F);
            bool exitWall = ShipCrewPlayer_ShouldLeaveCrawlspace(play, player, step);
            if (!exitWall && sPilot.crawlDistance > 22.0f) {
                // Some hollow crawl holes have only a narrow interior
                // collision polygon. Probe the original forward direction
                // when the capsule did not report WALL this exact frame.
                Vec3f from = actor->world.pos;
                from.y += 26.0f;
                Vec3f to = from;
                to.x += Math_SinS(movementYaw) * 28.0f;
                to.z += Math_CosS(movementYaw) * 28.0f;
                Vec3f hit = {};
                CollisionPoly* wall = nullptr;
                s32 bgId = BGCHECK_SCENE;
                if (BgCheck_EntityLineTest1(&play->colCtx, &from, &to, &hit, &wall, true, false, false, true, &bgId) &&
                    wall != nullptr && (SurfaceType_GetWallFlags(&play->colCtx, wall, bgId) & WALL_FLAG_CRAWLSPACE)) {
                    // Apply P1's exact surface and signed facing test to the
                    // P2 candidate instead of interpreting every nearby
                    // tagged wall as a tunnel exit.
                    CollisionPoly* oldWall = actor->wallPoly;
                    const s32 oldBgId = actor->wallBgId;
                    const s16 oldWallYaw = actor->wallYaw;
                    const u32 oldBgFlags = actor->bgCheckFlags;
                    actor->wallPoly = wall;
                    actor->wallBgId = bgId;
                    actor->wallYaw = Math_Atan2S(wall->normal.z, wall->normal.x);
                    actor->bgCheckFlags |= BGCHECKFLAG_WALL;
                    exitWall = ShipCrewPlayer_ShouldLeaveCrawlspace(play, player, step);
                    if (!exitWall) {
                        actor->wallPoly = oldWall;
                        actor->wallBgId = oldBgId;
                        actor->wallYaw = oldWallYaw;
                        actor->bgCheckFlags = oldBgFlags;
                    }
                }
            }

            if (exitWall && sPilot.crawlDistance > 22.0f) {
                sPilot.crawl = PilotCrawl::Exit;
                sPilot.crawlExitForward = step > 0.0f;
                // Like P1, face the actual exit wall, not the entrance yaw.
                actor->shape.rot.y = actor->world.rot.y = player->yaw =
                    sPilot.crawlExitForward ? (s16)(actor->wallYaw + 0x8000) : actor->wallYaw;
                LinkAnimationHeader* anim = Pilot_Animation(
                    sPilot.crawlExitForward ? gPlayerAnim_link_child_tunnel_end : gPlayerAnim_link_child_tunnel_start);
                const f32 last = Animation_GetLastFrame(anim);
                AnimationContext_SetNextQueue(play);
                player->skelAnime.movementFlags = 0x9D;
                LinkAnimation_Change(play, &player->skelAnime, anim, sPilot.crawlExitForward ? 1.0f : -1.0f,
                                     sPilot.crawlExitForward ? 0.0f : last, sPilot.crawlExitForward ? last : 0.0f,
                                     ANIMMODE_ONCE, 0.0f);
                AnimationContext_SetNextQueue(play);
                SPDLOG_INFO("[ShipCrew] P2 native crawl exit: forward={} wallYaw={} travelled={}",
                            sPilot.crawlExitForward, actor->wallYaw, sPilot.crawlDistance);
            }
        }
    }
    Actor_SetFocus(actor, 25.0f);
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

    // It is a visual/input probe, not yet a combat-capable player.
    Effect_Delete(play, player->meleeWeaponEffectIndex);
    player->meleeWeaponEffectIndex = TOTAL_EFFECT_COUNT;
    play->func_11D54(player, play);
    actor->flags |= ACTOR_FLAG_LOCK_ON_DISABLED;
    actor->colChkInfo.mass = MASS_IMMOVABLE;
    if (CVarGetInteger(SHIPCREW_NATIVE_LOCOMOTION_CVAR, 0) != 0)
        ShipCrewPlayer_ResetNativeGravity(player);
    else {
        actor->gravity = kPilotGravity;
        actor->minVelocityY = kPilotTerminalVelocity;
    }

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
    // P1 registers its initialized body collider inside Player_UpdateCommon.
    // P2 does not run that function, so its original Player cylinder was
    // invisible to prop OC collision despite matching P1's geometry.
    Collider_UpdateCylinder(actor, &player->cylinder);
    CollisionCheck_SetOC(play, &play->colChkCtx, &player->cylinder.base);

    if (sPilot.ladderExitGraceFrames > 0)
        --sPilot.ladderExitGraceFrames;
    if (sPilot.contextCooldown > 0)
        --sPilot.contextCooldown;

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
    // Match the save's native Hold/Switch Z-target preference. With no
    // available target Z enters native Parallel mode and recenters P2's camera.
    const bool holdTargeting = gSaveContext.zTargetSetting != 0;
    const bool zHeld = (buttons & BTN_Z) != 0;
    if (!Pilot_TargetIsLive(play, sPilot.lockedTarget))
        sPilot.lockedTarget = nullptr;
    if (!canAct || (holdTargeting && !zHeld)) {
        sPilot.lockedTarget = nullptr;
        sPilot.parallelTargeting = false;
        sPilot.parallelRecenterFrames = 0;
    } else if (canAct && (pressed & BTN_Z)) {
        Actor* next = Pilot_FindTarget(play, actor, sPilot.lockedTarget);
        if (next != nullptr) {
            sPilot.lockedTarget = next;
            sPilot.parallelTargeting = false;
            sPilot.parallelRecenterFrames = 0;
        } else if (sPilot.lockedTarget != nullptr) {
            // Switch mode toggles lock off when no other eligible actor exists.
            // In hold mode, losing a target returns to parallel while held.
            sPilot.lockedTarget = nullptr;
            sPilot.parallelTargeting = holdTargeting;
            sPilot.parallelFacing = actor->shape.rot.y;
        } else {
            sPilot.parallelTargeting = true;
            sPilot.parallelFacing = actor->shape.rot.y;
            sPilot.parallelRecenterFrames = 15;
        }
    }
    if (sPilot.lockedTarget != nullptr) {
        const f32 dx = sPilot.lockedTarget->world.pos.x - actor->world.pos.x;
        const f32 dz = sPilot.lockedTarget->world.pos.z - actor->world.pos.z;
        if (dx * dx + dz * dz > 1050.0f * 1050.0f) {
            sPilot.lockedTarget = nullptr;
            sPilot.parallelTargeting = holdTargeting && zHeld;
            sPilot.parallelFacing = actor->shape.rot.y;
        }
    }
    if (sPilot.parallelRecenterFrames > 0)
        --sPilot.parallelRecenterFrames;
    if (!zHeld && sPilot.parallelTargeting && (holdTargeting || sPilot.parallelRecenterFrames == 0))
        sPilot.parallelTargeting = false;
    if (sPilot.parallelTargeting)
        player->parallelYaw = sPilot.parallelFacing;
    if (sPilot.lockedTarget != nullptr) {
        player->focusActor = sPilot.lockedTarget;
        player->stateFlags1 |= PLAYER_STATE1_Z_TARGETING;
        player->stateFlags1 &= ~PLAYER_STATE1_PARALLEL;
    } else {
        player->focusActor = nullptr;
        player->stateFlags1 &= ~PLAYER_STATE1_Z_TARGETING;
        if (sPilot.parallelTargeting)
            player->stateFlags1 |= PLAYER_STATE1_PARALLEL;
        else
            player->stateFlags1 &= ~PLAYER_STATE1_PARALLEL;
    }

    Actor* carriedProp = Pilot_LiveProp(play, sPilot.carriedProp);
    if (sPilot.carriedProp != nullptr && (carriedProp == nullptr || carriedProp->parent != actor)) {
        Pilot_DetachProp(player, nullptr, false);
        carriedProp = nullptr;
    }
    if (Pilot_UpdateCrawl(player, play, pad) || Pilot_UpdatePropPickup(player, play))
        return;
    Actor* heldBomb = Pilot_FindHeldBomb(actor, play);
    // The original action button releases a carried bomb first. Otherwise A
    // rolls while running; there is no manual A-button jump.
    const bool nativeMovement = CVarGetInteger(SHIPCREW_NATIVE_LOCOMOTION_CVAR, 0) != 0;
    const bool nativeTraversal = nativeMovement && CVarGetInteger(SHIPCREW_NATIVE_TRAVERSAL_CVAR, 0) != 0;
    if (Pilot_TryCrawl(player, play, nativeTraversal, canAct, pressed))
        return;
    // Native P1 gives environmental pickup priority over the A-button roll.
    if (canAct && (pressed & BTN_A) && nativeMovement && wasGrounded && heldBomb == nullptr && sPilot.rollFrames == 0 &&
        sPilot.itemFrames == 0 && sPilot.dodge == PilotDodge::None && sPilot.traversal == PilotTraversal::None &&
        sPilot.ladder == PilotLadder::None && Pilot_StartPropPickup(player, play))
        return;
    // One cooldown clock even when probing before AND after ground movement.
    if (sPilot.ladderCooldown > 0)
        --sPilot.ladderCooldown;
    if (!nativeTraversal &&
        (sPilot.traversal == PilotTraversal::Hanging || sPilot.traversal == PilotTraversal::Climbing ||
         sPilot.traversal == PilotTraversal::HighStepWindup)) {
        Pilot_ClearTraversal(actor, player);
    }
    if (!nativeTraversal && sPilot.ladder != PilotLadder::None) {
        ShipCrewPlayer_CancelNativeClimbForPilot(play, player);
        Pilot_ClearLadder(player);
    }
    if (nativeTraversal && Pilot_UpdateLadder(player, play, worldX, worldZ, canAct)) {
        player->currentTunic = GET_PLAYER(play)->currentTunic;
        player->currentBoots = GET_PLAYER(play)->currentBoots;
        player->currentShield = GET_PLAYER(play)->currentShield;
        return;
    }
    if (nativeTraversal && Pilot_UpdateTraversal(player, play, pad, pressed, canAct)) {
        player->currentTunic = GET_PLAYER(play)->currentTunic;
        player->currentBoots = GET_PLAYER(play)->currentBoots;
        player->currentShield = GET_PLAYER(play)->currentShield;
        return;
    }
    if (nativeMovement && !sPilot.nativeMovementPreviouslyEnabled) {
        player->linearVelocity = actor->speedXZ;
        player->yaw = actor->world.rot.y;
    }
    sPilot.nativeMovementPreviouslyEnabled = nativeMovement;

    if (canAct && (pressed & BTN_A) && carriedProp != nullptr && sPilot.contextCooldown == 0) {
        const bool throwing = Player_CanThrowCarriedActor(player, carriedProp) != 0;
        Pilot_DetachProp(player, carriedProp, throwing);
        carriedProp = nullptr;
        sPilot.itemPose = PilotItemPose::BombThrow;
        sPilot.itemFrames = kItemFrames;
    } else if (canAct && (pressed & BTN_A) && heldBomb != nullptr && sPilot.itemDebounceFrames == 0) {
        Pilot_ReleaseBomb(actor, heldBomb, moving);
        heldBomb = nullptr;
    } else if (canAct && (pressed & BTN_A) && carriedProp == nullptr && heldBomb == nullptr && wasGrounded && moving &&
               sPilot.rollFrames == 0 && sPilot.itemFrames == 0 && sPilot.dodge == PilotDodge::None) {
        PilotDodge dodge = PilotDodge::None;
        if (nativeMovement && (sPilot.lockedTarget != nullptr || sPilot.parallelTargeting)) {
            const s16 facing =
                sPilot.lockedTarget != nullptr
                    ? static_cast<s16>(std::atan2(sPilot.lockedTarget->focus.pos.x - actor->world.pos.x,
                                                  sPilot.lockedTarget->focus.pos.z - actor->world.pos.z) *
                                       kRadiansToN64Angle)
                    : sPilot.parallelFacing;
            const f32 forward = worldX * Math_SinS(facing) + worldZ * Math_CosS(facing);
            const f32 right = worldZ * Math_SinS(facing) - worldX * Math_CosS(facing);
            if (std::fabs(right) > std::fabs(forward) * 1.2f)
                dodge = right > 0 ? PilotDodge::SideRight : PilotDodge::SideLeft;
            else if (forward < -0.35f)
                dodge = PilotDodge::Backflip;
            if (dodge != PilotDodge::None) {
                SPDLOG_INFO("[ShipCrew] P2 Z dodge: direction={} hostile={} parallel={}", static_cast<int>(dodge),
                            sPilot.lockedTarget != nullptr, sPilot.parallelTargeting);
                sPilot.dodge = dodge;
                sPilot.dodgeLanding = false;
                const s32 nativeDodge = static_cast<s32>(dodge);
                const bool side = dodge != PilotDodge::Backflip;
                Pilot_BeginJump(player, ShipCrewPlayer_NativeDodgeVerticalSpeed(nativeDodge));
                const s16 angle = dodge == PilotDodge::Backflip   ? static_cast<s16>(0x8000)
                                  : dodge == PilotDodge::SideLeft ? static_cast<s16>(0x4000)
                                                                  : static_cast<s16>(-0x4000);
                sPilot.dodgeYaw = static_cast<s16>(facing + angle);
                player->yaw = actor->world.rot.y = sPilot.dodgeYaw;
                actor->shape.rot.y = facing;
                actor->speedXZ = player->linearVelocity = ShipCrewPlayer_NativeDodgeHorizontalSpeed(nativeDodge);
                LinkAnimationHeader* anim = Pilot_DodgeAnim(dodge, false);
                LinkAnimation_Change(play, &player->skelAnime, anim, 1.0f, 0.0f, Animation_GetLastFrame(anim),
                                     ANIMMODE_ONCE, -3.0f);
                if (side)
                    gSaveContext.ship.stats.count[COUNT_SIDEHOPS]++;
                else
                    gSaveContext.ship.stats.count[COUNT_BACKFLIPS]++;
            }
        }
        if (dodge == PilotDodge::None) {
            sPilot.nativeRoll = nativeMovement;
            sPilot.rollRecoveryFrames = 0;
            sPilot.rollInvulnStarted = false;
            sPilot.rollFrames = nativeMovement ? 30 : kRollFrames;
            const f32 magnitude = 80.0f * std::min(inputLength, 1.0f);
            const f32 speedCap = ShipCrewPlayer_GetRunSpeedLimit();
            sPilot.rollSpeed = nativeMovement ? std::max(3.0f, ShipCrewPlayer_CalcGroundSpeedTarget(
                                                                   magnitude, speedCap, player->floorPitch, true) *
                                                                   1.5f)
                                              : kRollSpeed;
            // P1 commits rolls to Link's existing facing, not a fresh
            // camera-relative stick heading at the button-press frame.
            sPilot.rollYaw =
                nativeMovement ? actor->shape.rot.y : static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
            sPilot.landingFrames = 0;
            if (nativeMovement) {
                // P1's roll starts at 1.25x playback and lasts through the
                // animation's actual frame 20, not a hard-coded 20 physics ticks.
                ShipCrewPlayer_StartNativeRollClip(play, player, 1.0f);
                gSaveContext.ship.stats.count[COUNT_ROLLS]++;
            }
        }
    }

    // Two independent yaws are essential for native Z movement: world/yaw
    // controls travel, while shape faces the target. Do not infer a strafe
    // animation from a smoothed movement yaw (it can lag behind the stick).
    sPilot.lockMove = PilotLockMove::None;
    const bool hostileLock = sPilot.lockedTarget != nullptr;
    const bool zMovement = hostileLock || sPilot.parallelTargeting;
    s16 targetFacing = sPilot.parallelTargeting ? sPilot.parallelFacing : actor->shape.rot.y;
    if (hostileLock) {
        const Vec3f& focus = sPilot.lockedTarget->focus.pos;
        const f32 targetX = focus.x - actor->world.pos.x;
        const f32 targetZ = focus.z - actor->world.pos.z;
        if (targetX * targetX + targetZ * targetZ > 1.0f) {
            targetFacing = static_cast<s16>(std::atan2(targetX, targetZ) * kRadiansToN64Angle);
        }
    }
    if (zMovement) {
        if (canAct && moving) {
            // Relative to P2's enemy or independent Z-parallel facing, never P1's camera and not to the
            // delayed velocity heading. This stays stable as the battle
            // camera circles the target and handles diagonal movement.
            const f32 sine = Math_SinS(targetFacing);
            const f32 cosine = Math_CosS(targetFacing);
            const f32 forward = worldX * sine + worldZ * cosine;
            // Link-facing right is (-cos(yaw), +sin(yaw)), the inverse of
            // the previous sign. A right-stick direction must pick right.
            const f32 right = worldZ * sine - worldX * cosine;
            if (std::fabs(right) > std::fabs(forward) * 0.75f) {
                sPilot.lockMove = right > 0.0f ? PilotLockMove::Right : PilotLockMove::Left;
            } else if (forward < -0.25f) {
                sPilot.lockMove = PilotLockMove::Back;
            } else {
                sPilot.lockMove = PilotLockMove::Forward;
            }
        }
    }

    // Native straight running uses the ORIGINAL P1 func_8083DF68: REG(19)
    // acceleration, 1.5 braking and REG(27) turning. The previous pilot
    // incorrectly multiplied even full-speed normal running by 0.9 or 0.4,
    // causing visible speed disparity with P1.
    const f32 requestedSpeed = canAct && moving ? kRunSpeed * std::min(inputLength, 1.0f) : 0.0f;
    if (nativeMovement) {
        const f32 nativeLimit = ShipCrewPlayer_GetRunSpeedLimit();
        const f32 stickMagnitude = 80.0f * std::min(inputLength, 1.0f);
        f32 speedLimit = nativeLimit;
        if (wasGrounded && (actor->bgCheckFlags & BGCHECKFLAG_WALL)) {
            const s16 wallDiff = player->yaw - static_cast<s16>(actor->wallYaw + 0x8000);
            speedLimit = std::clamp(static_cast<f32>(std::abs(static_cast<s32>(wallDiff))) * 0.00008f,
                                    0.1f / std::max(nativeLimit, 0.1f), 1.0f) *
                         nativeLimit;
        }
        player->unk_880 = speedLimit;
        const f32 nativeTarget =
            canAct && moving ? ShipCrewPlayer_CalcNativeAnalogSpeed(player, stickMagnitude, !zMovement) : 0.0f;
        if (sPilot.dodge != PilotDodge::None) {
            player->yaw = actor->world.rot.y = sPilot.dodgeYaw;
            if (sPilot.dodgeLanding) {
                // Landing in P1 returns to the grounded action handler.
                // Holding P2 at airborne speed until a long landing clip
                // finished produced the reported stop delay and sliding.
                Math_StepToF(&player->linearVelocity, 0.0f, moving ? 2.5f : 3.5f);
            }
            actor->speedXZ = std::max(player->linearVelocity, 0.0f);
        } else if (sPilot.rollFrames > 0) {
            if (sPilot.nativeRoll) {
                // Mirror P1 Player_Action_Roll's per-frame speed calculation:
                // curved stick speed * 1.5, minimum 3, original run step,
                // and movement fixed to Link's roll-facing yaw.
                if (player->skelAnime.curFrame < 20.0f) {
                    const f32 rollTarget =
                        std::max(3.0f, ShipCrewPlayer_CalcNativeAnalogSpeed(player, stickMagnitude, true) * 1.5f);
                    // Preserve P1's committed opening roll direction, then
                    // permit modest analog corrections, not an instant
                    // reverse or a perpetual camera-facing lock.
                    if (player->skelAnime.curFrame >= 8.0f && moving) {
                        const s16 inputYaw = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
                        const s16 delta = static_cast<s16>(inputYaw - sPilot.rollYaw);
                        if (std::abs(static_cast<s32>(delta)) < 0x3000)
                            sPilot.rollYaw =
                                static_cast<s16>(sPilot.rollYaw + std::clamp(static_cast<s32>(delta), -0x380, 0x380));
                    }
                    ShipCrewPlayer_ApplyNativeRunMotion(player, rollTarget, sPilot.rollYaw);
                } else {
                    Math_StepToF(&player->linearVelocity, 0.0f, REG(43) / 100.0f);
                }
                actor->speedXZ = std::max(player->linearVelocity, 0.0f);
                actor->world.rot.y = player->yaw;
            } else {
                actor->speedXZ = kRollSpeed;
            }
        } else if (!wasGrounded) {
            // Use P1's actual airborne function for natural falls AND
            // autojumps, rather than reimplementing the coefficients here.
            // P1 uses the linear input curve in the air, not the ground curve.
            const s16 desiredYaw =
                moving ? static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle) : player->yaw;
            const f32 airTarget =
                canAct && moving ? ShipCrewPlayer_CalcNativeAnalogSpeed(player, stickMagnitude, false) : 0.0f;
            ShipCrewPlayer_ApplyNativeAirMotion(player, airTarget, desiredYaw);
            actor->speedXZ = std::max(0.0f, player->linearVelocity);
            actor->world.rot.y = player->yaw;
        } else if (sPilot.rollRecoveryFrames > 0 &&
                   (!canAct || !moving ||
                    std::abs(static_cast<s32>(static_cast<s16>(
                        static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle) - player->yaw))) > 0x1800)) {
            // P2 previously carried almost the entire roll velocity into
            // ordinary walking, where a large yaw change felt like ice.
            // Apply stronger braking only during the short roll handoff.
            if (Math_StepToF(&player->linearVelocity, 0.0f, 2.5f) && canAct && moving)
                player->yaw = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
            actor->speedXZ = std::max(player->linearVelocity, 0.0f);
            actor->world.rot.y = player->yaw;
        } else if (!canAct || !moving) {
            // Vanilla standing uses the boot-dependent idle deceleration.
            ShipCrewPlayer_ApplyNativeIdleBrake(player);
            actor->speedXZ = std::max(player->linearVelocity, 0.0f);
            actor->world.rot.y = player->yaw;
        } else {
            const s16 desiredYaw = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
            const s16 yawDiff = desiredYaw - player->yaw;
            if (zMovement) {
                // Native Link selects separate side/back locomotion states.
                // Do not run the ordinary >90-degree turn brake here: that
                // prevented lateral motion as the camera turned in battle.
                player->yaw = desiredYaw;
                if (sPilot.lockMove == PilotLockMove::Back) {
                    Math_AsymStepToF(&player->linearVelocity, nativeTarget * 1.5f, 1.5f, 2.0f);
                } else if (sPilot.lockMove == PilotLockMove::Left || sPilot.lockMove == PilotLockMove::Right) {
                    Math_AsymStepToF(&player->linearVelocity, nativeTarget * 0.9f, 2.0f, 3.0f);
                } else {
                    ShipCrewPlayer_ApplyNativeRunMotion(player, nativeTarget, desiredYaw);
                }
            } else if (wasGrounded && std::abs(static_cast<s32>(yawDiff)) > 0x6000) {
                if (Math_StepToF(&player->linearVelocity, 0.0f, 1.0f))
                    player->yaw = desiredYaw;
            } else {
                ShipCrewPlayer_ApplyNativeRunMotion(player, nativeTarget, desiredYaw);
            }
            actor->speedXZ = std::max(player->linearVelocity, 0.0f);
            actor->world.rot.y = player->yaw;
        }
        if (!zMovement)
            actor->shape.rot.y = actor->world.rot.y;
    } else if (sPilot.rollFrames > 0) {
        actor->speedXZ = kRollSpeed;
    } else {
        if (requestedSpeed > actor->speedXZ) {
            actor->speedXZ = std::min(requestedSpeed, actor->speedXZ + kAcceleration);
        } else {
            actor->speedXZ = std::max(requestedSpeed, actor->speedXZ - kDeceleration);
        }
        if (canAct && moving) {
            actor->world.rot.y = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
            if (!zMovement)
                actor->shape.rot.y = actor->world.rot.y;
        }
    }

    if (zMovement && sPilot.rollFrames == 0 && sPilot.dodge == PilotDodge::None) {
        actor->shape.rot.y = targetFacing;
    }
    // Player 1 can attach to the ladder at a platform lip before its
    // ground-leave physics step. P2 previously tested only after falling.
    const s16 approachYaw = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
    if (nativeTraversal && wasGrounded &&
        Pilot_TryLadder(player, play, approachYaw, nativeTraversal, canAct, moving,
                        heldBomb != nullptr || carriedProp != nullptr)) {
        Actor_SetFocus(actor, 40.0f);
        return;
    }
    // P1 refreshes its native gravity every movement frame rather than
    // keeping the standalone pilot's hard-coded -1.0/-18 values.
    // This path runs only after special ladder/climb actions have returned.
    if (nativeMovement) {
        ShipCrewPlayer_ResetNativeGravity(player);
        if (sPilot.lockedTarget != nullptr && !(actor->bgCheckFlags & BGCHECKFLAG_GROUND))
            actor->gravity = -1.2f;
    }
    const bool fallingBeforeMove = actor->velocity.y < -1.0f;
    Actor_MoveXZGravity(actor);
    if (nativeMovement && player->ageProperties != nullptr) {
        Actor_UpdateBgCheckInfo(play, actor, 26.0f, player->ageProperties->wallCheckRadius,
                                player->ageProperties->ceilingCheckHeight, 0x3F);
        if (actor->floorPoly != nullptr && (actor->bgCheckFlags & BGCHECKFLAG_GROUND)) {
            // Remember P2's own native floor property for the NEXT frame's
            // ground-leave gate. P1 already does this in its collision pass.
            player->floorProperty = func_80041EA4(&play->colCtx, actor->floorPoly, actor->floorBgId);
            // Link's floor pitch is sampled in the direction of travel and
            // feeds directly into the vanilla analog speed curve next frame.
            const f32 nx = COLPOLY_GET_NORMAL(actor->floorPoly->normal.x);
            const f32 ny = COLPOLY_GET_NORMAL(actor->floorPoly->normal.y);
            const f32 nz = COLPOLY_GET_NORMAL(actor->floorPoly->normal.z);
            if (std::fabs(ny) > 0.01f) {
                const f32 slope = -(nx * Math_SinS(player->yaw) + nz * Math_CosS(player->yaw)) / ny;
                player->floorPitch = Math_Atan2S(1.0f, slope);
            }
        } else {
            player->floorPitch = 0;
        }
    } else {
        Actor_UpdateBgCheckInfo(play, actor, kWallCheckHeight, kWallCheckRadius, kCeilingCheckHeight, 0x1D);
    }
    // Only one scene can be loaded by the original engine. When BOTH
    // Links reach the same vicinity, let P2 request P1's native floor-exit
    // transaction; independent far-away scene streaming is unsupported.
    if (nativeMovement && canAct && ShipCrewPlayer_TryPilotSharedSceneExit(play, player)) {
        SPDLOG_INFO("[ShipCrew] P2 requested native shared scene exit with P1 nearby");
        Actor_SetFocus(actor, 40.0f);
        return;
    }
    if (wasGrounded && !(actor->bgCheckFlags & BGCHECKFLAG_GROUND))
        sPilot.takeoffY = actor->world.pos.y;
    if (Pilot_TryLadder(player, play, approachYaw, nativeTraversal, canAct, moving,
                        (Pilot_FindHeldBomb(actor, play) != nullptr || sPilot.carriedProp != nullptr))) {
        Actor_SetFocus(actor, 40.0f);
        sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
        sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
        return;
    }
    if (Pilot_TryTraversal(player, play, wasGrounded, canAct, moving, nativeTraversal,
                           (Pilot_FindHeldBomb(actor, play) != nullptr || sPilot.carriedProp != nullptr))) {
        Actor_SetFocus(actor, 40.0f);
        sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
        sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
        return;
    }
    Actor_SetFocus(actor, 40.0f);
    if (CVarGetInteger(SHIPCREW_SPLIT_CVAR, 0) == 0) {
        sPilot.cameraReady = false;
    }
    const bool grounded = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    if (!wasGrounded && grounded && fallingBeforeMove) {
        if (sPilot.dodge != PilotDodge::None)
            sPilot.dodgeLanding = true;
        const f32 fallDistance = sPilot.takeoffY - actor->world.pos.y;
        sPilot.longLanding = nativeTraversal && fallDistance > 80.0f;
        if (sPilot.dodge == PilotDodge::None)
            sPilot.landingFrames = sPilot.longLanding ? kLandingFrames * 2 : kLandingFrames;
        if (sPilot.traversal == PilotTraversal::AutoJump)
            Pilot_ClearTraversal(actor, player);
    }

    carriedProp = Pilot_LiveProp(play, sPilot.carriedProp);
    if (carriedProp != nullptr && carriedProp->parent == actor)
        Pilot_PositionProp(player, carriedProp);
    // Keep the live bomb attached to P2's position until A releases it.
    // Check the actor list before touching the pointer; the fuse still runs.
    heldBomb = Pilot_FindHeldBomb(actor, play);
    if (heldBomb != nullptr) {
        Pilot_PositionHeldBomb(actor, heldBomb);
    }

    // Genuine button-down edges plus a minimum cooldown prevent held-button
    // repeats from draining the entire shared save inventory.
    if (canAct && grounded && heldBomb == nullptr && carriedProp == nullptr && sPilot.rollFrames == 0 &&
        sPilot.itemFrames == 0 && sPilot.itemDebounceFrames == 0) {
        if (pressed & BTN_CLEFT) {
            Pilot_UseBomb(actor, play);
        } else if (pressed & BTN_CRIGHT) {
            Pilot_UseNut(actor, play);
        }
    }

    // Independent P2 skeleton, borrowing ORIGINAL P1 animation assets by
    // group and P2's own modelAnimType (equipment stance). Never read P1's
    // animation time or pose: both Links can locomote independently.
    LinkAnimationHeader* animation = nullptr;
    u8 mode = ANIMMODE_LOOP;
    bool locomotionLoop = false;
    const f32 nativeInputSpeed =
        nativeMovement && moving
            ? ShipCrewPlayer_CalcNativeAnalogSpeed(player, 80.0f * std::min(inputLength, 1.0f),
                                                   sPilot.lockedTarget == nullptr && !sPilot.parallelTargeting)
            : 0.0f;
    const bool running = nativeMovement ? nativeInputSpeed > 4.9f : inputLength > kWalkThreshold;
    if (sPilot.dodge != PilotDodge::None) {
        animation = Pilot_DodgeAnim(sPilot.dodge, sPilot.dodgeLanding);
        mode = ANIMMODE_ONCE;
    } else if (sPilot.rollFrames > 0 && grounded) {
        animation = nativeMovement ? ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_landing_roll)
                                   : Pilot_Animation(gPlayerAnim_link_normal_landing_roll_free);
        mode = ANIMMODE_ONCE;
    } else if (!grounded) {
        // A P1 autojump retains the actual jump pose while descending from
        // its apex; P1 changes to the static falling/landing pose only after
        // dropping below takeoff height or colliding with a wall. P2 used to
        // flip immediately at velocity.y < 0, making it LOOK like gravity
        // suddenly accelerated even when its physical velocity was normal.
        if (nativeTraversal && sPilot.traversal == PilotTraversal::AutoJump && sPilot.nativeAutoJumpAnim != nullptr) {
            const f32 fallDistance = sPilot.takeoffY - actor->world.pos.y;
            if (ShipCrewPlayer_ShouldEnterFallAnimation(player, false, fallDistance)) {
                animation = Pilot_Animation(gPlayerAnim_link_normal_landing);
                mode = ANIMMODE_ONCE;
            } else {
                animation = sPilot.nativeAutoJumpAnim;
                mode = ANIMMODE_ONCE;
            }
        } else if (nativeTraversal && actor->velocity.y < 0.0f) {
            // Ordinary unassisted falls still use P1's fall-wait pose.
            animation = Pilot_Animation(gPlayerAnim_link_normal_landing_wait);
        } else {
            animation = nativeMovement && actor->speedXZ > 4.0f ? Pilot_Animation(gPlayerAnim_link_normal_run_jump)
                                                                : Pilot_Animation(gPlayerAnim_link_normal_jump);
        }
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
    } else if (heldBomb != nullptr || carriedProp != nullptr) {
        animation = Pilot_Animation(moving ? gPlayerAnim_link_normal_carryB : gPlayerAnim_link_normal_carryB_wait);
        locomotionLoop = moving;
    } else if (sPilot.landingFrames > 0) {
        const bool longFall = nativeTraversal && sPilot.longLanding;
        animation = nativeMovement ? ShipCrewPlayer_GetGroupAnimation(player, longFall ? PLAYER_ANIMGROUP_landing
                                                                                       : PLAYER_ANIMGROUP_short_landing)
                                   : Pilot_Animation(gPlayerAnim_link_normal_short_landing_free);
        mode = ANIMMODE_ONCE;
    } else if (zMovement) {
        // The animation must follow the independent motion quadrant chosen
        // from the target-relative stick vector. Comparing smoothed yaw with
        // actor facing gave the wrong side (or a forward run) after camera
        // movement, especially when locking onto a moving enemy.
        if (!moving || actor->speedXZ < 0.15f) {
            animation = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_wait);
        } else {
            switch (sPilot.lockMove) {
                case PilotLockMove::Back:
                    animation = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_back_walk);
                    break;
                case PilotLockMove::Left:
                    animation = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_side_walkL);
                    break;
                case PilotLockMove::Right:
                    animation = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_side_walkR);
                    break;
                default:
                    animation = ShipCrewPlayer_GetGroupAnimation(player, running ? PLAYER_ANIMGROUP_run
                                                                                 : PLAYER_ANIMGROUP_walk);
                    break;
            }
            locomotionLoop = true;
        }
    } else if (nativeMovement) {
        if (!moving || actor->speedXZ < 0.15f)
            animation = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_wait);
        else {
            animation =
                ShipCrewPlayer_GetGroupAnimation(player, running ? PLAYER_ANIMGROUP_run : PLAYER_ANIMGROUP_walk);
            locomotionLoop = true;
        }
    } else if (!moving) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_wait_free);
    } else if (inputLength <= kWalkThreshold) {
        animation = Pilot_Animation(gPlayerAnim_link_normal_walk_free);
    } else {
        animation = Pilot_Animation(gPlayerAnim_link_normal_run_free);
    }

    if (player->skelAnime.animation != animation) {
        const bool nativeFrozenFallPose = nativeTraversal && !grounded &&
                                          sPilot.traversal == PilotTraversal::AutoJump &&
                                          animation == Pilot_Animation(gPlayerAnim_link_normal_landing);
        // P1 enters its falling pose using start=end=0 with an 8-frame
        // transition; playing the entire landing animation mid-air is wrong.
        LinkAnimation_Change(play, &player->skelAnime, animation, 1.0f, 0.0f,
                             nativeFrozenFallPose ? 0.0f : Animation_GetLastFrame(animation), mode,
                             nativeFrozenFallPose ? 8.0f : -3.0f);
    }
    // Native Link's gait speed follows locomotion rather than replaying one
    // fixed-rate walk/run loop across every analog-stick magnitude.
    if (nativeMovement && locomotionLoop) {
        player->skelAnime.playSpeed = std::clamp(actor->speedXZ / (running ? 5.0f : 2.0f), 0.6f, 2.0f);
    }
    const bool animationFinished = LinkAnimation_Update(play, &player->skelAnime) != 0;
    if (sPilot.dodge != PilotDodge::None && sPilot.dodgeLanding &&
        (animationFinished || (moving && player->skelAnime.curFrame >= 3.0f) ||
         (!moving && player->skelAnime.curFrame >= 5.0f && player->linearVelocity <= 0.5f))) {
        // P1 hands control back at touchdown. Give P2 a few frames of
        // landing animation without forcing the entire clip to play before
        // the stick can resume grounded locomotion.
        sPilot.dodge = PilotDodge::None;
        sPilot.dodgeLanding = false;
    }

    if (sPilot.rollFrames > 0) {
        if (sPilot.nativeRoll) {
            if (!sPilot.rollInvulnStarted && player->skelAnime.curFrame >= 8.0f) {
                Player_SetInvulnerability(player, -10);
                sPilot.rollInvulnStarted = true;
            }
            const s16 inputYaw = static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle);
            const bool changedCourse =
                moving && std::abs(static_cast<s32>(static_cast<s16>(inputYaw - sPilot.rollYaw))) > 0x2800;
            // P1 can process a new action once its roll is mature. Release
            // or a deliberate major change of direction recovers at frame
            // 15 instead of holding P2 rigidly until frame 20.
            if (player->skelAnime.curFrame >= 20.0f ||
                (player->skelAnime.curFrame >= 15.0f && (!moving || changedCourse)) ||
                player->skelAnime.animation !=
                    ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_landing_roll)) {
                sPilot.rollRecoveryFrames = (!moving || changedCourse) ? 5 : 0;
                sPilot.nativeRoll = false;
                sPilot.rollFrames = 0;
            }
        } else {
            --sPilot.rollFrames;
        }
    }
    if (sPilot.rollRecoveryFrames > 0)
        --sPilot.rollRecoveryFrames;
    if (sPilot.landingFrames > 0) {
        --sPilot.landingFrames;
        if (sPilot.landingFrames == 0)
            sPilot.longLanding = false;
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
    Actor* carriedProp = Pilot_LiveProp(play, sPilot.carriedProp);
    if (carriedProp != nullptr && carriedProp->parent == actor)
        Pilot_DetachProp(reinterpret_cast<Player*>(actor), carriedProp, false);
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
// P2's private camera can reproduce the original 9601/9602 crawl-exit
// presentation without allocating a global one-point cutscene that would
// steal P1's main camera and block independent movement.
extern "C" s32 ShipCrewPilot_GetCrawlExitCamera(PlayState* play, s32* forward, f32* progress) {
    if (play == nullptr || forward == nullptr || progress == nullptr || sPilot.crawl != PilotCrawl::Exit ||
        FindPilotActor(play) != sPilot.actor)
        return false;
    Player* p2 = reinterpret_cast<Player*>(sPilot.actor);
    const f32 last = Animation_GetLastFrame(p2->skelAnime.animation);
    *forward = sPilot.crawlExitForward;
    *progress = last > 0.0f ? std::clamp(p2->skelAnime.curFrame / last, 0.0f, 1.0f) : 0.0f;
    if (!sPilot.crawlExitForward)
        *progress = 1.0f - *progress; // Reverse clip plays last -> zero.
    return true;
}

extern "C" s32 ShipCrewCamera_GetSecondParallel(PlayState* play) {
    if (play == nullptr || sPilot.actor == nullptr || FindPilotActor(play) != sPilot.actor)
        return false;
    return sPilot.parallelTargeting;
}

extern "C" Actor* ShipCrewCamera_GetSecondTarget(PlayState* play) {
    if (play == nullptr || sPilot.actor == nullptr || FindPilotActor(play) != sPilot.actor ||
        !Pilot_TargetIsLive(play, sPilot.lockedTarget))
        return nullptr;
    return sPilot.lockedTarget;
}

// Prop collision and interaction access must not depend on split-screen
// rendering or the secondary camera being initialized.
extern "C" Player* ShipCrewPilot_GetInteractionPlayer(PlayState* play) {
    if (play == nullptr || CVarGetInteger(SHIPCREW_PILOT_CVAR, 0) == 0 ||
        CVarGetInteger(CVAR_ENHANCEMENT("IvanCoopModeEnabled"), 0) != 0 || GET_PLAYER(play) == nullptr)
        return nullptr;
    Actor* pilot = FindPilotActor(play);
    return pilot != nullptr && pilot == sPilot.actor && pilot->update != nullptr ? reinterpret_cast<Player*>(pilot)
                                                                                 : nullptr;
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
