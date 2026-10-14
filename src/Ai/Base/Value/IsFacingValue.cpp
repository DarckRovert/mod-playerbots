/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "IsFacingValue.h"
#include "Playerbots.h"
#include <cmath>

bool IsFacingValue::Calculate()
{
    Unit* target = AI_VALUE(Unit*, qualifier);
    if (!target)
        return false;

    if (sPlayerbotAIConfig.jainaNextTargetSameTick)
        return bot->HasInArc(170.0f * float(M_PI) / 180.0f, target);

    return bot->HasInArc(M_PI_2, target);
}
