/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PriestActions.h"
#include "Event.h"
#include "Playerbots.h"

bool CastRemoveShadowformAction::Execute(Event /*event*/)
{
    botAI->RemoveAura("shadowform");
    return true;
}

bool CastRemoveShadowformAction::isUseful() { return botAI->HasAura("shadowform", AI_VALUE(Unit*, "self target")); }

Unit* CastPowerWordShieldOnAlmostFullHealthBelowAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        if (!bot->isDead() && bot->GetHealthPct() <= sPlayerbotAIConfig.almostFullHealth &&
            !botAI->HasAnyAuraOf(bot, "weakened soul", "power word: shield", nullptr))
        {
            return bot;
        }
        return nullptr;
    }

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || player->isDead())
            continue;

        if (player->GetHealthPct() > sPlayerbotAIConfig.almostFullHealth)
            continue;

        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.spellDistance)
            continue;

        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
            continue;

        return player;
    }
    return nullptr;
}

bool CastPowerWordShieldOnAlmostFullHealthBelowAction::isUseful()
{
    return GetTarget() != nullptr;
}

Unit* CastPowerWordShieldOnNotFullAction::GetTarget()
{
    Group* group = bot->GetGroup();
    if (!group)
    {
        if (!bot->isDead() && !bot->IsFullHealth() &&
            !botAI->HasAnyAuraOf(bot, "weakened soul", "power word: shield", nullptr))
        {
            return bot;
        }
        return nullptr;
    }

    MinValueCalculator calc(100);
    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || player->isDead() || player->IsFullHealth())
            continue;

        if (player->GetDistance2d(bot) > sPlayerbotAIConfig.spellDistance)
            continue;

        if (botAI->HasAnyAuraOf(player, "weakened soul", "power word: shield", nullptr))
            continue;

        calc.probe(player->GetHealthPct(), player);
    }
    return (Unit*)calc.param;
}

bool CastPowerWordShieldOnNotFullAction::isUseful()
{
    return GetTarget() != nullptr;
}

bool CastFadeAction::isUseful()
{
    if (!bot->GetGroup() || !CastBuffSpellAction::isUseful())
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return !(target && target->IsPlayer());
}
