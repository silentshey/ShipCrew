// Opt-in, deliberately limited first integration test for a second local Link.
// Independent P2 movement, empty-handed idle/run and experimental world physics.
// This is not the final multiplayer actor/combat/camera architecture.

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

extern PlayState* gPlayState;
void Player_UseItem(PlayState* play, Player* player, s32 item);
void Player_Draw(Actor* actor, PlayState* play);
}

// The experimental switch lives in the existing Controls settings screen.
// Leave disabled by default so the already validated single-player build is unchanged.
#define SHIPCREW_PILOT_CVAR CVAR_SETTING("ShipCrew.TwoLinkPilot")

namespace {

constexpr f32 kMaxStickValue = 80.0f;
constexpr f32 kMovementPerFrame = 3.0f;
constexpr f32 kSpawnSeparation = 70.0f;
constexpr f32 kPilotGravity = -1.0f;
constexpr f32 kPilotTerminalVelocity = -18.0f;
constexpr f32 kPilotHopVelocity = 8.0f;
// World collisions only; player/NPC combat and interactions are later milestones.
constexpr f32 kWallCheckHeight = 50.0f;
constexpr f32 kWallCheckRadius = 22.0f;
constexpr f32 kCeilingCheckHeight = 55.0f;
constexpr f32 kRadiansToN64Angle = 32768.0f / 3.14159265358979323846f;

bool sSpawningLocalPilot = false;
bool sSpawnAttempted = false;

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

    NameTag_RegisterForActorWithOptions(actor, "P2 PILOT", {});
}

void Pilot_Update(Actor* actor, PlayState* play) {
    Player* player = reinterpret_cast<Player*>(actor);
    Player* mainPlayer = GET_PLAYER(play);
    if (mainPlayer == nullptr || player == mainPlayer) {
        return;
    }

    // The existing input deck already reads up to four independent N64 ports.
    // IMPORTANT: never swap input[0] or GET_PLAYER to impersonate P2: that would
    // corrupt P1's actions and hide the real multi-player architecture work.
    const auto& pad = play->state.input[1].cur;
    const f32 x = static_cast<f32>(pad.stick_x) / kMaxStickValue;
    const f32 z = static_cast<f32>(pad.stick_y) / kMaxStickValue;
    const f32 length = std::sqrt(x * x + z * z);
    const bool moving = length > 0.17f;
    // Use the engine's actor movement so its background collision receives
    // correct previous/current positions, instead of directly teleporting X/Z.
    actor->speedXZ = moving ? kMovementPerFrame * (length > 1.0f ? 1.0f : length) : 0.0f;
    if (moving) {
        actor->world.rot.y = static_cast<s16>(std::atan2(x, z) * kRadiansToN64Angle);
        actor->shape.rot.y = actor->world.rot.y;
    }

    // This A-button hop only probes independent input, gravity and floor
    // landing. It is NOT the game's eventual ledge auto-jump system.
    if ((play->state.input[1].press.button & BTN_A) && (actor->bgCheckFlags & BGCHECKFLAG_GROUND)) {
        actor->velocity.y = kPilotHopVelocity;
        actor->bgCheckFlags &= ~BGCHECKFLAG_GROUND;
    }
    Actor_MoveXZGravity(actor);
    // Regular engine checks: resolve world walls, ceilings and floor contact.
    // Actor-vs-actor interaction and specialized Link ledge physics are separate.
    Actor_UpdateBgCheckInfo(play, actor, kWallCheckHeight, kWallCheckRadius, kCeilingCheckHeight, 0x1D);
    Actor_SetFocus(actor, 40.0f);

    // P2 has no independent item/equip actions yet: Player_UseItem(ITEM_NONE)
    // initializes the pilot's default model group with empty hands. Use the
    // corresponding free-arm loops, not the shield/weapon-ready animations.
    // This depends ONLY on P2 input and never mirrors P1's active animation.
    // Once P2 equipment exists, choose from the game's anim group/model type.
    LinkAnimationHeader* animation = reinterpret_cast<LinkAnimationHeader*>(
        const_cast<char*>(moving ? gPlayerAnim_link_normal_run_free : gPlayerAnim_link_normal_wait_free));
    if (player->skelAnime.animation != animation) {
        LinkAnimation_Change(play, &player->skelAnime, animation, 1.0f, 0.0f, Animation_GetLastFrame(animation),
                             ANIMMODE_LOOP, -4.0f);
    }
    LinkAnimation_Update(play, &player->skelAnime);
    player->upperLimbRot = { 0, 0, 0 };
    player->currentTunic = mainPlayer->currentTunic;
    player->currentBoots = mainPlayer->currentBoots;
    player->currentShield = mainPlayer->currentShield;
}

void Pilot_Draw(Actor* actor, PlayState* play) {
    Player_Draw(actor, play);
}

void Pilot_Destroy(Actor* actor, PlayState* play) {
    NameTag_RemoveAllForActor(actor);
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
