/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ReachTargetActions.h"
#include "Event.h"
#include "Group.h"
#include "GroupReference.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"

bool ReachTargetAction::Execute(Event /*event*/) { return ReachCombatTo(AI_VALUE(Unit*, GetTargetName()), distance); }

bool ReachTargetAction::isUseful()
{
    // do not move while staying
    if (botAI->HasStrategy("stay", botAI->GetState()))
    {
        return false;
    }

    // do not move while casting
    if (bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr)
    {
        return false;
    }
    Unit* target = GetTarget();
    if (!target || bot->IsWithinCombatRange(target, distance))
        return false;

    return !(ChasesEnemy() && IsChaseLeashed(target));
}

Unit* ReachTargetAction::ChaseAnchor() const
{
    if (!PlayerbotAI::IsTank(bot))
        return AI_VALUE(Unit*, "main tank");

    Unit* healer = nullptr;
    Unit* nearest = nullptr;
    if (Group* group = bot->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
                continue;

            if (PlayerbotAI::IsHeal(member) && (!healer || bot->GetExactDist(member) < bot->GetExactDist(healer)))
                healer = member;
            if (!nearest || bot->GetExactDist(member) < bot->GetExactDist(nearest))
                nearest = member;
        }
    }

    return healer ? healer : nearest;
}

bool ReachTargetAction::IsChaseLeashed(Unit* target) const
{
    if (!sPlayerbotAIConfig.jainaChaseLeash || !target || !bot->GetGroup() || !bot->IsInCombat() ||
        !bot->GetMap()->IsDungeon())
        return false;

    if (PlayerbotAI::IsTank(bot) && !target->GetVictim())
        return false;

    Unit* anchor = ChaseAnchor();
    if (!anchor || anchor == bot || !anchor->IsAlive() || anchor->GetMapId() != bot->GetMapId())
        return false;

    return anchor->GetExactDist(target) > botAI->GetRange("heal");
}

std::string const ReachTargetAction::GetTargetName() { return "current target"; }

bool CastReachTargetSpellAction::isUseful()
{
    // do not move while staying
    if (botAI->HasStrategy("stay", botAI->GetState()))
    {
        return false;
    }

    return ServerFacade::instance().IsDistanceGreaterThan(AI_VALUE2(float, "distance", "current target"),
                                                (distance + sPlayerbotAIConfig.contactDistance));
}

ReachSpellAction::ReachSpellAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach spell", botAI->GetRange("spell"))
{
}

ReachPartyMemberToHealAction::ReachPartyMemberToHealAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach party member to heal", botAI->GetRange("heal"))
{
}

std::string const ReachPartyMemberToHealAction::GetTargetName() { return "party member to heal"; }

ReachPartyMemberToResurrectAction::ReachPartyMemberToResurrectAction(PlayerbotAI* botAI)
    : ReachTargetAction(botAI, "reach party member to resurrect", botAI->GetRange("spell"))
{
}

std::string const ReachPartyMemberToResurrectAction::GetTargetName() { return "party member to resurrect"; }
