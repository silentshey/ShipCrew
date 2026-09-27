// Opt-in, deliberately limited first integration test for a second local Link.
// Experimental single-process P2 movement + shared-inventory item integration.
// P2 remains a separate NPC-category Link actor, not the global GET_PLAYER.

#include <algorithm>
#include <cmath>

#include <libultraship/bridge/consolevariablebridge.h>

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

// This runtime belongs to the one experimental P2 actor; never borrow P1's
// action state or animation tables. When expanded to 4 players, move this
// runtime into the common per-player component keyed by local player slot.
enum class PilotItemPose { None, Bomb, Nut };
struct PilotRuntime {
    Actor* actor = nullptr;
    int rollFrames = 0;
    int landingFrames = 0;
    int itemFrames = 0;
    PilotItemPose itemPose = PilotItemPose::None;
};
PilotRuntime sPilot;

LinkAnimationHeader* Pilot_Animation(const char* asset) {
    return reinterpret_cast<LinkAnimationHeader*>(const_cast<char*>(asset));
}

// Item actors are the normal game actors, so they affect the same world.
// Only consume the shared save's ammo AFTER the actor actually spawned.
void Pilot_UseBomb(Actor* actor, PlayState* play) {
    if (AMMO(ITEM_BOMB) <= 0 || (play->actorCtx.actorLists[ACTORCAT_EXPLOSIVE].length >= kMaxWorldBombs &&
                                 CVarGetInteger(CVAR_ENHANCEMENT("RemoveExplosiveLimit"), 0) == 0)) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    const f32 radians = static_cast<f32>(actor->shape.rot.y) / kRadiansToN64Angle;
    Actor* bomb = Actor_Spawn(&play->actorCtx, play, ACTOR_EN_BOM,
                              actor->world.pos.x + std::sin(radians) * kItemDropDistance, actor->world.pos.y + 7.0f,
                              actor->world.pos.z + std::cos(radians) * kItemDropDistance, 0, actor->shape.rot.y, 0, 0);
    if (bomb != nullptr) {
        Inventory_ChangeAmmo(ITEM_BOMB, -1);
        sPilot.itemPose = PilotItemPose::Bomb;
        sPilot.itemFrames = kItemFrames;
    }
}

void Pilot_UseNut(Actor* actor, PlayState* play) {
    if (AMMO(ITEM_NUT) <= 0) {
        Sfx_PlaySfxCentered(NA_SE_SY_ERROR);
        return;
    }
    const f32 radians = static_cast<f32>(actor->shape.rot.y) / kRadiansToN64Angle;
    Actor* nut =
        Actor_Spawn(&play->actorCtx, play, ACTOR_EN_ARROW, actor->world.pos.x + std::sin(radians) * kItemDropDistance,
                    actor->world.pos.y + 35.0f, actor->world.pos.z + std::cos(radians) * kItemDropDistance, 0x1000,
                    actor->shape.rot.y, 0, ARROW_NUT);
    if (nut != nullptr) {
        Inventory_ChangeAmmo(ITEM_NUT, -1);
        sPilot.itemPose = PilotItemPose::Nut;
        sPilot.itemFrames = kItemFrames;
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

    // Player 2 reads only port 2. Never redirect GET_PLAYER or input[0]:
    // the save, scene, game clock and inventory remain intentionally shared.
    const auto& pad = play->state.input[1].cur;
    const auto pressed = play->state.input[1].press.button;
    const f32 x = static_cast<f32>(pad.stick_x) / kMaxStickValue;
    const f32 z = static_cast<f32>(pad.stick_y) / kMaxStickValue;
    const f32 inputLength = std::sqrt(x * x + z * z);
    const bool moving = inputLength > 0.17f;
    const bool wasGrounded = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    const bool canAct = !Player_InBlockingCsMode(play, GET_PLAYER(play));

    if (canAct && (pressed & BTN_B) && wasGrounded && moving && sPilot.rollFrames == 0 && sPilot.itemFrames == 0) {
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
            actor->world.rot.y = static_cast<s16>(std::atan2(x, z) * kRadiansToN64Angle);
            actor->shape.rot.y = actor->world.rot.y;
        }
    }

    const bool fallingBeforeMove = actor->velocity.y < -1.0f;
    Actor_MoveXZGravity(actor);
    Actor_UpdateBgCheckInfo(play, actor, kWallCheckHeight, kWallCheckRadius, kCeilingCheckHeight, 0x1D);
    Actor_SetFocus(actor, 40.0f);
    const bool grounded = (actor->bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    if (!wasGrounded && grounded && fallingBeforeMove) {
        sPilot.landingFrames = kLandingFrames;
    }

    // Pilot items are inventory-gated and intentionally use already existing
    // bomb / nut actors. P2 has no full equipment or item action system yet.
    if (canAct && grounded && sPilot.rollFrames == 0 && sPilot.itemFrames == 0) {
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
        animation = Pilot_Animation(sPilot.itemPose == PilotItemPose::Bomb ? gPlayerAnim_link_normal_put_free
                                                                           : gPlayerAnim_link_normal_light_bom);
        mode = ANIMMODE_ONCE;
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
}

void Pilot_Draw(Actor* actor, PlayState* play) {
    Player_Draw(actor, play);
}

void Pilot_Destroy(Actor* actor, PlayState* play) {
    NameTag_RemoveAllForActor(actor);
    if (sPilot.actor == actor) {
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
