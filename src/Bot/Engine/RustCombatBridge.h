/*
 * Project Jaina — jAInaCore Rust Combat Core Bridge (F2 Shadow Mode / F3 Pilot / F4 Squad Blackboard)
 * Connects mod-playerbots Engine::DoNextAction to jaina_combat_core.dll via versioned C-ABI POD structs.
 *
 * Design Invariants (PLAN_JAINA_COMBAT_CORE_RUST):
 * 1. Stateless & Re-entrant Rust: SquadBlackboardPOD (600B) is owned by C++ per map worker thread (thread_local),
 *    eliminating all Mutexes and preventing unbounded HashMap memory growth.
 * 2. Engine Action & Multiplier Architecture (R2): Integrated as `RustCombatAction` inside `Engine::DoNextAction`'s
 *    while-loop at relevance `ACTION_MOVE - 0.5f` (29.5f), so all Multipliers (BossAI, Passive) and all high-priority
 *    actions (`>= 30.0f`: Emergency 90, Raid 60, Assist 50, Dispel 50, Interrupt 40, Move 30) are evaluated first.
 * 3. Per-Bot Spell ID Cache: Resolves canonical WotLK 3.3.5a spell chains (`GetFirstSpellInChain`/`GetNextSpellInChain`)
 *    once per (bot_guid, level, spec_id, form) and caches the `uint32_t` IDs on the worker thread (100% DBC-locale immune).
 */

#ifndef PLAYERBOTS_RUST_COMBAT_BRIDGE_H
#define PLAYERBOTS_RUST_COMBAT_BRIDGE_H

#include "Action.h"
#include "AiFactory.h"
#include "Group.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Strategy.h"
#include "Timer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace JainaRustCombat
{
    static constexpr uint32_t JCC_ABI_VERSION = 0x00010002u;

    enum SpellSlot : uint8_t
    {
        SLOT_PRIMARY_FILLER    = 0,
        SLOT_INSTANT_PROC_1    = 1,
        SLOT_INSTANT_PROC_2    = 2,
        SLOT_CORE_SIGNATURE    = 3,
        SLOT_EXECUTE_FINISHER  = 4,
        SLOT_BURST_COOLDOWN    = 5,
        SLOT_SUMMON_OR_MIRROR  = 6,
        SLOT_DEFENSIVE_BARRIER = 7,
        SLOT_INTERRUPT_SPELL   = 8,
        SLOT_AOE_SPELL         = 9,
        SLOT_DOT_MAINTENANCE   = 10,
        SLOT_EMERGENCY_SAVE    = 11,
        SLOT_HEAL_FAST_INSTANT = 12,
        SLOT_HEAL_BIG_DIRECT   = 13,
        SLOT_HEAL_GROUP_CHAIN  = 14,
        SLOT_TAUNT_SPELL       = 15,
        SLOT_MAX               = 16
    };

#pragma pack(push, 8)
    struct CombatSnapshotPOD
    {
        uint32_t abi_version;
        uint32_t struct_size;
        uint64_t bot_guid;
        uint64_t target_guid;
        uint64_t group_guid;
        uint32_t map_id;
        uint32_t instance_id;
        uint64_t now_ms;
        uint8_t  class_id;
        uint8_t  spec_id;
        uint8_t  role;
        uint8_t  level;
        uint8_t  is_pve;
        uint8_t  is_moving;
        uint8_t  is_casting;
        uint8_t  in_dungeon_or_raid;
        float    bot_hp_pct;
        float    bot_power_pct;
        float    target_hp_pct;
        float    target_distance;
        uint8_t  target_is_casting;
        uint8_t  target_is_boss;
        uint8_t  target_is_frozen_or_fof;
        uint8_t  has_brain_freeze;
        uint8_t  has_missile_barrage;
        uint8_t  has_hot_streak;
        uint8_t  has_ice_barrier;
        uint8_t  has_target_dot;
        uint8_t  active_enemy_count;
        uint8_t  injured_ally_count;
        uint8_t  tank_needs_swap;
        uint8_t  _pad0;
        uint32_t ready_spells[16];
        uint64_t lowest_ally_guid;
        float    lowest_ally_hp_pct;
        uint32_t lowest_ally_missing_hp;
    };

    struct BlackboardSlotPOD
    {
        uint64_t key_guid;
        uint64_t holder_guid;
        uint64_t expires_or_timestamp_ms;
    };

    struct SquadBlackboardPOD
    {
        uint32_t          abi_version;
        uint32_t          struct_size;
        uint64_t          group_guid;
        uint32_t          map_id;
        uint32_t          instance_id;
        BlackboardSlotPOD interrupt_locks[8];
        BlackboardSlotPOD taunt_quarantine[8];
        BlackboardSlotPOD heal_reservations[8];
    };

    struct CombatDecisionPOD
    {
        uint32_t abi_version;
        uint32_t struct_size;
        uint32_t spell_id;
        uint32_t _pad_align;
        uint64_t target_guid;
        uint8_t  decision_category;
        uint8_t  slot_index;
        uint8_t  confidence;
        uint8_t  _pad0[5];
    };
#pragma pack(pop)

    static_assert(sizeof(CombatSnapshotPOD) == 168, "CombatSnapshotPOD C-ABI size must be exactly 168 bytes");
    static_assert(sizeof(SquadBlackboardPOD) == 600, "SquadBlackboardPOD C-ABI size must be exactly 600 bytes");
    static_assert(sizeof(CombatDecisionPOD) == 32, "CombatDecisionPOD C-ABI size must be exactly 32 bytes");

    using FnAbiVersion     = uint32_t (*)();
    using FnStructSize     = uint32_t (*)();
    using FnVersionStr     = char const* (*)();
    using FnEvaluateCombat = int32_t (*)(CombatSnapshotPOD const*, SquadBlackboardPOD*, CombatDecisionPOD*);

    struct RuntimeState
    {
        std::once_flag initFlag;
        bool isLoaded = false;
        FnAbiVersion fnAbiVersion = nullptr;
        FnStructSize fnSnapshotSize = nullptr;
        FnStructSize fnBlackboardSize = nullptr;
        FnStructSize fnDecisionSize = nullptr;
        FnVersionStr fnVersionStr = nullptr;
        FnEvaluateCombat fnEvaluate = nullptr;

        std::atomic<uint64_t> shadowCalls{0};
        std::atomic<uint64_t> shadowAgreements{0};
        std::atomic<uint64_t> shadowDisagreements{0};
        std::atomic<uint64_t> activeExecutions{0};
        std::atomic<uint64_t> totalEvaluations{0};
        std::atomic<uint64_t> totalLatencyNs{0};
    };

    inline RuntimeState& GetState()
    {
        static RuntimeState state;
        return state;
    }

    inline bool EnsureInitialized()
    {
        RuntimeState& st = GetState();
        std::call_once(st.initFlag, [&]()
        {
#ifdef _WIN32
            char const* candidates[] = {
                "jaina_combat_core.dll",
                "server/jaina_combat_core.dll",
                "E:/JainaCore_Server/server/jaina_combat_core.dll",
                "E:/JainaCore_Server/build/bin/RelWithDebInfo/jaina_combat_core.dll"
            };

            HMODULE hMod = nullptr;
            char const* loadedPath = nullptr;
            for (char const* path : candidates)
            {
                hMod = LoadLibraryA(path);
                if (hMod)
                {
                    loadedPath = path;
                    break;
                }
            }

            if (!hMod)
            {
                LOG_WARN("server.loading", "[jAInaCore-RustCombat] jaina_combat_core.dll no encontrada; operando 100% con Engine C++.");
                return;
            }

            st.fnAbiVersion     = reinterpret_cast<FnAbiVersion>(GetProcAddress(hMod, "jcc_abi_version"));
            st.fnSnapshotSize   = reinterpret_cast<FnStructSize>(GetProcAddress(hMod, "jcc_snapshot_size"));
            st.fnBlackboardSize = reinterpret_cast<FnStructSize>(GetProcAddress(hMod, "jcc_blackboard_size"));
            st.fnDecisionSize   = reinterpret_cast<FnStructSize>(GetProcAddress(hMod, "jcc_decision_size"));
            st.fnVersionStr     = reinterpret_cast<FnVersionStr>(GetProcAddress(hMod, "jcc_version_str"));
            st.fnEvaluate       = reinterpret_cast<FnEvaluateCombat>(GetProcAddress(hMod, "jcc_evaluate_combat"));

            if (!st.fnAbiVersion || !st.fnSnapshotSize || !st.fnBlackboardSize || !st.fnDecisionSize || !st.fnVersionStr || !st.fnEvaluate)
            {
                LOG_ERROR("server.loading", "[jAInaCore-RustCombat] Simbolos C-ABI incompletos en {}; desactivando RustCombat.", loadedPath);
                return;
            }

            uint32_t dllAbi  = st.fnAbiVersion();
            uint32_t dllSnap = st.fnSnapshotSize();
            uint32_t dllBb   = st.fnBlackboardSize();
            uint32_t dllDec  = st.fnDecisionSize();

            if (dllAbi != JCC_ABI_VERSION || dllSnap != sizeof(CombatSnapshotPOD) || dllBb != sizeof(SquadBlackboardPOD) || dllDec != sizeof(CombatDecisionPOD))
            {
                LOG_ERROR("server.loading",
                    "[jAInaCore-RustCombat] Desajuste de ABI en {} (ABI=0x{:08X} vs 0x{:08X}, Snap={} vs {}, Bb={} vs {}, Dec={} vs {}). Desactivando de forma segura.",
                    loadedPath, dllAbi, JCC_ABI_VERSION, dllSnap, sizeof(CombatSnapshotPOD), dllBb, sizeof(SquadBlackboardPOD), dllDec, sizeof(CombatDecisionPOD));
                return;
            }

            st.isLoaded = true;
            LOG_INFO("server.loading",
                "[jAInaCore-RustCombat] Motor {} cargado desde {} (ABI=0x{:08X}, Snap={}B, Bb={}B, Dec={}B).",
                st.fnVersionStr(), loadedPath, dllAbi, dllSnap, dllBb, dllDec);
#endif
        });
        return st.isLoaded;
    }

    /// Pizarra de Escuadrón propiedad de C++ en el hilo de mapa (`thread_local`), acotada a 32 grupos simultáneos por hilo (19.2 KB fijos, 0 Mutex, 0 fugas).
    inline SquadBlackboardPOD* GetThreadLocalSquadBlackboard(uint64_t groupGuid, uint32_t mapId, uint32_t instanceId)
    {
        if (groupGuid == 0)
            return nullptr;

        struct ThreadBlackboardRing
        {
            SquadBlackboardPOD entries[32]{};
            uint64_t lastTouchMs[32]{};
        };

        thread_local ThreadBlackboardRing ring;
        uint64_t nowMs = static_cast<uint64_t>(getMSTime());

        // 1. Buscar entrada existente para (groupGuid, mapId, instanceId)
        for (size_t i = 0; i < 32; ++i)
        {
            SquadBlackboardPOD& b = ring.entries[i];
            if (b.abi_version == JCC_ABI_VERSION && b.group_guid == groupGuid && b.map_id == mapId && b.instance_id == instanceId)
            {
                ring.lastTouchMs[i] = nowMs;
                return &b;
            }
        }

        // 2. Asignar ranura vacía o desalojar la menos usada recientemente (LRU acotado)
        size_t replaceIdx = 0;
        uint64_t oldestTouch = UINT64_MAX;
        for (size_t i = 0; i < 32; ++i)
        {
            if (ring.entries[i].abi_version != JCC_ABI_VERSION || ring.entries[i].group_guid == 0)
            {
                replaceIdx = i;
                break;
            }
            if (ring.lastTouchMs[i] < oldestTouch)
            {
                oldestTouch = ring.lastTouchMs[i];
                replaceIdx = i;
            }
        }

        SquadBlackboardPOD& target = ring.entries[replaceIdx];
        std::memset(&target, 0, sizeof(SquadBlackboardPOD));
        target.abi_version = JCC_ABI_VERSION;
        target.struct_size = sizeof(SquadBlackboardPOD);
        target.group_guid  = groupGuid;
        target.map_id      = mapId;
        target.instance_id = instanceId;
        ring.lastTouchMs[replaceIdx] = nowMs;
        return &target;
    }

    inline uint32_t ResolveKnownSpellChain(Player* bot, uint32_t baseSpellId)
    {
        if (!bot || !baseSpellId)
            return 0;

        uint32_t firstId = sSpellMgr->GetFirstSpellInChain(baseSpellId);
        if (!firstId)
            firstId = baseSpellId;

        uint32_t bestId = 0;
        uint32_t currId = firstId;
        for (uint32_t step = 0; currId != 0 && step < 32; ++step)
        {
            if (bot->HasSpell(currId))
            {
                bool isPetSpell = false;
                if (Pet* pet = bot->GetPet())
                    isPetSpell = pet->HasSpell(currId);
                if (!isPetSpell)
                    bestId = currId;
            }
            uint32_t nextId = sSpellMgr->GetNextSpellInChain(currId);
            if (!nextId || nextId == currId)
                break;
            currId = nextId;
        }
        return bestId;
    }

    static constexpr size_t MAX_SLOT_CANDIDATES = 3;

    inline void SetSlotCandidates(
        Player* bot,
        uint32_t (&outSpells)[SLOT_MAX][MAX_SLOT_CANDIDATES],
        SpellSlot slot,
        uint32_t s0,
        uint32_t s1 = 0,
        uint32_t s2 = 0)
    {
        uint32_t baseIds[MAX_SLOT_CANDIDATES] = { s0, s1, s2 };
        size_t writeIdx = 0;
        for (size_t i = 0; i < MAX_SLOT_CANDIDATES; ++i)
        {
            uint32_t id = ResolveKnownSpellChain(bot, baseIds[i]);
            if (!id)
                continue;
            bool duplicate = false;
            for (size_t j = 0; j < writeIdx; ++j)
            {
                if (outSpells[slot][j] == id)
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                outSpells[slot][writeIdx++] = id;
        }
    }

    inline void ResolveClassSpellBookIds(
        Player* bot,
        AiObjectContext* /*ctx*/,
        uint8_t classId,
        uint8_t specTab,
        uint8_t shapeshiftForm,
        uint32_t (&outSpells)[SLOT_MAX][MAX_SLOT_CANDIDATES])
    {
        std::memset(outSpells, 0, sizeof(outSpells));
        switch (classId)
        {
            case CLASS_MAGE:
            {
                uint32_t filler = (specTab == MAGE_TAB_ARCANE) ? 30451u : (specTab == MAGE_TAB_FIRE ? 133u : 116u);
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    filler, 116u, 133u);          // Arcane Blast / Frostbolt / Fireball
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    30455u, 44425u, 2136u);       // Ice Lance / Arcane Barrage / Fire Blast
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_2,    44614u, 11366u, 133u);        // Frostfire Bolt / Pyroblast / Fireball
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    44572u, 120u, 5143u);         // Deep Freeze / Cone of Cold / Arcane Missiles
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  2136u, 30455u, 120u);         // Fire Blast / Ice Lance / Cone of Cold
                SetSlotCandidates(bot, outSpells, SLOT_BURST_COOLDOWN,    12472u, 12042u, 11129u);      // Icy Veins / Arcane Power / Combustion
                SetSlotCandidates(bot, outSpells, SLOT_SUMMON_OR_MIRROR,  55342u, 31687u, 12043u);      // Mirror Image / Water Elemental / Presence of Mind
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 11426u, 1463u, 6143u);        // Ice Barrier / Mana Shield / Frost Ward
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   2139u);                       // Counterspell
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         10u, 2120u, 1449u);           // Blizzard / Flamestrike / Arcane Explosion
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   44457u);                      // Living Bomb
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    12051u, 45438u);              // Evocation / Ice Block
                break;
            }
            case CLASS_WARLOCK:
            {
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    686u, 29722u, 689u);          // Shadow Bolt / Incinerate / Drain Life
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    48181u, 50796u, 17962u);      // Haunt / Chaos Bolt / Conflagrate
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  17877u, 1120u, 6789u);        // Shadowburn / Drain Soul / Death Coil
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   172u, 348u, 980u);            // Corruption / Immolate / Curse of Agony
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         27243u, 5740u, 1949u);        // Seed of Corruption / Rain of Fire / Hellfire
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 28176u, 706u, 687u);          // Fel Armor / Demon Armor / Demon Skin
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    6789u, 689u);                 // Death Coil / Drain Life
                break;
            }
            case CLASS_PRIEST:
            {
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    15407u, 585u, 14914u);        // Mind Flay / Smite / Holy Fire
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    8092u, 47540u, 14914u);       // Mind Blast / Penance / Holy Fire
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  32379u, 2944u, 585u);         // Shadow Word: Death / Devouring Plague / Smite
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 17u, 588u, 586u);             // Power Word: Shield / Inner Fire / Fade
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   589u, 34914u, 2944u);         // Shadow Word: Pain / Vampiric Touch / Devouring Plague
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         48045u, 15237u);              // Mind Sear / Holy Nova
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    47585u, 19236u, 8122u);       // Dispersion / Desperate Prayer / Psychic Scream
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 2061u, 47540u, 139u);         // Flash Heal / Penance / Renew
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   2060u, 2054u, 2050u);         // Greater Heal / Heal / Lesser Heal
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_GROUP_CHAIN,  596u, 34861u, 15237u);        // Prayer of Healing / Circle of Healing / Holy Nova
                break;
            }
            case CLASS_SHAMAN:
            {
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    403u, 60103u, 8042u);         // Lightning Bolt / Lava Lash / Earth Shock
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    51505u, 17364u, 421u);        // Lava Burst / Stormstrike / Chain Lightning
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  8042u, 8056u);                // Earth Shock / Frost Shock
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   57994u, 8042u);               // Wind Shear / Earth Shock
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         421u, 1535u, 8190u);          // Chain Lightning / Fire Nova / Magma Totem
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   8050u);                       // Flame Shock
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 324u, 52127u, 974u);          // Lightning Shield / Water Shield / Earth Shield
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    30823u, 5730u);               // Shamanistic Rage / Stoneclaw Totem
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 61295u, 8004u);               // Riptide / Lesser Healing Wave
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   331u, 8004u);                 // Healing Wave / Lesser Healing Wave
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_GROUP_CHAIN,  1064u);                       // Chain Heal
                break;
            }
            case CLASS_WARRIOR:
            {
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    23922u, 12294u, 23881u);      // Shield Slam / Mortal Strike / Bloodthirst
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    6572u, 7384u, 78u);           // Revenge / Overpower / Heroic Strike
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  5308u, 34428u, 7384u);        // Execute / Victory Rush / Overpower
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 2565u, 2687u, 18499u);        // Shield Block / Bloodrage / Berserker Rage
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   6552u, 72u, 12809u);          // Pummel / Shield Bash / Concussion Blow
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         6343u, 1680u, 845u);          // Thunder Clap / Whirlwind / Cleave
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   772u);                        // Rend
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    871u, 12975u, 20230u);        // Shield Wall / Last Stand / Retaliation
                SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       355u, 694u, 1161u);           // Taunt / Mocking Blow / Challenging Shout
                break;
            }
            case CLASS_PALADIN:
            {
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    35395u, 53595u, 26573u);      // Crusader Strike / Hammer of the Righteous / Consecration
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    20271u, 53408u, 879u);        // Judgement of Light / Judgement of Wisdom / Exorcism
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  24275u, 879u, 2812u);         // Hammer of Wrath / Exorcism / Holy Wrath
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 20925u, 53601u, 21084u);      // Holy Shield / Sacred Shield / Seal of Righteousness
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   853u, 31935u);                // Hammer of Justice / Avenger's Shield
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         53385u, 26573u, 2812u);       // Divine Storm / Consecration / Holy Wrath
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    642u, 633u, 498u);            // Divine Shield / Lay on Hands / Divine Protection
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 20473u, 19750u);              // Holy Shock / Flash of Light
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   635u, 19750u);                // Holy Light / Flash of Light
                SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       62124u, 31789u);              // Hand of Reckoning / Righteous Defense
                break;
            }
            case CLASS_ROGUE:
            {
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    1329u, 16511u, 1752u);        // Mutilate / Hemorrhage / Sinister Strike
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    1752u, 53u, 5938u);           // Sinister Strike / Backstab / Shiv
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  2098u, 32645u);               // Eviscerate / Envenom
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   1766u, 408u, 1776u);          // Kick / Kidney Shot / Gouge
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         51723u, 13877u);              // Fan of Knives / Blade Flurry
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   1943u, 703u);                 // Rupture / Garrote
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 5277u, 1966u);                // Evasion / Feint
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    31224u, 5277u, 1856u);        // Cloak of Shadows / Evasion / Vanish
                break;
            }
            case CLASS_HUNTER:
            {
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    56641u, 3044u, 2973u);        // Steady Shot / Arcane Shot / Raptor Strike
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    19434u, 53209u, 53301u);      // Aimed Shot / Chimera Shot / Explosive Shot
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  53351u, 3044u, 1495u);        // Kill Shot / Arcane Shot / Mongoose Bite
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   1978u, 3674u);                // Serpent Sting / Black Arrow
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         2643u, 1510u, 13813u);        // Multi-Shot / Volley / Explosive Trap
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   34490u, 19503u, 19386u);      // Silencing Shot / Scatter Shot / Wyvern Sting
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    19263u, 5384u, 781u);         // Deterrence / Feign Death / Disengage
                break;
            }
            case CLASS_DEATH_KNIGHT:
            {
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    49020u, 55090u, 55050u);      // Obliterate / Scourge Strike / Heart Strike
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    45902u, 45477u, 45462u);      // Blood Strike / Icy Touch / Plague Strike
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  47541u, 49143u, 56815u);      // Death Coil / Frost Strike / Rune Strike
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   47528u, 47476u);              // Mind Freeze / Strangulate
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         49184u, 48721u, 43265u);      // Howling Blast / Blood Boil / Death and Decay
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 49222u, 57330u);              // Bone Shield / Horn of Winter
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    48792u, 55233u, 48707u);      // Icebound Fortitude / Vampiric Blood / Anti-Magic Shell
                SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       56222u, 49576u);              // Dark Command / Death Grip
                break;
            }
            case CLASS_DRUID:
            {
                if (shapeshiftForm == FORM_BEAR || shapeshiftForm == FORM_DIREBEAR)
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    33878u, 33745u, 6807u);   // Mangle (Bear) / Lacerate / Maul
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    6807u, 779u, 16857u);     // Maul / Swipe (Bear) / Faerie Fire (Feral)
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         779u, 99u);               // Swipe (Bear) / Demoralizing Roar
                    SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 22812u, 5229u);           // Barkskin / Enrage
                    SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    61336u, 22842u, 22812u);  // Survival Instincts / Frenzied Regeneration / Barkskin
                    SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       6795u, 5209u);            // Growl / Challenging Roar
                }
                else if (shapeshiftForm == FORM_CAT)
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    33876u, 5221u, 1082u);    // Mangle (Cat) / Shred / Claw
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    1082u, 1822u, 16857u);    // Claw / Rake / Faerie Fire (Feral)
                    SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  22568u, 1079u);           // Ferocious Bite / Rip
                    SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   1822u, 1079u);            // Rake / Rip
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         62078u);                  // Swipe (Cat)
                    SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 22812u, 5217u);           // Barkskin / Tiger's Fury
                    SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    61336u, 22812u);          // Survival Instincts / Barkskin
                }
                else
                {
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    5176u, 2912u);            // Wrath / Starfire
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    2912u, 5176u);            // Starfire / Wrath
                    SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   8921u, 5570u);            // Moonfire / Insect Swarm
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         48505u, 50516u, 16914u);  // Starfall / Typhoon / Hurricane
                    SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 22812u, 467u);            // Barkskin / Thorns
                    SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    22812u, 16689u);          // Barkskin / Nature's Grasp
                    SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 774u, 33763u, 18562u);    // Rejuvenation / Lifebloom / Swiftmend
                    SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   50464u, 8936u, 5185u);    // Nourish / Regrowth / Healing Touch
                    SetSlotCandidates(bot, outSpells, SLOT_HEAL_GROUP_CHAIN,  48438u, 740u);            // Wild Growth / Tranquility
                }
                break;
            }
            default:
                break;
        }
    }

    inline bool IsSpellReadyAndCastableOnTarget(
        Player* bot,
        AiObjectContext* ctx,
        uint32_t spellId,
        Unit* target,
        size_t slotIdx)
    {
        if (!spellId || !bot || !target || !target->IsInWorld() || !target->IsAlive())
            return false;

        if (bot->HasSpellCooldown(spellId))
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return false;

        // Verificar coste de poder antes de instanciar Spell (Spell::Spell inicializa m_powerCost = 0 hasta Spell::prepare)
        if (spellInfo->PowerType < MAX_POWERS)
        {
            Powers powerType = static_cast<Powers>(spellInfo->PowerType);
            int32_t neededPower = spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
            if (static_cast<int32_t>(bot->GetPower(powerType)) < neededPower)
                return false;
        }

        // Evitar encolar golpes de "siguiente ataque melee" (Heroic Strike, Maul, Cleave) si ya hay uno activo
        if ((spellInfo->Attributes & (SPELL_ATTR0_ON_NEXT_SWING | SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE)) != 0)
        {
            if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr)
                return false;
        }

        // Si el bot se está moviendo, descartar hechizos con tiempo de casteo o canalizados
        uint32_t castTime = !spellInfo->IsChanneled() ? spellInfo->CalcCastTime(bot) : spellInfo->GetDuration();
        if ((castTime > 0 || spellInfo->IsAutoRepeatRangedSpell()) && bot->isMoving())
            return false;

        // Evitar lanzar AoE centrado en el lanzador (Thunder Clap, Consecration, Arcane Explosion, Swipe) lejos del enemigo
        if ((slotIdx == SLOT_AOE_SPELL || slotIdx == SLOT_CORE_SIGNATURE) && spellInfo->GetMaxRange(false) <= 0.0f && bot != target && bot->GetDistance(target) > 8.5f)
            return false;

        if (bot != target && target->IsImmunedToSpell(spellInfo) && spellId != 44572)
            return false;

        ObjectGuid oldSel = bot->GetSelectedUnit() ? bot->GetSelectedUnit()->GetGUID() : ObjectGuid::Empty;
        Spell* spell = new Spell(bot, spellInfo, TRIGGERED_NONE);
        if (spellInfo->Targets & TARGET_FLAG_DEST_LOCATION)
            spell->m_targets.SetDst(*target);
        else if (spellInfo->Targets & TARGET_FLAG_SOURCE_LOCATION)
            spell->m_targets.SetDst(*bot);
        else
            spell->m_targets.SetUnitTarget(target);

        if (ctx)
        {
            if (Item* itemTarget = ctx->GetValue<Item*>("item for spell", spellId)->Get())
                spell->m_targets.SetItemTarget(itemTarget);
        }

        SpellCastResult result = spell->CheckCast(true);
        delete spell;
        if (oldSel)
            bot->SetSelection(oldSel);

        return (result == SPELL_CAST_OK || result == SPELL_FAILED_NOT_INFRONT || result == SPELL_FAILED_UNIT_NOT_INFRONT);
    }

    inline void PopulateClassSpellSlots(
        Player* bot,
        AiObjectContext* ctx,
        uint8_t classId,
        uint8_t specTab,
        Unit* enemyTarget,
        Unit* lowestAllyUnit,
        CombatSnapshotPOD& snap)
    {
        struct BotSpellCacheSlot
        {
            uint64_t botGuid        = 0;
            uint8_t  level          = 0;
            uint8_t  specTab        = 0;
            uint8_t  classId        = 0;
            uint8_t  shapeshiftForm = 0;
            uint32_t spellIds[SLOT_MAX][MAX_SLOT_CANDIDATES]{};
        };

        uint8_t form = static_cast<uint8_t>(bot->GetShapeshiftForm());
        thread_local BotSpellCacheSlot spellCache[64]{};
        size_t idx = static_cast<size_t>(snap.bot_guid ^ (snap.bot_guid >> 17)) & 63u;
        BotSpellCacheSlot& entry = spellCache[idx];

        if (entry.botGuid != snap.bot_guid || entry.level != snap.level ||
            entry.specTab != specTab || entry.classId != classId || entry.shapeshiftForm != form)
        {
            entry.botGuid        = snap.bot_guid;
            entry.level          = snap.level;
            entry.specTab        = specTab;
            entry.classId        = classId;
            entry.shapeshiftForm = form;
            ResolveClassSpellBookIds(bot, ctx, classId, specTab, form, entry.spellIds);
        }

        bool anyKnownDotMissing = false;
        bool anyKnownDotPresent = false;

        for (size_t s = 0; s < SLOT_MAX; ++s)
        {
            Unit* slotTarget = enemyTarget;
            if (s == SLOT_BURST_COOLDOWN || s == SLOT_SUMMON_OR_MIRROR ||
                s == SLOT_DEFENSIVE_BARRIER || s == SLOT_EMERGENCY_SAVE)
            {
                slotTarget = bot;
            }
            else if (s == SLOT_HEAL_FAST_INSTANT || s == SLOT_HEAL_BIG_DIRECT || s == SLOT_HEAL_GROUP_CHAIN)
            {
                slotTarget = lowestAllyUnit;
            }

            uint32_t chosenSpell = 0;
            for (size_t c = 0; c < MAX_SLOT_CANDIDATES; ++c)
            {
                uint32_t candId = entry.spellIds[s][c];
                if (!candId)
                    continue;

                if (s == SLOT_DEFENSIVE_BARRIER && bot->HasAura(candId))
                {
                    if (classId == CLASS_MAGE)
                        snap.has_ice_barrier = 1;
                    continue;
                }

                if (s == SLOT_DOT_MAINTENANCE && enemyTarget)
                {
                    if (enemyTarget->HasAura(candId, bot->GetGUID()))
                    {
                        anyKnownDotPresent = true;
                        continue;
                    }
                    anyKnownDotMissing = true;
                }

                if (slotTarget && IsSpellReadyAndCastableOnTarget(bot, ctx, candId, slotTarget, s))
                {
                    chosenSpell = candId;
                    break;
                }
            }
            snap.ready_spells[s] = chosenSpell;
        }

        if (anyKnownDotPresent && !anyKnownDotMissing)
            snap.has_target_dot = 1;
    }

    inline bool BuildCombatSnapshot(PlayerbotAI* botAI, AiObjectContext* ctx, CombatSnapshotPOD& snap)
    {
        if (!botAI || !ctx)
            return false;

        Player* bot = botAI->GetBot();
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || (botAI->GetState() != BOT_STATE_COMBAT && !bot->IsInCombat()))
            return false;

        std::memset(&snap, 0, sizeof(CombatSnapshotPOD));
        snap.abi_version = JCC_ABI_VERSION;
        snap.struct_size = sizeof(CombatSnapshotPOD);
        snap.bot_guid    = bot->GetGUID().GetRawValue();
        snap.map_id      = bot->GetMapId();
        snap.instance_id = bot->GetInstanceId();
        snap.now_ms      = static_cast<uint64_t>(getMSTime());
        snap.class_id    = bot->getClass();
        snap.spec_id     = static_cast<uint8_t>(AiFactory::GetPlayerSpecTab(bot));
        snap.level       = bot->GetLevel();

        if (PlayerbotAI::IsTank(bot))
            snap.role = 0;
        else if (PlayerbotAI::IsHeal(bot))
            snap.role = 1;
        else if (PlayerbotAI::IsMelee(bot))
            snap.role = 2;
        else
            snap.role = 3;

        Unit* target = ctx->GetValue<Unit*>("current target")->Get();
        if (!target || !target->IsInWorld() || !target->IsAlive() || target->IsFriendlyTo(bot))
            target = bot->GetVictim();
        if (!target || !target->IsInWorld() || !target->IsAlive() || target->IsFriendlyTo(bot))
            target = ctx->GetValue<Unit*>(PlayerbotAI::IsTank(bot) ? "tank target" : "dps target")->Get();
        if (!target || !target->IsInWorld() || !target->IsAlive() || target->IsFriendlyTo(bot))
        {
            if (!bot->getAttackers().empty())
                target = *bot->getAttackers().begin();
        }

        bool isPvP = bot->InBattleground() || bot->InArena();
        snap.is_pve             = isPvP ? 0 : 1;
        snap.is_moving          = bot->isMoving() ? 1 : 0;
        snap.is_casting         = (bot->HasUnitState(UNIT_STATE_CASTING) || bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) != nullptr) ? 1 : 0;
        snap.in_dungeon_or_raid = (bot->GetMap() && (bot->GetMap()->IsDungeon() || bot->GetMap()->IsRaid())) ? 1 : 0;
        snap.bot_hp_pct         = bot->GetHealthPct();
        snap.bot_power_pct      = (bot->getPowerType() == POWER_MANA && bot->GetMaxPower(POWER_MANA) > 0)
                                    ? (100.0f * static_cast<float>(bot->GetPower(POWER_MANA)) / static_cast<float>(bot->GetMaxPower(POWER_MANA)))
                                    : 100.0f;

        uint8_t attackerCount = ctx->GetValue<uint8>("attacker count")->Get();
        if (attackerCount == 0 && target && target->IsAlive())
            attackerCount = 1;
        snap.active_enemy_count = attackerCount;

        Unit* lowestAllyUnit = bot;
        snap.lowest_ally_guid       = snap.bot_guid;
        snap.lowest_ally_hp_pct     = snap.bot_hp_pct;
        snap.lowest_ally_missing_hp = (bot->GetMaxHealth() > bot->GetHealth()) ? (bot->GetMaxHealth() - bot->GetHealth()) : 0;
        snap.injured_ally_count     = (snap.bot_hp_pct < 92.0f) ? 1 : 0;

        if (Group* grp = bot->GetGroup())
        {
            snap.group_guid = grp->GetGUID().GetRawValue();
            uint8_t injuredCount = 0;
            float lowestPct = 100.0f;
            uint64_t lowestGuid = snap.bot_guid;
            uint32_t lowestMissing = 0;

            for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
                    continue;
                if (bot->GetDistance2d(member) > 40.0f)
                    continue;

                float hpPct = member->GetHealthPct();
                if (hpPct < 92.0f)
                    ++injuredCount;
                if (hpPct < lowestPct || !lowestAllyUnit)
                {
                    lowestPct = hpPct;
                    lowestGuid = member->GetGUID().GetRawValue();
                    lowestAllyUnit = member;
                    lowestMissing = (member->GetMaxHealth() > member->GetHealth()) ? (member->GetMaxHealth() - member->GetHealth()) : 0;
                }
            }
            snap.injured_ally_count     = injuredCount;
            snap.lowest_ally_guid       = lowestGuid;
            snap.lowest_ally_hp_pct     = lowestPct;
            snap.lowest_ally_missing_hp = lowestMissing;
        }

        if (target && target->IsInWorld() && target->IsAlive() && !target->IsFriendlyTo(bot))
        {
            snap.target_guid     = target->GetGUID().GetRawValue();
            snap.target_hp_pct   = target->GetHealthPct();
            snap.target_distance = bot->GetDistance(target);

            bool isCastingInterruptible = false;
            if (target->IsNonMeleeSpellCast(true))
            {
                Spell* currentCasting = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (!currentCasting)
                    currentCasting = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                if (currentCasting && currentCasting->GetSpellInfo())
                    isCastingInterruptible = (currentCasting->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;
                else
                    isCastingInterruptible = true;
            }
            snap.target_is_casting = isCastingInterruptible ? 1 : 0;

            if (Creature* c = target->ToCreature())
            {
                snap.target_is_boss = (c->IsDungeonBoss() || c->isWorldBoss() || c->GetCreatureTemplate()->rank == CREATURE_ELITE_WORLDBOSS) ? 1 : 0;
            }
            if (snap.role == 0 && target->GetVictim() && target->GetVictim() != bot)
            {
                snap.tank_needs_swap = 1;
            }
        }

        // Procs y auras específicas de Mago (Piloto F3)
        if (snap.class_id == CLASS_MAGE)
        {
            bool hasFoF = bot->HasAura(44544) || (target && (target->HasAura(122) || target->HasAura(44572) || target->HasAura(33395)));
            snap.target_is_frozen_or_fof = hasFoF ? 1 : 0;
            snap.has_brain_freeze        = bot->HasAura(57761) ? 1 : 0;
            snap.has_missile_barrage     = bot->HasAura(44401) ? 1 : 0;
            snap.has_hot_streak          = bot->HasAura(48108) ? 1 : 0;
            snap.has_ice_barrier         = (bot->HasAura(11426) || bot->HasAura(43039)) ? 1 : 0;
            snap.has_target_dot          = (target && (target->HasAura(44457) || target->HasAura(55360))) ? 1 : 0;
        }

        PopulateClassSpellSlots(bot, ctx, snap.class_id, snap.spec_id, target, lowestAllyUnit, snap);
        return true;
    }

    inline bool IsBotControlledByRust(Player* bot, uint32_t mode)
    {
        if (mode < 2 || !bot)
            return false;
        if (mode == 2 && bot->getClass() != CLASS_MAGE)
            return false;
        return EnsureInitialized();
    }

    /// Modo Activo (F3/F5): Ejecutado por `RustCombatAction::Execute` dentro del bucle `while` de `Engine::DoNextAction`.
    inline bool TryExecuteActiveDecision(PlayerbotAI* botAI, AiObjectContext* ctx, uint32_t mode)
    {
        if (mode < 2 || !EnsureInitialized())
            return false;

        Player* bot = botAI->GetBot();
        if (!bot || !bot->GetSession() || bot->GetSession()->IsLoggingOut())
            return false;

        if (mode == 2 && bot->getClass() != CLASS_MAGE)
            return false;

        CombatSnapshotPOD snap;
        if (!BuildCombatSnapshot(botAI, ctx, snap) || !snap.is_pve)
            return false;

        SquadBlackboardPOD* bb = GetThreadLocalSquadBlackboard(snap.group_guid, snap.map_id, snap.instance_id);
        CombatDecisionPOD dec{};
        auto t0 = std::chrono::steady_clock::now();
        int32_t rc = GetState().fnEvaluate(&snap, bb, &dec);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        GetState().totalLatencyNs.fetch_add(static_cast<uint64_t>(ns > 0 ? ns : 0), std::memory_order_relaxed);
        uint64_t evals = GetState().totalEvaluations.fetch_add(1, std::memory_order_relaxed) + 1;

        if (rc != 1 || dec.spell_id == 0 || dec.target_guid == 0)
            return false;

        // Regla R3: Doble verificación contra hechizos de mascota
        if (Pet* pet = bot->GetPet())
        {
            if (pet->HasSpell(dec.spell_id))
                return false;
        }

        Unit* targetUnit = (dec.target_guid == snap.bot_guid) ? static_cast<Unit*>(bot) : botAI->GetUnit(ObjectGuid(dec.target_guid));
        if (!targetUnit || !targetUnit->IsInWorld() || !targetUnit->IsAlive())
            return false;

        if (!botAI->CanCastSpell(dec.spell_id, targetUnit, true))
            return false;

        if (botAI->CastSpell(dec.spell_id, targetUnit))
        {
            uint64_t execs = GetState().activeExecutions.fetch_add(1, std::memory_order_relaxed) + 1;
            uint32_t sampleRate = sPlayerbotAIConfig.rustCombatShadowSampleRate ? sPlayerbotAIConfig.rustCombatShadowSampleRate : 200;
            if (execs <= 5 || execs % sampleRate == 0)
            {
                uint64_t totalNs = GetState().totalLatencyNs.load(std::memory_order_relaxed);
                double avgUs = evals > 0 ? (static_cast<double>(totalNs) / static_cast<double>(evals)) / 1000.0 : 0.0;
                LOG_INFO("server.loading",
                    "[jAInaCore-RustCombat][ACTIVE] Executions={} AvgLatency={:.3f}us | Bot={} Class={} SpellId={} Category={} Slot={}",
                    execs,
                    avgUs,
                    bot->GetName(),
                    static_cast<uint32_t>(snap.class_id),
                    dec.spell_id,
                    static_cast<uint32_t>(dec.decision_category),
                    static_cast<uint32_t>(dec.slot_index));
            }
            return true;
        }
        return false;
    }

    /// Acción nativa de `mod-playerbots` (R2 y Sección 3.10): se encola con relevancia `ACTION_MOVE - 0.5f` (`29.5f`)
    /// para evaluarse dentro del bucle `while` de `Engine::DoNextAction`, respetando todos los `Multipliers` y todas
    /// las acciones `>= 30.0f` (Emergencias 90, BossAI 60, Assist 50, Dispel 50, Interrupciones 40, Movimiento 30).
    class RustCombatAction : public Action
    {
    public:
        explicit RustCombatAction(PlayerbotAI* botAI) : Action(botAI, "rust combat core") {}

        bool isUseful() override
        {
            Player* bot = botAI ? botAI->GetBot() : nullptr;
            return bot && botAI->GetState() == BOT_STATE_COMBAT &&
                   !bot->InBattleground() && !bot->InArena() &&
                   IsBotControlledByRust(bot, sPlayerbotAIConfig.rustCombatMode);
        }

        bool isPossible() override
        {
            Player* bot = botAI ? botAI->GetBot() : nullptr;
            return bot && bot->IsAlive() && bot->IsInWorld() &&
                   (botAI->GetState() == BOT_STATE_COMBAT || bot->IsInCombat()) &&
                   (!bot->GetSession() || !bot->GetSession()->IsLoggingOut()) &&
                   !bot->HasUnitState(UNIT_STATE_CASTING) &&
                   bot->GetCurrentSpell(CURRENT_CHANNELED_SPELL) == nullptr;
        }

        bool Execute(Event /*event*/) override
        {
            return TryExecuteActiveDecision(botAI, botAI->GetAiObjectContext(), sPlayerbotAIConfig.rustCombatMode);
        }
    };

    /// Modo Sombra (F2): Calcula la decisión en Rust pasando `nullptr` como pizarra (100% libre de efectos secundarios),
    /// mide la latencia en nanosegundos y registra discrepancias muestreadas.
    inline void ObserveShadowDecision(PlayerbotAI* botAI, AiObjectContext* ctx, Action* cppAction)
    {
        if (!cppAction || !EnsureInitialized())
            return;

        CombatSnapshotPOD snap;
        if (!BuildCombatSnapshot(botAI, ctx, snap))
            return;

        CombatDecisionPOD dec{};
        auto t0 = std::chrono::steady_clock::now();
        int32_t rc = GetState().fnEvaluate(&snap, nullptr, &dec);
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();

        RuntimeState& st = GetState();
        st.totalLatencyNs.fetch_add(static_cast<uint64_t>(ns > 0 ? ns : 0), std::memory_order_relaxed);
        uint64_t calls = st.shadowCalls.fetch_add(1, std::memory_order_relaxed) + 1;

        uint32_t cppSpellId = ctx->GetValue<uint32>("spell id", cppAction->getName())->Get();
        bool agreed = (rc == 1 && dec.spell_id != 0 && dec.spell_id == cppSpellId);
        if (agreed)
            st.shadowAgreements.fetch_add(1, std::memory_order_relaxed);
        else
            st.shadowDisagreements.fetch_add(1, std::memory_order_relaxed);

        uint32_t sampleRate = sPlayerbotAIConfig.rustCombatShadowSampleRate ? sPlayerbotAIConfig.rustCombatShadowSampleRate : 200;
        if (calls % sampleRate == 0)
        {
            uint64_t totalNs = st.totalLatencyNs.load(std::memory_order_relaxed);
            double avgUs = calls > 0 ? (static_cast<double>(totalNs) / static_cast<double>(calls)) / 1000.0 : 0.0;
            LOG_INFO("server.loading",
                "[jAInaCore-RustCombat][SHADOW] Calls={} Agree={} Disagree={} AvgLatency={:.3f}us | Bot={} Class={} RustSpell={} vs CppAction='{}' (SpellId={})",
                calls,
                st.shadowAgreements.load(std::memory_order_relaxed),
                st.shadowDisagreements.load(std::memory_order_relaxed),
                avgUs,
                botAI->GetBot()->GetName(),
                static_cast<uint32_t>(snap.class_id),
                dec.spell_id,
                cppAction->getName(),
                cppSpellId);
        }
    }
} // namespace JainaRustCombat

#endif
