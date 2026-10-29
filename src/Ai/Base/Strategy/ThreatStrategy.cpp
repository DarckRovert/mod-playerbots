/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ThreatStrategy.h"
#include "GenericSpellActions.h"
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
    // Project Jaina Threat Lead Time: On newly engaged targets or fresh multi-mob pulls,
    // prevent non-tank DPS from dropping immediate heavy AoE or massive nukes before the tank
    // establishes baseline threat (preventing instant one-shots on casters and wiping on adds).
    if (bot && !botAI->IsTank(bot))
    {
        Unit* target = AI_VALUE(Unit*, "current target");
        if (target && target->IsAlive())
        {
            Unit* victim = target->GetVictim();
            // If the target is not attacking our group tank, or has zero victim, require caution
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
