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
s32 ShipCrewPlayer_QueryLedge(PlayState* play, Player* player, f32* rise, Vec3f* stand, s16* facing);
s32 ShipCrewPlayer_QueryLadder(PlayState* play, Player* player, s32 fromTop, Vec3f* anchor, s16* facing, f32* bottomY,
                               f32* topY);
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
enum class PilotLadder { None, EnterBottom, EnterTop, Active, DismountBottom, DismountTop };
enum class PilotLockMove { None, Forward, Back, Left, Right };
enum class PilotDodge { None, SideLeft, Backflip, SideRight };
struct PilotRuntime {
    Actor* actor = nullptr;
    Actor* heldBomb = nullptr;
    Actor* lockedTarget = nullptr;
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
    PilotDodge dodge = PilotDodge::None;
    bool dodgeLanding = false;
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
    int ladderStep = 0;
    int ladderDirection = 0;
    int ladderCooldown = 0;
    Vec3f ladderAnchor = {};
    Vec3f ladderTopEntry = {};
    s16 ladderYaw = 0;
    f32 ladderTopY = 0.0f;
    f32 ladderBottomY = 0.0f;
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
    actor->gravity = sPilot.lockedTarget != nullptr ? -1.2f : kPilotGravity;
    actor->velocity.y = verticalSpeed;
    actor->bgCheckFlags &= ~BGCHECKFLAG_GROUND;
    player->stateFlags1 |= PLAYER_STATE1_JUMPING;
    player->stateFlags1 &= ~(PLAYER_STATE1_HANGING_OFF_LEDGE | PLAYER_STATE1_CLIMBING_LEDGE);
    sPilot.traversal = PilotTraversal::AutoJump;
    sPilot.takeoffY = actor->world.pos.y;
    sPilot.landingFrames = 0;
    sPilot.ledgeCooldownFrames = 12;
}

// Native assets match P1's D_80853D4C directional hop table.
LinkAnimationHeader* Pilot_DodgeAnim(PilotDodge dodge, bool landing) {
    if (dodge == PilotDodge::SideLeft)
        return Pilot_Animation(landing ? gPlayerAnim_link_fighter_Lside_jump_endL
                                       : gPlayerAnim_link_fighter_Lside_jump);
    if (dodge == PilotDodge::SideRight)
        return Pilot_Animation(landing ? gPlayerAnim_link_fighter_Rside_jump_endR
                                       : gPlayerAnim_link_fighter_Rside_jump);
    return Pilot_Animation(landing ? gPlayerAnim_link_fighter_backturn_jump_endR
                                   : gPlayerAnim_link_fighter_backturn_jump);
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
    sPilot.ladderDirection = 0;
    sPilot.ladderCooldown = 16;
    player->stateFlags1 &= ~PLAYER_STATE1_CLIMBING_LADDER;
    player->actor.gravity = kPilotGravity;
    player->actor.shape.yOffset = 0.0f;
}

void Pilot_BeginLadder(Player* player, PlayState* play, bool fromTop, const Vec3f& anchor, s16 yaw, f32 bottomY,
                       f32 topY) {
    Actor* actor = &player->actor;
    sPilot.ladder = fromTop ? PilotLadder::EnterTop : PilotLadder::EnterBottom;
    SPDLOG_INFO("[ShipCrew] P2 ladder attached: fromTop={} bottomY={} topY={}", fromTop, bottomY, topY);
    sPilot.ladderAnchor = anchor;
    sPilot.ladderTopEntry = actor->world.pos;
    sPilot.ladderYaw = yaw;
    sPilot.ladderBottomY = bottomY;
    sPilot.ladderTopY = topY;
    sPilot.ladderStep = 0;
    sPilot.ladderDirection = 0;
    actor->world.pos.x = anchor.x;
    actor->world.pos.z = anchor.z;
    actor->prevPos = actor->world.pos; // Ladder attachment is an explicit position correction.
    actor->velocity.y = actor->speedXZ = player->linearVelocity = 0.0f;
    actor->gravity = 0.0f;
    actor->world.rot.y = actor->shape.rot.y = player->yaw = yaw;
    actor->bgCheckFlags &= ~BGCHECKFLAG_GROUND;
    player->stateFlags1 |= PLAYER_STATE1_CLIMBING_LADDER;
    LinkAnimationHeader* anim = fromTop ? player->ageProperties->unk_A8 : player->ageProperties->unk_A4;
    LinkAnimation_Change(play, &player->skelAnime, anim, 1.0f, 0.0f, Animation_GetLastFrame(anim), ANIMMODE_ONCE,
                         -3.0f);
}

void Pilot_LadderDismount(Player* player, PlayState* play, bool atTop, const Vec3f& landing) {
    Actor* actor = &player->actor;
    actor->world.pos = landing;
    actor->prevPos = actor->world.pos; // Prevent visual rewind after top/bottom dismount.
    actor->velocity.y = actor->speedXZ = player->linearVelocity = 0.0f;
    actor->gravity = 0.0f;
    actor->bgCheckFlags |= BGCHECKFLAG_GROUND;
    LinkAnimationHeader* anim = atTop ? player->ageProperties->unk_CC[sPilot.ladderStep & 1]
                                      : player->ageProperties->unk_C4[sPilot.ladderStep & 1];
    sPilot.ladder = atTop ? PilotLadder::DismountTop : PilotLadder::DismountBottom;
    sPilot.ladderDirection = 0;
    LinkAnimation_Change(play, &player->skelAnime, anim, 4.0f / 3.0f, 0.0f, Animation_GetLastFrame(anim), ANIMMODE_ONCE,
                         0.0f);
}

// Runs before ordinary movement so climbing never receives an independent
// gravity step, stale roll animation or a second ammo transaction.
bool Pilot_UpdateLadder(Player* player, PlayState* play, const OSContPad& pad, bool canAct) {
    if (sPilot.ladder == PilotLadder::None)
        return false;
    Actor* actor = &player->actor;
    if (!canAct || player->ageProperties == nullptr) {
        Pilot_ClearLadder(player);
        return false;
    }

    actor->prevPos = actor->world.pos;
    actor->velocity.y = actor->speedXZ = player->linearVelocity = 0.0f;
    actor->gravity = 0.0f;
    actor->world.rot.y = actor->shape.rot.y = player->yaw = sPilot.ladderYaw;
    player->stateFlags1 |= PLAYER_STATE1_CLIMBING_LADDER;
    if (sPilot.ladder == PilotLadder::EnterTop || sPilot.ladder == PilotLadder::EnterBottom) {
        const bool enteringFromTop = sPilot.ladder == PilotLadder::EnterTop;
        if (LinkAnimation_Update(play, &player->skelAnime)) {
            sPilot.ladder = PilotLadder::Active;
            sPilot.ladderDirection = 0;
            if (enteringFromTop)
                actor->world.pos.y -= 2.0f;
        }
    } else if (sPilot.ladder == PilotLadder::DismountTop || sPilot.ladder == PilotLadder::DismountBottom) {
        if (LinkAnimation_Update(play, &player->skelAnime))
            Pilot_ClearLadder(player);
    } else {
        const int direction = pad.stick_y > 23 ? 1 : (pad.stick_y < -23 ? -1 : 0);
        if (direction == 0) {
            // Hold on the ladder at the current animation frame.
            player->stateFlags2 |= PLAYER_STATE2_STATIONARY_LADDER;
        } else {
            player->stateFlags2 &= ~PLAYER_STATE2_STATIONARY_LADDER;
            const f32 stickSpeed = std::clamp(std::fabs(static_cast<f32>(pad.stick_y)) * 0.05f, 1.0f, 3.35f);
            const f32 climbSpeed = stickSpeed + CVarGetInteger(CVAR_ENHANCEMENT("ClimbSpeed"), 0);
            if (direction != sPilot.ladderDirection) {
                sPilot.ladderDirection = direction;
                LinkAnimationHeader* anim = player->ageProperties->unk_AC[sPilot.ladderStep & 1];
                const f32 last = Animation_GetLastFrame(anim);
                LinkAnimation_Change(play, &player->skelAnime, anim, direction * climbSpeed,
                                     direction > 0 ? 0.0f : last, direction > 0 ? last : 0.0f, ANIMMODE_ONCE, 0.0f);
            } else {
                player->skelAnime.playSpeed = direction * climbSpeed;
            }
            if (LinkAnimation_Update(play, &player->skelAnime)) {
                sPilot.ladderStep ^= 1;
                sPilot.ladderDirection = 0;
            }
            // The vanilla ladder uses root translation from age-specific
            // climbing assets. The separate pilot currently owns position,
            // so reproduce that translation without Player_StartAnimMovement
            // (which would mutate P1-only action globals).
            const f32 cycle = std::max(1.0f, Animation_GetLastFrame(player->skelAnime.animation));
            // P1's native ladder is a roughly 15-unit root-motion step per
            // animation cycle, not the pilot's arbitrary 1.45 units/frame.
            // Scale displacement with animation playback, including reverse.
            actor->world.pos.y += direction * (15.0f / cycle) * climbSpeed * player->ageProperties->unk_08;

            // A bottom-entry query can see only one segmented ladder polygon.
            // Refresh the upper bound when the next tagged section is visible.
            if (direction > 0 && actor->world.pos.y + 20.0f > sPilot.ladderTopY) {
                Vec3f nextAnchor = {};
                s16 nextYaw = 0;
                f32 nextBottom = 0.0f;
                f32 nextTop = 0.0f;
                if (ShipCrewPlayer_QueryLadder(play, player, false, &nextAnchor, &nextYaw, &nextBottom, &nextTop) &&
                    nextTop > sPilot.ladderTopY)
                    sPilot.ladderTopY = nextTop;
            }

            if (direction < 0) {
                Vec3f floorProbe = actor->world.pos;
                floorProbe.y += 12.0f;
                CollisionPoly* ground = nullptr;
                const f32 floorY = BgCheck_EntityRaycastFloor1(&play->colCtx, &ground, &floorProbe);
                if (ground != nullptr && actor->world.pos.y <= floorY + 14.0f &&
                    actor->world.pos.y < sPilot.ladderTopY - 25.0f) {
                    Vec3f landing = actor->world.pos;
                    landing.y = floorY;
                    Pilot_LadderDismount(player, play, false, landing);
                }
            } else {
                const f32 dirX = Math_SinS(sPilot.ladderYaw);
                const f32 dirZ = Math_CosS(sPilot.ladderYaw);
                Vec3f upperProbe = actor->world.pos;
                upperProbe.x += dirX * (player->ageProperties->wallCheckRadius + 13.0f);
                upperProbe.z += dirZ * (player->ageProperties->wallCheckRadius + 13.0f);
                upperProbe.y += player->ageProperties->unk_40;
                CollisionPoly* upperFloor = nullptr;
                const f32 upperY = BgCheck_EntityRaycastFloor1(&play->colCtx, &upperFloor, &upperProbe);
                if (upperFloor != nullptr && actor->world.pos.y >= upperY - 7.0f &&
                    upperY >= sPilot.ladderBottomY + 25.0f) {
                    Vec3f landing = upperProbe;
                    landing.y = upperY;
                    Pilot_LadderDismount(player, play, true, landing);
                } else if (actor->world.pos.y >= sPilot.ladderTopY - 3.0f &&
                           sPilot.ladderTopEntry.y >= sPilot.ladderTopY - 14.0f) {
                    // Return to the ORIGINAL platform when climbing back out
                    // of the ladder P2 entered from above.
                    Pilot_LadderDismount(player, play, true, sPilot.ladderTopEntry);
                }
            }
        }
    }
    Actor_SetFocus(actor, 40.0f);
    sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
    sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
    return true;
}

bool Pilot_TryLadder(Player* player, PlayState* play, const OSContPad& pad, bool enabled, bool canAct, bool moving,
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
    // Prefer the requested top descent: from ground directly above an
    // actual flagged ladder, P2 can press DOWN without falling first.
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && std::abs(pad.stick_y) > 25 &&
        ShipCrewPlayer_QueryLadder(play, player, true, &anchor, &yaw, &bottomY, &topY)) {
        Pilot_BeginLadder(player, play, true, anchor, yaw, bottomY, topY);
        return true;
    }
    if ((player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) && pad.stick_y > 25 &&
        ShipCrewPlayer_QueryLadder(play, player, false, &anchor, &yaw, &bottomY, &topY)) {
        Pilot_BeginLadder(player, play, false, anchor, yaw, bottomY, topY);
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
    // Player 1's autojump starts on BGCHECKFLAG_GROUND_LEAVE for a real drop,
    // with forward momentum and an unobstructed facing direction. Never
    // assign a manual jump button to P2.
    if (wasGrounded && !grounded && (actor->bgCheckFlags & BGCHECKFLAG_GROUND_LEAVE) &&
        sPilot.traversal != PilotTraversal::AutoJump && actor->speedXZ > 3.0f &&
        std::abs(static_cast<s32>(static_cast<s16>(actor->world.rot.y - actor->shape.rot.y))) < 0x2000 &&
        actor->world.pos.y - actor->floorHeight > 20.0f) {
        f32 jumpSpeed;
        if (player->linearVelocity > IREG(66) / 100.0f) {
            jumpSpeed = IREG(67) / 100.0f;
        } else {
            jumpSpeed = IREG(68) / 100.0f + IREG(69) * player->linearVelocity / 1000.0f;
        }
        Pilot_BeginJump(player, std::max(4.0f, jumpSpeed));
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
        Pilot_BeginJump(player, rise * 0.08f + 5.5f);
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
        }
    }
    if (sPilot.parallelRecenterFrames > 0)
        --sPilot.parallelRecenterFrames;
    if (!zHeld && sPilot.parallelTargeting && (holdTargeting || sPilot.parallelRecenterFrames == 0))
        sPilot.parallelTargeting = false;
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

    Actor* heldBomb = Pilot_FindHeldBomb(actor, play);
    // The original action button releases a carried bomb first. Otherwise A
    // rolls while running; there is no manual A-button jump.
    const bool nativeMovement = CVarGetInteger(SHIPCREW_NATIVE_LOCOMOTION_CVAR, 0) != 0;
    const bool nativeTraversal = nativeMovement && CVarGetInteger(SHIPCREW_NATIVE_TRAVERSAL_CVAR, 0) != 0;
    // One cooldown clock even when probing before AND after ground movement.
    if (sPilot.ladderCooldown > 0)
        --sPilot.ladderCooldown;
    if (!nativeTraversal &&
        (sPilot.traversal == PilotTraversal::Hanging || sPilot.traversal == PilotTraversal::Climbing ||
         sPilot.traversal == PilotTraversal::HighStepWindup)) {
        Pilot_ClearTraversal(actor, player);
    }
    if (!nativeTraversal && sPilot.ladder != PilotLadder::None)
        Pilot_ClearLadder(player);
    if (nativeTraversal && Pilot_UpdateLadder(player, play, pad, canAct)) {
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

    if (canAct && (pressed & BTN_A) && heldBomb != nullptr && sPilot.itemDebounceFrames == 0) {
        Pilot_ReleaseBomb(actor, heldBomb, moving);
        heldBomb = nullptr;
    } else if (canAct && (pressed & BTN_A) && heldBomb == nullptr && wasGrounded && moving && sPilot.rollFrames == 0 &&
               sPilot.itemFrames == 0 && sPilot.dodge == PilotDodge::None) {
        PilotDodge dodge = PilotDodge::None;
        if (nativeMovement && (sPilot.lockedTarget != nullptr || sPilot.parallelTargeting)) {
            const s16 facing = sPilot.lockedTarget != nullptr
                                   ? static_cast<s16>(
                                         std::atan2(sPilot.lockedTarget->focus.pos.x - actor->world.pos.x,
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
                SPDLOG_INFO("[ShipCrew] P2 Z dodge: direction={} hostile={} parallel={}",
                            static_cast<int>(dodge), sPilot.lockedTarget != nullptr, sPilot.parallelTargeting);
                sPilot.dodge = dodge;
                sPilot.dodgeLanding = false;
                const bool side = dodge != PilotDodge::Backflip;
                Pilot_BeginJump(player, side ? 3.5f : 5.8f);
                const s16 angle = dodge == PilotDodge::Backflip   ? static_cast<s16>(0x8000)
                                  : dodge == PilotDodge::SideLeft ? static_cast<s16>(0x4000)
                                                                  : static_cast<s16>(-0x4000);
                sPilot.dodgeYaw = static_cast<s16>(facing + angle);
                player->yaw = actor->world.rot.y = sPilot.dodgeYaw;
                actor->shape.rot.y = facing;
                actor->speedXZ = player->linearVelocity = side ? 8.5f : 6.0f;
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
                LinkAnimationHeader* roll = ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_landing_roll);
                LinkAnimation_PlayOnceSetSpeed(play, &player->skelAnime, roll, 1.25f);
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
            canAct && moving ? ShipCrewPlayer_CalcGroundSpeedTarget(stickMagnitude, speedLimit, player->floorPitch,
                                                                    !zMovement)
                             : 0.0f;
        if (sPilot.dodge != PilotDodge::None) {
            player->yaw = actor->world.rot.y = sPilot.dodgeYaw;
            if (sPilot.dodgeLanding)
                Math_StepToF(&player->linearVelocity, 0.0f, REG(43) / 100.0f);
            actor->speedXZ = std::max(player->linearVelocity, 0.0f);
        } else if (sPilot.rollFrames > 0) {
            if (sPilot.nativeRoll) {
                // Mirror P1 Player_Action_Roll's per-frame speed calculation:
                // curved stick speed * 1.5, minimum 3, original run step,
                // and movement fixed to Link's roll-facing yaw.
                if (player->skelAnime.curFrame < 20.0f) {
                    const f32 rollTarget = std::max(3.0f, ShipCrewPlayer_CalcGroundSpeedTarget(
                                                              stickMagnitude, speedLimit, player->floorPitch, true) *
                                                              1.5f);
                    ShipCrewPlayer_ApplyNativeRunMotion(player, rollTarget, sPilot.rollYaw);
                } else {
                    Math_StepToF(&player->linearVelocity, 0.0f, REG(43) / 100.0f);
                }
                actor->speedXZ = std::max(player->linearVelocity, 0.0f);
                actor->world.rot.y = player->yaw;
            } else {
                actor->speedXZ = kRollSpeed;
            }
        } else if (!wasGrounded && sPilot.traversal == PilotTraversal::AutoJump) {
            // Player 1's airborne func_8083DFE0 uses 0.05/0.1 velocity
            // changes and a 200-unit yaw step, not full ground acceleration.
            // Full ground acceleration every airborne frame made P2's
            // autojump violently re-steer as its own camera followed it.
            const s16 desiredYaw =
                moving ? static_cast<s16>(std::atan2(worldX, worldZ) * kRadiansToN64Angle) : player->yaw;
            const s16 yawDiff = player->yaw - desiredYaw;
            if (std::abs(static_cast<s32>(yawDiff)) > 0x6000) {
                if (Math_StepToF(&player->linearVelocity, 0.0f, 1.0f))
                    player->yaw = desiredYaw;
            } else {
                Math_AsymStepToF(&player->linearVelocity, moving ? nativeTarget : 0.0f, 0.05f, 0.1f);
                Math_ScaledStepToS(&player->yaw, desiredYaw, 200);
            }
            actor->speedXZ = std::max(0.0f, player->linearVelocity);
            actor->world.rot.y = player->yaw;
        } else if (!canAct || !moving) {
            // Vanilla standing uses the boot-dependent idle deceleration.
            Math_StepToF(&player->linearVelocity, 0.0f, REG(43) / 100.0f);
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
    if (nativeTraversal && wasGrounded &&
        Pilot_TryLadder(player, play, pad, nativeTraversal, canAct, moving, heldBomb != nullptr)) {
        Actor_SetFocus(actor, 40.0f);
        return;
    }
    const bool fallingBeforeMove = actor->velocity.y < -1.0f;
    Actor_MoveXZGravity(actor);
    if (nativeMovement && player->ageProperties != nullptr) {
        Actor_UpdateBgCheckInfo(play, actor, 26.0f, player->ageProperties->wallCheckRadius,
                                player->ageProperties->ceilingCheckHeight, 0x3F);
        if (actor->floorPoly != nullptr && (actor->bgCheckFlags & BGCHECKFLAG_GROUND)) {
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
    if (wasGrounded && !(actor->bgCheckFlags & BGCHECKFLAG_GROUND))
        sPilot.takeoffY = actor->world.pos.y;
    if (Pilot_TryLadder(player, play, pad, nativeTraversal, canAct, moving,
                        Pilot_FindHeldBomb(actor, play) != nullptr)) {
        Actor_SetFocus(actor, 40.0f);
        sPilot.lastObservedBombAmmo = AMMO(ITEM_BOMB);
        sPilot.lastObservedNutAmmo = AMMO(ITEM_NUT);
        return;
    }
    if (Pilot_TryTraversal(player, play, wasGrounded, canAct, moving, nativeTraversal,
                           Pilot_FindHeldBomb(actor, play) != nullptr)) {
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

    // Independent P2 skeleton, borrowing ORIGINAL P1 animation assets by
    // group and P2's own modelAnimType (equipment stance). Never read P1's
    // animation time or pose: both Links can locomote independently.
    LinkAnimationHeader* animation = nullptr;
    u8 mode = ANIMMODE_LOOP;
    bool locomotionLoop = false;
    const f32 nativeInputSpeed = nativeMovement && moving
                                     ? ShipCrewPlayer_CalcGroundSpeedTarget(
                                           80.0f * std::min(inputLength, 1.0f), player->unk_880, player->floorPitch,
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
        // Native jump startup retains forward momentum; after the apex use
        // the original falling-loop pose, rather than restarting jump every
        // frame. No manual A-button jump is introduced.
        if (nativeTraversal && actor->velocity.y < 0.0f) {
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
    } else if (heldBomb != nullptr) {
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
        LinkAnimation_Change(play, &player->skelAnime, animation, 1.0f, 0.0f, Animation_GetLastFrame(animation), mode,
                             -3.0f);
    }
    // Native Link's gait speed follows locomotion rather than replaying one
    // fixed-rate walk/run loop across every analog-stick magnitude.
    if (nativeMovement && locomotionLoop) {
        player->skelAnime.playSpeed = std::clamp(actor->speedXZ / (running ? 5.0f : 2.0f), 0.6f, 2.0f);
    }
    const bool animationFinished = LinkAnimation_Update(play, &player->skelAnime) != 0;
    if (sPilot.dodge != PilotDodge::None && sPilot.dodgeLanding && animationFinished) {
        sPilot.dodge = PilotDodge::None;
        sPilot.dodgeLanding = false;
    }

    if (sPilot.rollFrames > 0) {
        if (sPilot.nativeRoll) {
            if (!sPilot.rollInvulnStarted && player->skelAnime.curFrame >= 8.0f) {
                Player_SetInvulnerability(player, -10);
                sPilot.rollInvulnStarted = true;
            }
            if (player->skelAnime.curFrame >= 20.0f ||
                player->skelAnime.animation !=
                    ShipCrewPlayer_GetGroupAnimation(player, PLAYER_ANIMGROUP_landing_roll)) {
                sPilot.nativeRoll = false;
                sPilot.rollFrames = 0;
            }
        } else {
            --sPilot.rollFrames;
        }
    }
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
    return sPilot.parallelTargeting;
}

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
