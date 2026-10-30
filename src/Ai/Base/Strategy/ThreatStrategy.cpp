/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ThreatStrategy.h"
#include "GenericSpellActions.h"
#include "Group.h"
#include "Map.h"
#include "Playerbots.h"

float ThreatMultiplier::GetValue(Action* action)
{
    if (AI_VALUE(bool, "neglect threat"))
    {
        return 1.0f;
    }

    if (!action || action->getThreatType() == Action::ActionThreatType::None)
        return 1.0f;

    if (!AI_VALUE(bool, "group"))
        return 1.0f;

    Player* bot = botAI->GetBot();
    // Project Jaina Threat Lead Time: Strictly inside PvE Dungeon/Raid instances against non-player
    // creatures, and ONLY when the group has an alive, nearby Tank, prevent non-tank DPS from dropping
    // immediate heavy AoE or fresh-pull nukes before the tank establishes baseline threat.
    // Never throttle in Open World, Battlegrounds, Arenas, PvP targets, or tankless groups.
    if (bot && !botAI->IsTank(bot) && bot->GetMap() && bot->GetMap()->IsDungeon() &&
        !bot->InBattleground() && !bot->InArena())
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive() && !target->IsPlayer())
        {
            bool groupHasActiveTank = false;
            if (Group* group = bot->GetGroup())
            {
                for (GroupReference* ref = group->GetFirstMember(); ref != nullptr; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && member != bot && member->IsInWorld() && member->IsAlive() &&
                        member->GetMapId() == bot->GetMapId() &&
                        bot->GetDistance2d(member) <= sPlayerbotAIConfig.sightDistance &&
                        botAI->IsTank(member))
                    {
                        groupHasActiveTank = true;
                        break;
                    }
                }
            }

            if (groupHasActiveTank)
            {
                Unit* victim = target->GetVictim();
                bool tankHasTarget = victim && victim->ToPlayer() && botAI->IsTank(victim->ToPlayer());
                if (!tankHasTarget)
                {
                    // Disallow AoE spells until the tank securely holds the primary target
                    if (action->getThreatType() == Action::ActionThreatType::Aoe)
                        return 0.0f;

                    // For single target spells, throttle if target hasn't engaged anyone yet (fresh pull)
                    if (action->getThreatType() == Action::ActionThreatType::Single && !victim)
                    {
                        return 0.0f;
                    }
                }
            }
        }
    }

    if (action->getThreatType() == Action::ActionThreatType::Aoe)
    {
        uint8 threat = AI_VALUE2(uint8, "threat", "aoe");
        if (threat >= 50)
            return 0.0f;
    }

    uint8 threat = AI_VALUE2(uint8, "threat", "current target");
    if (threat >= 80)
        return 0.0f;

    return 1.0f;
}

void ThreatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new ThreatMultiplier(botAI));
}

float FocusMultiplier::GetValue(Action* action)
{
    if (!action)
    {
        return 1.0f;
    }
    if (action->getThreatType() == Action::ActionThreatType::Aoe && !dynamic_cast<CastHealingSpellAction*>(action))
    {
        return 0.0f;
    }
    if (dynamic_cast<CastDebuffSpellOnAttackerAction*>(action))
    {
        return 0.0f;
    }
    return 1.0f;
}

void FocusStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    multipliers.push_back(new FocusMultiplier(botAI));
}
