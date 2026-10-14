/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "DruidCatActions.h"
#include "Playerbots.h"

bool CastCowerAction::isUseful()
{
    if (!CastBuffSpellAction::isUseful())
        return false;

    Unit* target = AI_VALUE(Unit*, "current target");
    return !(target && target->IsPlayer());
}
