/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DpsTargetValue.h"
#include "AttackersValue.h"
#include "Group.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "ServerFacade.h"
#include "Strategy.h"

namespace
{
    float StickyLifeTime(Unit* unit, Unit* currentTarget, float lifeTime)
    {
        return sPlayerbotAIConfig.jainaStickyTarget && unit == currentTarget ? lifeTime * 0.75f : lifeTime;
    }

    float StickyDistance(Unit* unit, Unit* currentTarget, float distance)
    {
        return sPlayerbotAIConfig.jainaStickyTarget && unit == currentTarget ? distance - 5.0f : distance;
    }

    float StickyDistance(Player* bot, Unit* unit, Unit* currentTarget)
    {
        float const distance = bot->GetDistance(unit);
        return StickyDistance(unit, currentTarget, distance);
    }
}  // namespace

class DpsFindTargetStrategy : public FindTargetStrategy
{
public:
    DpsFindTargetStrategy(PlayerbotAI* botAI) : FindTargetStrategy(botAI) {}

    TargetValueExclusionType GetExclusionType() override { return TargetValueExclusionType::Dps; }

    bool IsCrowdControlled(Unit* attacker) const
    {
        return sPlayerbotAIConfig.enableSquadTacticalCoordination && attacker->HasBreakableByDamageCrowdControlAura();
    }
};

class FindMaxThreatGapTargetStrategy : public DpsFindTargetStrategy
{
public:
    FindMaxThreatGapTargetStrategy(PlayerbotAI* botAI) : DpsFindTargetStrategy(botAI), minThreat(0) {}

    void CheckAttacker(Unit* attacker, ThreatManager* threatMgr) override
    {
        if (!attacker->IsAlive())
            return;

        if (CheckPriority(attacker))
            return;

        if (!result || CalcThreatGap(attacker, threatMgr) > CalcThreatGap(result, &result->GetThreatMgr()))
            result = attacker;
    }
    float CalcThreatGap(Unit* attacker, ThreatManager* threatMgr)
    {
        Unit* victim = attacker->GetVictim();
        return threatMgr->GetThreat(victim) - threatMgr->GetThreat(attacker);
    }

protected:
    float minThreat;
};

// caster
class CasterFindTargetSmartStrategy : public DpsFindTargetStrategy
{
public:
    CasterFindTargetSmartStrategy(PlayerbotAI* botAI, float dps)
        : DpsFindTargetStrategy(botAI), dps_(dps), targetExpectedLifeTime(1000000),
          currentTarget(botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
    {
        result = nullptr;
    }

    void CheckAttacker(Unit* attacker, ThreatManager* /*threatMgr*/) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }
        if (!attacker->IsAlive() || IsCrowdControlled(attacker))
            return;

        if (CheckPriority(attacker))
            return;

        float expectedLifeTime = attacker->GetHealth() / dps_;
        // Unit* victim = attacker->GetVictim();
        if (!result || IsBetter(attacker, result))
        {
            targetExpectedLifeTime = expectedLifeTime;
            result = attacker;
        }
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        float new_time = new_unit->GetHealth() / dps_;
        float old_time = old_unit->GetHealth() / dps_;
        // [5-30] > (5-0] > (20-inf)
        int new_level = GetIntervalLevel(new_unit);
        int old_level = GetIntervalLevel(old_unit);
        if (new_level != old_level)
            return new_level > old_level;

        int32_t level = new_level;
        if (level % 10 == 2 || level % 10 == 0)
            return StickyLifeTime(new_unit, currentTarget, new_time) <
                   StickyLifeTime(old_unit, currentTarget, old_time);
        // dont switch targets when all of them with low health
        if (currentTarget == new_unit)
            return true;

        if (currentTarget == old_unit)
            return false;

        return new_time > old_time;
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        float time = unit->GetHealth() / dps_;
        float dis = unit->GetDistance(botAI->GetBot());
        float attackRange =
            botAI->IsRanged(botAI->GetBot()) ? sPlayerbotAIConfig.spellDistance : sPlayerbotAIConfig.meleeDistance;
        attackRange += 5.0f;
        if (sPlayerbotAIConfig.jainaStickyTarget && unit == currentTarget)
            attackRange += 5.0f;
        int level = dis < attackRange ? 10 : 0;
        if (time >= 5 && time <= 30)
            return level + 2;

        if (time > 30)
            return level;

        return level + 1;
    }

protected:
    float dps_;
    float targetExpectedLifeTime;
    Unit* currentTarget;
};

// General
class GeneralFindTargetSmartStrategy : public DpsFindTargetStrategy
{
public:
    GeneralFindTargetSmartStrategy(PlayerbotAI* botAI, float dps)
        : DpsFindTargetStrategy(botAI), dps_(dps), targetExpectedLifeTime(1000000),
          currentTarget(botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
    {
        result = nullptr;
    }

    void CheckAttacker(Unit* attacker, ThreatManager*) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }
        if (!attacker->IsAlive() || IsCrowdControlled(attacker))
            return;

        if (CheckPriority(attacker))
            return;

        float expectedLifeTime = attacker->GetHealth() / dps_;
        // Unit* victim = attacker->GetVictim();
        if (!result || IsBetter(attacker, result))
        {
            targetExpectedLifeTime = expectedLifeTime;
            result = attacker;
        }
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        float new_time = new_unit->GetHealth() / dps_;
        float old_time = old_unit->GetHealth() / dps_;
        int new_level = GetIntervalLevel(new_unit);
        int old_level = GetIntervalLevel(old_unit);
        if (new_level != old_level)
        {
            return new_level > old_level;
        }
        // attack enemy in range and with lowest health
        int level = new_level;
        if (level == 10)
            return StickyLifeTime(new_unit, currentTarget, new_time) <
                   StickyLifeTime(old_unit, currentTarget, old_time);

        // all targets are far away, choose the closest one
        return StickyDistance(new_unit, currentTarget, botAI->GetBot()->GetDistance(new_unit)) <
               StickyDistance(old_unit, currentTarget, botAI->GetBot()->GetDistance(old_unit));
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        float dis = unit->GetDistance(botAI->GetBot());
        float attackRange =
            botAI->IsRanged(botAI->GetBot()) ? sPlayerbotAIConfig.spellDistance : sPlayerbotAIConfig.meleeDistance;
        attackRange += 5.0f;
        if (sPlayerbotAIConfig.jainaStickyTarget && unit == currentTarget)
            attackRange += 5.0f;
        int level = dis < attackRange ? 10 : 0;
        return level;
    }

protected:
    float dps_;
    float targetExpectedLifeTime;
    Unit* currentTarget;
};

// combo
class ComboFindTargetSmartStrategy : public DpsFindTargetStrategy
{
public:
    ComboFindTargetSmartStrategy(PlayerbotAI* botAI, float dps)
        : DpsFindTargetStrategy(botAI), dps_(dps), targetExpectedLifeTime(1000000),
          currentTarget(botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get())
    {
        result = nullptr;
    }

    void CheckAttacker(Unit* attacker, ThreatManager*) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }
        if (!attacker->IsAlive() || IsCrowdControlled(attacker))
            return;

        if (CheckPriority(attacker))
            return;

        float expectedLifeTime = attacker->GetHealth() / dps_;
        // Unit* victim = attacker->GetVictim();
        if (!result || IsBetter(attacker, result))
        {
            targetExpectedLifeTime = expectedLifeTime;
            result = attacker;
        }
    }
    bool IsBetter(Unit* new_unit, Unit* old_unit)
    {
        float new_time = new_unit->GetHealth() / dps_;
        float old_time = old_unit->GetHealth() / dps_;
        // [5-20] > (5-0] > (20-inf)
        int new_level = GetIntervalLevel(new_unit);
        int old_level = GetIntervalLevel(old_unit);
        if (new_level != old_level)
            return new_level > old_level;

        // attack enemy in range and with lowest health
        int level = new_level;
        Player* bot = botAI->GetBot();
        if (level == 10)
        {
            Unit* combo_unit = bot->GetComboTarget();
            if (new_unit == combo_unit)
                return true;

            return StickyLifeTime(new_unit, currentTarget, new_time) <
                   StickyLifeTime(old_unit, currentTarget, old_time);
        }
        // all targets are far away, choose the closest one
        return StickyDistance(new_unit, currentTarget, bot->GetDistance(new_unit)) <
               StickyDistance(old_unit, currentTarget, bot->GetDistance(old_unit));
    }
    int32_t GetIntervalLevel(Unit* unit)
    {
        float dis = unit->GetDistance(botAI->GetBot());
        float attackRange =
            botAI->IsRanged(botAI->GetBot()) ? sPlayerbotAIConfig.spellDistance : sPlayerbotAIConfig.meleeDistance;
        attackRange += 5.0f;
        if (sPlayerbotAIConfig.jainaStickyTarget && unit == currentTarget)
            attackRange += 5.0f;
        int level = dis < attackRange ? 10 : 0;
        return level;
    }

protected:
    float dps_;
    float targetExpectedLifeTime;
    Unit* currentTarget;
};

Unit* DpsTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti)
        return rti;

    // Pillar III: Squad Tactical Coordination (Skull/Cross Focus & Tank Assist)
    if (sPlayerbotAIConfig.enableSquadTacticalCoordination)
    {
        if (Group* group = bot->GetGroup())
        {
            // 1. Raid Target Icon Priority: Skull (7) then Cross (6)
            for (int32 icon : {7, 6})
            {
                ObjectGuid iconGuid = group->GetTargetIcon(icon);
                if (iconGuid)
                {
                    Unit* markedUnit = botAI->GetUnit(iconGuid);
                    if (markedUnit && markedUnit->IsAlive() && AttackersValue::IsValidTarget(markedUnit, bot) &&
                        bot->IsWithinLOSInMap(markedUnit) &&
                        ServerFacade::instance().GetDistance2d(bot, markedUnit) <= sPlayerbotAIConfig.sightDistance)
                    {
                        return markedUnit;
                    }
                }
            }

            // 2. Tank Assist: Focus fire on the group tank's target
            for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && member->IsInWorld() && member->IsAlive() && member != bot && PlayerbotAI::IsTank(member))
                {
                    Unit* tankVictim = member->GetVictim();
                    if (tankVictim && tankVictim->IsAlive() && AttackersValue::IsValidTarget(tankVictim, bot) &&
                        bot->IsWithinLOSInMap(tankVictim) &&
                        ServerFacade::instance().GetDistance2d(bot, tankVictim) <= sPlayerbotAIConfig.sightDistance &&
                        !tankVictim->HasBreakableByDamageCrowdControlAura())
                    {
                        return tankVictim;
                    }
                }
            }

            // 3. Master Assist: If no group tank target found, assist the party leader / master
            if (Player* master = botAI->GetMaster())
            {
                if (master->IsInWorld() && master->IsAlive() && master != bot)
                {
                    Unit* masterVictim = master->GetVictim();
                    if (masterVictim && masterVictim->IsAlive() && AttackersValue::IsValidTarget(masterVictim, bot) &&
                        bot->IsWithinLOSInMap(masterVictim) &&
                        ServerFacade::instance().GetDistance2d(bot, masterVictim) <= sPlayerbotAIConfig.sightDistance &&
                        !masterVictim->HasBreakableByDamageCrowdControlAura())
                    {
                        return masterVictim;
                    }
                }
            }
        }
    }

    float dps = AI_VALUE(float, "estimated group dps");

    if (botAI->GetNearGroupMemberCount() > 3)
    {
        if (botAI->IsCaster(bot))
        {
            // Caster find target strategy avoids casting spells on enemies
            // with too low health to ensure the effectiveness of casting
            CasterFindTargetSmartStrategy strategy(botAI, dps);
            return TargetValue::FindTarget(&strategy);
        }
        else if (botAI->IsCombo(bot))
        {
            ComboFindTargetSmartStrategy strategy(botAI, dps);
            return TargetValue::FindTarget(&strategy);
        }
    }
    GeneralFindTargetSmartStrategy strategy(botAI, dps);
    return TargetValue::FindTarget(&strategy);
}

class FindMaxHpTargetStrategy : public DpsFindTargetStrategy
{
public:
    FindMaxHpTargetStrategy(PlayerbotAI* botAI) : DpsFindTargetStrategy(botAI), maxHealth(0) {}

    void CheckAttacker(Unit* attacker, ThreatManager*) override
    {
        if (Group* group = botAI->GetBot()->GetGroup())
        {
            ObjectGuid guid = group->GetTargetIcon(4);
            if (guid && attacker->GetGUID() == guid)
                return;
        }

        if (!result || result->GetHealth() < attacker->GetHealth())
            result = attacker;
    }

protected:
    float maxHealth;
};

Unit* DpsAoeTargetValue::Calculate()
{
    Unit* rti = RtiTargetValue::Calculate();
    if (rti)
        return rti;

    FindMaxHpTargetStrategy strategy(botAI);
    return TargetValue::FindTarget(&strategy);
}
