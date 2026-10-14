/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

/*
 * Ported from the CMaNGOS playerbots project (https://github.com/cmangos/playerbots), GPL v2,
 * with modifications for AzerothCore.
 * Original authors:
 *   David Parra Ausina (davidonete/Flekz) <davidparraausina@gmail.com> - original author
 *   Sebastiaan Keek (mostlikely4r) <sebastiaan.keek@gmail.com>
 *   Cyberium <cyberium@users.noreply.github.com>
 */

#include "PullTriggers.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"
#include "PositionValue.h"
#include "PullStrategy.h"

bool PullStartTrigger::IsActive()
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);
    return strategy && strategy->IsPullPendingToStart();
}

bool PullEndTrigger::IsActive()
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);

    if (!strategy || !strategy->HasPullStarted())
        return false;

    Unit* target = strategy->GetTarget();
    if (!target || !target->IsInWorld() || !target->IsAlive())
        return true;

    time_t const secondsSincePullStarted = time(nullptr) - strategy->GetPullStartTime();
    if (secondsSincePullStarted >= PullStrategy::GetMaxPullTime())
        return true;

    float distanceToPullTarget = bot->GetDistance(target);
    if (distanceToPullTarget > ATTACK_DISTANCE && !target->IsNonMeleeSpellCast(false, false, true) &&
        (!botAI->IsRanged(bot) || distanceToPullTarget > botAI->GetRange("spell")))
        return false;

    if (!botAI->HasStrategy("pull back", BOT_STATE_COMBAT))
        return true;

    PositionInfo pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    if (!pullPosition.isSet() || pullPosition.mapId != bot->GetMapId())
        return true;

    return bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z) <= botAI->GetRange("follow");
}

bool ReturnToPullPositionTrigger::IsActive()
{
    PullStrategy const* strategy = PullStrategy::Get(botAI);

    Unit* target = strategy ? strategy->GetTarget() : nullptr;
    if (!strategy || !strategy->HasPullStarted() || !target || !target->IsInCombat() ||
        !botAI->HasStrategy("pull back", BOT_STATE_COMBAT))
        return false;

    PositionInfo pullPosition = AI_VALUE(PositionMap&, "position")["pull"];
    return pullPosition.isSet() && pullPosition.mapId == bot->GetMapId() &&
           bot->GetDistance(pullPosition.x, pullPosition.y, pullPosition.z) > sPlayerbotAIConfig.followDistance;
}

bool ReadyToPullTrigger::IsActive()
{
    if (!bot || !botAI || !bot->IsAlive())
        return false;

    if (!PlayerbotAI::IsTank(bot, true))
        return false;

    if (bot->IsInCombat() || !AI_VALUE(std::vector<Unit*>, "attackers").empty())
        return false;

    PullStrategy* strategy = PullStrategy::Get(botAI);
    if (!strategy || strategy->HasTarget() || strategy->IsPullPendingToStart())
        return false;

    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
        {
            Player* member = gref->GetSource();
            if (!member || !member->IsAlive())
                return false;

            if (member->GetHealthPct() < sPlayerbotAIConfig.almostFullHealth)
                return false;

            if (member->GetMaxPower(POWER_MANA) > 0 && member->GetPowerPct(POWER_MANA) < sPlayerbotAIConfig.mediumHealth)
            {
                if (PlayerbotAI::IsHeal(member) || member->getClass() == CLASS_MAGE ||
                    member->getClass() == CLASS_PRIEST || member->getClass() == CLASS_WARLOCK)
                    return false;
            }

            if (member->GetMapId() != bot->GetMapId() || bot->GetDistance(member) > sPlayerbotAIConfig.sightDistance)
                return false;
        }
    }

    // Must have a viable pull target
    Unit* target = nullptr;
    if (Group* group = bot->GetGroup())
    {
        for (int32 icon : {7, 6})
        {
            ObjectGuid const guid = group->GetTargetIcon(icon);
            if (guid)
            {
                Unit* u = botAI->GetUnit(guid);
                if (u && u->IsAlive() && !u->IsPlayer() && bot->IsWithinLOSInMap(u) &&
                    bot->GetDistance(u) <= sPlayerbotAIConfig.sightDistance &&
                    !u->HasBreakableByDamageCrowdControlAura())
                {
                    target = u;
                    break;
                }
            }
        }
    }
    if (!target)
    {
        target = AI_VALUE(Unit*, "grind target");
        if (!target || !target->IsAlive() || target->IsPlayer() || !bot->IsWithinLOSInMap(target) ||
            bot->GetDistance(target) > sPlayerbotAIConfig.sightDistance ||
            target->HasBreakableByDamageCrowdControlAura())
        {
            target = AI_VALUE(Unit*, "dps target");
        }
    }

    return target != nullptr && target->IsAlive() && !target->IsPlayer() &&
           bot->IsWithinLOSInMap(target) &&
           bot->GetDistance(target) <= sPlayerbotAIConfig.sightDistance &&
           !target->HasBreakableByDamageCrowdControlAura();
}
