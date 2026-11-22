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
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Group.h"
#include "Log.h"
#include "ObjectGuid.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "Spell.h"
#include "SpellAuras.h"
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
    static constexpr uint32_t JCC_ABI_VERSION = 0x00010003u;

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
        uint8_t  has_defensive_barrier;
        uint32_t ready_spells[16];
        uint64_t lowest_ally_guid;
        float    lowest_ally_hp_pct;
        uint32_t lowest_ally_missing_hp;
        uint8_t  eval_phase;                // 0: All, 1: Tactical (29.5f), 2: Rotation (19.5f)
        uint8_t  combo_points;              // 0..5
        uint8_t  target_has_breakable_cc;   // 0/1
        uint8_t  nearby_cc_count;           // hostile units <= 12y with breakable CC
        uint8_t  target_disease_mask;       // bit0=Frost Fever, bit1=Blood Plague, bit2=Ebon Plague
        uint8_t  lowest_ally_is_tank;       // 0/1
        uint8_t  lowest_ally_in_los;        // 0/1
        uint8_t  target_in_los;             // 0/1
        uint32_t active_procs_mask;         // 14-bit proc/buff mask
        uint32_t dispel_spell_id;           // ready dispel spell id
        uint64_t dispel_ally_guid;          // ally guid needing dispel
        uint64_t loose_mob_on_healer_guid;  // loose hostile mob attacking healer/caster
        uint32_t enemy_cast_elapsed_ms;     // elapsed enemy cast ms
        uint32_t enemy_cast_total_ms;       // total enemy cast ms
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

    static_assert(sizeof(CombatSnapshotPOD) == 208, "CombatSnapshotPOD C-ABI size must be exactly 208 bytes");
    static_assert(sizeof(SquadBlackboardPOD) == 600, "SquadBlackboardPOD C-ABI size must be exactly 600 bytes");
    static_assert(sizeof(CombatDecisionPOD) == 32, "CombatDecisionPOD C-ABI size must be exactly 32 bytes");

    using FnAbiVersion     = uint32_t (*)();
    using FnStructSize     = uint32_t (*)();
    using FnVersionStr     = char const* (*)();
    using FnEvaluateCombat = int32_t (*)(CombatSnapshotPOD const*, SquadBlackboardPOD*, CombatDecisionPOD*);
    using FnReloadConfig   = int32_t (*)();

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
        FnReloadConfig fnReloadConfig = nullptr;

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
            st.fnReloadConfig   = reinterpret_cast<FnReloadConfig>(GetProcAddress(hMod, "jcc_reload_config"));

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

    inline void ResolveDispelBookIds(
        Player* bot,
        uint8_t classId,
        uint32_t (&outDispelIds)[2],
        uint32_t (&outDispelMasks)[2])
    {
        std::memset(outDispelIds, 0, sizeof(outDispelIds));
        std::memset(outDispelMasks, 0, sizeof(outDispelMasks));
        if (!bot)
            return;
        switch (classId)
        {
            case CLASS_PRIEST:
                outDispelIds[0]   = ResolveKnownSpellChain(bot, 527u); // Dispel Magic
                outDispelMasks[0] = (1u << DISPEL_MAGIC);
                outDispelIds[1]   = ResolveKnownSpellChain(bot, 552u); // Abolish Disease
                if (!outDispelIds[1])
                    outDispelIds[1] = ResolveKnownSpellChain(bot, 528u); // Cure Disease
                outDispelMasks[1] = (1u << DISPEL_DISEASE);
                break;
            case CLASS_PALADIN:
                outDispelIds[0]   = ResolveKnownSpellChain(bot, 4987u); // Cleanse
                outDispelMasks[0] = (1u << DISPEL_MAGIC) | (1u << DISPEL_DISEASE) | (1u << DISPEL_POISON);
                outDispelIds[1]   = ResolveKnownSpellChain(bot, 1152u); // Purify
                outDispelMasks[1] = (1u << DISPEL_DISEASE) | (1u << DISPEL_POISON);
                break;
            case CLASS_SHAMAN:
                outDispelIds[0]   = ResolveKnownSpellChain(bot, 51886u); // Cleanse Spirit
                outDispelMasks[0] = (1u << DISPEL_CURSE) | (1u << DISPEL_DISEASE) | (1u << DISPEL_POISON);
                outDispelIds[1]   = ResolveKnownSpellChain(bot, 526u); // Cure Toxins
                outDispelMasks[1] = (1u << DISPEL_DISEASE) | (1u << DISPEL_POISON);
                break;
            case CLASS_MAGE:
                outDispelIds[0]   = ResolveKnownSpellChain(bot, 475u); // Remove Curse
                outDispelMasks[0] = (1u << DISPEL_CURSE);
                break;
            case CLASS_DRUID:
                outDispelIds[0]   = ResolveKnownSpellChain(bot, 2782u); // Remove Curse
                outDispelMasks[0] = (1u << DISPEL_CURSE);
                outDispelIds[1]   = ResolveKnownSpellChain(bot, 2893u); // Abolish Poison
                if (!outDispelIds[1])
                    outDispelIds[1] = ResolveKnownSpellChain(bot, 8946u); // Cure Poison
                outDispelMasks[1] = (1u << DISPEL_POISON);
                break;
            default:
                break;
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
                uint32_t filler = (specTab == 2) ? 29722u : 686u;                                       // Incinerate / Shadow Bolt
                uint32_t proc1  = (specTab == 1) ? 6353u : 686u;                                        // Soul Fire / Shadow Bolt
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    filler, 686u, 689u);          // Shadow Bolt / Incinerate / Drain Life
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    proc1, 29722u, 686u);         // Decimation / Molten Core / Nightfall
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    48181u, 50796u, 17962u);      // Haunt / Chaos Bolt / Conflagrate
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  17877u, 1120u, 6789u);        // Shadowburn / Drain Soul / Death Coil
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   (specTab == 2 ? 348u : 172u), 30108u, 980u); // Immolate / Corruption / UA / CoA
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
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   34914u, 589u, 2944u);         // Vampiric Touch / SW:Pain / Devouring Plague
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         48045u, 15237u);              // Mind Sear / Holy Nova
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    47585u, 19236u, 8122u);       // Dispersion / Desperate Prayer / Psychic Scream
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 47540u, 2061u, 139u);         // Penance / Flash Heal / Renew
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   2060u, 2054u, 2050u);         // Greater Heal / Heal / Lesser Heal
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_GROUP_CHAIN,  34861u, 596u, 15237u);        // Circle of Healing / Prayer of Healing / Holy Nova
                break;
            }
            case CLASS_SHAMAN:
            {
                if (specTab == 1) // Enhancement
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    17364u, 60103u);          // Stormstrike / Lava Lash
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    60103u, 8042u);           // Lava Lash / Earth Shock
                    SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    403u);                    // Maelstrom x5 instant Lightning Bolt
                    SetSlotCandidates(bot, outSpells, SLOT_BURST_COOLDOWN,    51533u);                  // Feral Spirit
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         1535u, 8190u, 421u);      // Fire Nova / Magma Totem / Chain Lightning
                }
                else
                {
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    403u, 8042u);             // Lightning Bolt / Earth Shock
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    51505u, 421u);            // Lava Burst / Chain Lightning
                    SetSlotCandidates(bot, outSpells, SLOT_BURST_COOLDOWN,    16166u);                  // Elemental Mastery
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         421u, 1535u);             // Chain Lightning / Fire Nova
                }
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  8042u, 8056u);                // Earth Shock / Frost Shock
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   57994u, 8042u);               // Wind Shear / Earth Shock
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
                if (specTab == 2) // Prot
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    23922u, 6572u);           // Shield Slam / Revenge
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    20243u, 6572u, 78u);      // Devastate / Revenge / Heroic Strike
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         6343u, 46968u, 845u);     // Thunder Clap / Shockwave / Cleave
                }
                else if (specTab == 1) // Fury
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    23881u, 12294u);          // Bloodthirst / Mortal Strike
                    SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    1464u, 7384u);            // Instant Slam (Bloodsurge) / Overpower
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    78u, 7384u);              // Heroic Strike / Overpower
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         1680u, 845u, 6343u);      // Whirlwind / Cleave / Thunder Clap
                    SetSlotCandidates(bot, outSpells, SLOT_BURST_COOLDOWN,    12292u, 1719u);           // Death Wish / Recklessness
                }
                else // Arms
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    12294u, 7384u);           // Mortal Strike / Overpower
                    SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    7384u, 1464u);            // Overpower (TfB) / Slam
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    1464u, 78u, 7384u);       // Slam / Heroic Strike / Overpower
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         46924u, 1680u, 6343u);    // Bladestorm / Whirlwind / Thunder Clap
                }
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  5308u, 34428u, 7384u);        // Execute / Victory Rush / Overpower
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 2565u, 2687u, 18499u);        // Shield Block / Bloodrage / Berserker Rage
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   6552u, 72u, 12809u);          // Pummel / Shield Bash / Concussion Blow
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   772u);                        // Rend
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    871u, 12975u, 20230u);        // Shield Wall / Last Stand / Retaliation
                SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       355u, 694u, 1161u);           // Taunt / Mocking Blow / Challenging Shout
                break;
            }
            case CLASS_PALADIN:
            {
                if (specTab == 1) // Prot
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    53595u, 53600u, 26573u);  // Hammer of Righteous / Shield of Righteousness / Consecration
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    20271u, 53408u, 879u);    // Judgement
                }
                else
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    35395u, 26573u);          // Crusader Strike / Consecration
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    20271u, 53408u, 35395u);  // Judgement of Light / Wisdom / Crusader Strike
                    SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    879u);                    // Art of War Exorcism
                    SetSlotCandidates(bot, outSpells, SLOT_BURST_COOLDOWN,    31884u);                  // Avenging Wrath
                }
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  24275u, 879u, 2812u);         // Hammer of Wrath / Exorcism / Holy Wrath
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 20925u, 53601u, 21084u);      // Holy Shield / Sacred Shield / Seal of Righteousness
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   31935u, 853u);                // Avenger's Shield / Hammer of Justice
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         53385u, 26573u, 2812u);       // Divine Storm / Consecration / Holy Wrath
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    642u, 633u, 498u);            // Divine Shield / Lay on Hands / Divine Protection
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 20473u, 19750u);              // Holy Shock / Flash of Light
                SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   635u, 19750u);                // Holy Light / Flash of Light
                SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       62124u, 31789u);              // Hand of Reckoning / Righteous Defense
                break;
            }
            case CLASS_ROGUE:
            {
                uint32_t builder = (specTab == 0) ? 1329u : ((specTab == 2) ? 16511u : 1752u);          // Mutilate / Hemorrhage / Sinister Strike
                uint32_t finish  = (specTab == 0) ? 32645u : 2098u;                                     // Envenom / Eviscerate
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    builder, 1752u, 53u);         // Core combo builder
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    1752u, 53u, 5938u);           // Sinister Strike / Backstab / Shiv
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    5171u);                       // Slice and Dice (self)
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  finish, 2098u, 32645u);       // Envenom / Eviscerate
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   1766u, 408u, 1776u);          // Kick / Kidney Shot / Gouge
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         51723u, 13877u);              // Fan of Knives / Blade Flurry
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   1943u, 703u);                 // Rupture / Garrote
                SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 5277u, 1966u);                // Evasion / Feint
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    31224u, 5277u, 1856u);        // Cloak of Shadows / Evasion / Vanish
                break;
            }
            case CLASS_HUNTER:
            {
                uint32_t sig = (specTab == 2) ? 53301u : ((specTab == 1) ? 53209u : 19434u);            // Explosive / Chimera / Aimed
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    56641u, 3044u, 2973u);        // Steady Shot / Arcane Shot / Raptor Strike
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    sig, 19434u, 3044u);          // Core signature shot
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    53301u, 3044u);               // Lock and Load Explosive/Arcane Shot
                SetSlotCandidates(bot, outSpells, SLOT_BURST_COOLDOWN,    19574u, 3045u);               // Bestial Wrath / Rapid Fire
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  53351u, 3044u, 1495u);        // Kill Shot / Arcane Shot / Mongoose Bite
                SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   1978u, 3674u);                // Serpent Sting / Black Arrow
                SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         2643u, 1510u, 13813u);        // Multi-Shot / Volley / Explosive Trap
                SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   34490u, 19503u, 19386u);      // Silencing Shot / Scatter Shot / Wyvern Sting
                SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    19263u, 5384u, 781u);         // Deterrence / Feign Death / Disengage
                break;
            }
            case CLASS_DEATH_KNIGHT:
            {
                uint32_t core = (specTab == 0) ? 55050u : ((specTab == 1) ? 49020u : 55090u);           // Heart / Obliterate / Scourge Strike
                uint32_t dump = (specTab == 1) ? 49143u : 47541u;                                       // Frost Strike / Death Coil
                SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    core, 49020u, 55090u);        // Spec core strike
                SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    45902u, 49998u, 45477u);      // Blood Strike / Death Strike / Icy Touch
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    45477u);                      // Icy Touch (Frost Fever)
                SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_2,    45462u);                      // Plague Strike (Blood Plague)
                SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  dump, 47541u, 56815u);        // Frost Strike / Death Coil / Rune Strike
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
                    SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   16979u, 5211u);           // Feral Charge Bear / Bash
                    SetSlotCandidates(bot, outSpells, SLOT_DEFENSIVE_BARRIER, 22812u, 5229u);           // Barkskin / Enrage
                    SetSlotCandidates(bot, outSpells, SLOT_EMERGENCY_SAVE,    61336u, 22842u, 22812u);  // Survival Instincts / Frenzied Regeneration / Barkskin
                    SetSlotCandidates(bot, outSpells, SLOT_TAUNT_SPELL,       6795u, 5209u);            // Growl / Challenging Roar
                }
                else if (shapeshiftForm == FORM_CAT)
                {
                    SetSlotCandidates(bot, outSpells, SLOT_CORE_SIGNATURE,    5221u, 33876u, 1082u);    // Shred / Mangle (Cat) / Claw
                    SetSlotCandidates(bot, outSpells, SLOT_PRIMARY_FILLER,    33876u, 1082u, 16857u);   // Mangle (Cat) / Claw / Faerie Fire (Feral)
                    SetSlotCandidates(bot, outSpells, SLOT_INSTANT_PROC_1,    52610u);                  // Savage Roar (self)
                    SetSlotCandidates(bot, outSpells, SLOT_EXECUTE_FINISHER,  1079u, 22568u);           // Rip / Ferocious Bite
                    SetSlotCandidates(bot, outSpells, SLOT_DOT_MAINTENANCE,   1822u, 1079u);            // Rake / Rip
                    SetSlotCandidates(bot, outSpells, SLOT_AOE_SPELL,         62078u);                  // Swipe (Cat)
                    SetSlotCandidates(bot, outSpells, SLOT_INTERRUPT_SPELL,   22570u, 5211u);           // Maim / Bash
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
                    SetSlotCandidates(bot, outSpells, SLOT_HEAL_FAST_INSTANT, 774u, 18562u, 33763u);    // Rejuvenation / Swiftmend / Lifebloom
                    SetSlotCandidates(bot, outSpells, SLOT_HEAL_BIG_DIRECT,   50464u, 8936u, 5185u);    // Nourish / Regrowth / Healing Touch
                    SetSlotCandidates(bot, outSpells, SLOT_HEAL_GROUP_CHAIN,  48438u, 740u);            // Wild Growth / Tranquility
                }
                break;
            }
            default:
                break;
        }
    }

    inline bool IsSpellQuicklyReady(
        Player* bot,
        uint32_t spellId,
        Unit* target,
        size_t slotIdx,
        float cachedTargetDist,
        uint32_t procsMask)
    {
        if (!spellId || !bot || !target || !target->IsInWorld() || !target->IsAlive())
            return false;

        if (bot->HasSpellCooldown(spellId))
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return false;

        if (spellInfo->CheckShapeshift(bot->GetShapeshiftForm()) != SPELL_CAST_OK)
            return false;

        // Verificar coste de poder (Maná/Ira/Energía/Runic Power)
        if (spellInfo->PowerType < MAX_POWERS)
        {
            Powers powerType = static_cast<Powers>(spellInfo->PowerType);
            int32_t neededPower = spellInfo->CalcPowerCost(bot, spellInfo->GetSchoolMask());
            if (static_cast<int32_t>(bot->GetPower(powerType)) < neededPower)
                return false;
        }

        // Verificar runas disponibles en Caballero de la Muerte sin instanciar `new Spell`
        if (spellInfo->PowerType == POWER_RUNE && spellInfo->RuneCostID && bot->getClass() == CLASS_DEATH_KNIGHT)
        {
            if (SpellRuneCostEntry const* src = sSpellRuneCostStore.LookupEntry(spellInfo->RuneCostID))
            {
                if (!src->NoRuneCost())
                {
                    int32_t runeCost[NUM_RUNE_TYPES]{};
                    for (uint32_t r = 0; r < RUNE_DEATH; ++r)
                        runeCost[r] = static_cast<int32_t>(src->RuneCost[r]);
                    runeCost[RUNE_DEATH] = MAX_RUNES;

                    for (uint32_t i = 0; i < MAX_RUNES; ++i)
                    {
                        RuneType rt = bot->GetCurrentRune(i);
                        if (bot->GetRuneCooldown(i) == 0 && runeCost[rt] > 0)
                            --runeCost[rt];
                    }
                    for (uint32_t r = 0; r < RUNE_DEATH; ++r)
                    {
                        if (runeCost[r] > 0)
                            runeCost[RUNE_DEATH] += runeCost[r];
                    }
                    if (runeCost[RUNE_DEATH] > MAX_RUNES)
                        return false;
                }
            }
        }

        // Slice & Dice (5171/6774) y Savage Roar (52610) exigen al menos 1 punto de combo activo
        if ((spellId == 5171u || spellId == 6774u || spellId == 52610u) && bot->GetComboPoints() == 0)
            return false;

        // Evitar encolar golpes de "siguiente ataque melee" (Heroic Strike, Maul, Cleave) si ya hay uno activo
        if ((spellInfo->Attributes & (SPELL_ATTR0_ON_NEXT_SWING | SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE)) != 0)
        {
            if (bot->GetCurrentSpell(CURRENT_MELEE_SPELL) != nullptr)
                return false;
        }

        // Procs instantáneos que permiten lanzar mientras se camina aunque el hechizo base tenga tiempo de casteo
        bool procInstant = false;
        if (slotIdx == SLOT_INSTANT_PROC_2 && (bot->HasAura(57761u) || bot->HasAura(48108u)))
            procInstant = true;
        else if (slotIdx == SLOT_INSTANT_PROC_1 && (procsMask & ((1u << 2) | (1u << 3) | (1u << 8))) != 0)
            procInstant = true;
        else if ((procsMask & (1u << 11)) != 0 && (slotIdx == SLOT_HEAL_FAST_INSTANT || slotIdx == SLOT_INSTANT_PROC_1))
            procInstant = true;

        uint32_t castTime = !spellInfo->IsChanneled() ? spellInfo->CalcCastTime(bot) : spellInfo->GetDuration();
        if (!procInstant && (castTime > 0 || spellInfo->IsAutoRepeatRangedSpell()) && bot->isMoving())
            return false;

        if (target != bot)
        {
            bool positive = target->IsFriendlyTo(bot);
            float maxRange = spellInfo->GetMaxRange(positive, bot);
            float minRange = spellInfo->GetMinRange(positive);
            float d = (cachedTargetDist >= 0.0f) ? cachedTargetDist : bot->GetDistance(target);

            if (maxRange > 0.0f && maxRange <= 6.0f)
            {
                if (!bot->IsWithinMeleeRange(target))
                    return false;
            }
            else if (maxRange > 6.0f)
            {
                if (d > maxRange + 2.5f)
                    return false;
                if (minRange > 0.0f && d < minRange - 1.0f)
                    return false;
            }
            else
            {
                if ((slotIdx == SLOT_AOE_SPELL || slotIdx == SLOT_CORE_SIGNATURE || slotIdx == SLOT_PRIMARY_FILLER) && d > 8.5f)
                    return false;
            }

            if (target->IsImmunedToSpell(spellInfo) && spellId != 44572u)
                return false;
        }

        return true;
    }

    inline uint32_t FindReadyDispelSpellForMember(
        Player* bot,
        Unit* member,
        uint32_t const (&dispelIds)[2],
        uint32_t const (&dispelMasks)[2])
    {
        if (!bot || !member)
            return 0;

        uint32_t availableMasks[2]{};
        for (size_t i = 0; i < 2; ++i)
        {
            uint32_t sid = dispelIds[i];
            if (!sid || bot->HasSpellCooldown(sid))
                continue;
            SpellInfo const* sInfo = sSpellMgr->GetSpellInfo(sid);
            if (!sInfo)
                continue;
            if (sInfo->PowerType < MAX_POWERS)
            {
                Powers pt = static_cast<Powers>(sInfo->PowerType);
                if (static_cast<int32_t>(bot->GetPower(pt)) < sInfo->CalcPowerCost(bot, sInfo->GetSchoolMask()))
                    continue;
            }
            availableMasks[i] = dispelMasks[i];
        }
        if (!availableMasks[0] && !availableMasks[1])
            return 0;

        for (auto const& kv : member->GetAppliedAuras())
        {
            AuraApplication* app = kv.second;
            if (!app || app->IsPositive())
                continue;
            Aura* aura = app->GetBase();
            SpellInfo const* auraInfo = aura ? aura->GetSpellInfo() : nullptr;
            if (!auraInfo || !auraInfo->Dispel)
                continue;
            uint32_t bit = (1u << auraInfo->Dispel);
            for (size_t i = 0; i < 2; ++i)
            {
                if ((availableMasks[i] & bit) != 0)
                    return dispelIds[i];
            }
        }
        return 0;
    }

    struct BotSpellCacheSlot
    {
        uint64_t botGuid        = 0;
        uint8_t  level          = 0;
        uint8_t  specTab        = 0;
        uint8_t  classId        = 0;
        uint8_t  shapeshiftForm = 0;
        uint32_t spellIds[SLOT_MAX][MAX_SLOT_CANDIDATES]{};
        uint32_t dispelIds[2]{};
        uint32_t dispelMasks[2]{};
    };

    inline BotSpellCacheSlot& GetCachedBotSpellBook(Player* bot, AiObjectContext* ctx, uint8_t classId, uint8_t specTab)
    {
        uint8_t form = static_cast<uint8_t>(bot->GetShapeshiftForm());
        uint64_t botGuid = bot->GetGUID().GetRawValue();
        thread_local BotSpellCacheSlot spellCache[64]{};
        size_t idx = static_cast<size_t>(botGuid ^ (botGuid >> 17)) & 63u;
        BotSpellCacheSlot& entry = spellCache[idx];

        if (entry.botGuid != botGuid || entry.level != bot->GetLevel() ||
            entry.specTab != specTab || entry.classId != classId || entry.shapeshiftForm != form)
        {
            entry.botGuid        = botGuid;
            entry.level          = bot->GetLevel();
            entry.specTab        = specTab;
            entry.classId        = classId;
            entry.shapeshiftForm = form;
            ResolveClassSpellBookIds(bot, ctx, classId, specTab, form, entry.spellIds);
            ResolveDispelBookIds(bot, classId, entry.dispelIds, entry.dispelMasks);
        }
        return entry;
    }

    inline void PopulateClassSpellSlots(
        Player* bot,
        AiObjectContext* ctx,
        BotSpellCacheSlot const& entry,
        uint8_t classId,
        Unit* enemyTarget,
        Unit* lowestAllyUnit,
        CombatSnapshotPOD& snap)
    {
        bool anyKnownDotMissing = false;
        bool anyKnownDotPresent = false;
        uint8_t form = entry.shapeshiftForm;
        float enemyDist = (enemyTarget && enemyTarget->IsInWorld()) ? bot->GetDistance(enemyTarget) : -1.0f;
        float allyDist  = (lowestAllyUnit && lowestAllyUnit->IsInWorld())
                              ? ((lowestAllyUnit == bot) ? 0.0f : bot->GetDistance(lowestAllyUnit))
                              : -1.0f;

        for (size_t s = 0; s < SLOT_MAX; ++s)
        {
            Unit* slotTarget = enemyTarget;
            float cachedDist = enemyDist;
            if (s == SLOT_BURST_COOLDOWN || s == SLOT_SUMMON_OR_MIRROR ||
                s == SLOT_DEFENSIVE_BARRIER || s == SLOT_EMERGENCY_SAVE ||
                (s == SLOT_INSTANT_PROC_1 && (classId == CLASS_ROGUE || (classId == CLASS_DRUID && form == FORM_CAT))))
            {
                slotTarget = bot;
                cachedDist = 0.0f;
            }
            else if (s == SLOT_HEAL_FAST_INSTANT || s == SLOT_HEAL_BIG_DIRECT || s == SLOT_HEAL_GROUP_CHAIN)
            {
                slotTarget = lowestAllyUnit;
                cachedDist = allyDist;
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
                    snap.has_defensive_barrier = 1;
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

                if (slotTarget && IsSpellQuicklyReady(bot, candId, slotTarget, s, cachedDist, snap.active_procs_mask))
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

    inline bool BuildCombatSnapshot(PlayerbotAI* botAI, AiObjectContext* ctx, CombatSnapshotPOD& snap, uint8_t evalPhase = 0)
    {
        if (!botAI || !ctx)
            return false;

        Player* bot = botAI->GetBot();
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || (botAI->GetState() != BOT_STATE_COMBAT && !bot->IsInCombat()))
            return false;

        uint64_t botGuid = bot->GetGUID().GetRawValue();
        uint64_t nowMs   = static_cast<uint64_t>(getMSTime());

        thread_local uint64_t s_cachedBotGuid = 0;
        thread_local uint64_t s_cachedNowMs   = 0;
        thread_local CombatSnapshotPOD s_cachedSnap{};

        if (evalPhase != 0 && s_cachedBotGuid == botGuid && s_cachedNowMs == nowMs)
        {
            snap = s_cachedSnap;
            snap.eval_phase = evalPhase;
            return true;
        }

        std::memset(&snap, 0, sizeof(CombatSnapshotPOD));
        snap.abi_version   = JCC_ABI_VERSION;
        snap.struct_size   = sizeof(CombatSnapshotPOD);
        snap.bot_guid      = botGuid;
        snap.map_id        = bot->GetMapId();
        snap.instance_id   = bot->GetInstanceId();
        snap.now_ms        = nowMs;
        snap.eval_phase    = evalPhase;
        snap.class_id      = bot->getClass();
        snap.spec_id       = static_cast<uint8_t>(AiFactory::GetPlayerSpecTab(bot));
        snap.level         = bot->GetLevel();
        snap.target_in_los = 1;

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

        BotSpellCacheSlot const& spellBook = GetCachedBotSpellBook(bot, ctx, snap.class_id, snap.spec_id);

        Unit* lowestAllyUnit = bot;
        snap.lowest_ally_guid       = snap.bot_guid;
        snap.lowest_ally_hp_pct     = snap.bot_hp_pct;
        snap.lowest_ally_missing_hp = (bot->GetMaxHealth() > bot->GetHealth()) ? (bot->GetMaxHealth() - bot->GetHealth()) : 0;
        snap.lowest_ally_is_tank    = (snap.role == 0) ? 1 : 0;
        snap.lowest_ally_in_los     = 1;
        snap.injured_ally_count     = (snap.bot_hp_pct < 92.0f) ? 1 : 0;

        Group* grp = bot->GetGroup();
        if (grp)
        {
            snap.group_guid = grp->GetGUID().GetRawValue();
            uint8_t  injuredCount      = 0;
            float    bestInLosScore    = 200.0f;
            float    lowestInLosPct    = 100.0f;
            uint64_t lowestInLosGuid   = 0;
            uint32_t lowestInLosMiss   = 0;
            uint8_t  lowestInLosTank   = 0;
            Unit*    lowestInLosUnit   = nullptr;

            float    lowestOutPct      = 100.0f;
            uint64_t lowestOutGuid     = 0;
            uint32_t lowestOutMiss     = 0;
            uint8_t  lowestOutTank     = 0;
            Unit*    lowestOutUnit     = nullptr;

            for (GroupReference* ref = grp->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || !member->IsInWorld() || !member->IsAlive() || member->GetMapId() != bot->GetMapId())
                    continue;
                float dist = (member == bot) ? 0.0f : bot->GetDistance2d(member);
                if (dist > 40.0f)
                    continue;

                float hpPct = member->GetHealthPct();
                if (hpPct < 92.0f)
                    ++injuredCount;

                bool inLos  = (member == bot) || bot->IsWithinLOSInMap(member);
                bool isTank = PlayerbotAI::IsTank(member);
                uint32_t missing = (member->GetMaxHealth() > member->GetHealth()) ? (member->GetMaxHealth() - member->GetHealth()) : 0;

                if (inLos && dist <= 30.0f && snap.dispel_ally_guid == 0 && (spellBook.dispelIds[0] || spellBook.dispelIds[1]))
                {
                    if (uint32_t dSpell = FindReadyDispelSpellForMember(bot, member, spellBook.dispelIds, spellBook.dispelMasks))
                    {
                        snap.dispel_spell_id  = dSpell;
                        snap.dispel_ally_guid = member->GetGUID().GetRawValue();
                    }
                }

                if (inLos)
                {
                    float score = hpPct - (isTank ? 12.0f : 0.0f);
                    if (!lowestInLosUnit || score < bestInLosScore)
                    {
                        bestInLosScore  = score;
                        lowestInLosPct  = hpPct;
                        lowestInLosGuid = member->GetGUID().GetRawValue();
                        lowestInLosMiss = missing;
                        lowestInLosTank = isTank ? 1 : 0;
                        lowestInLosUnit = member;
                    }
                }
                else if (!lowestOutUnit || hpPct < lowestOutPct)
                {
                    lowestOutPct  = hpPct;
                    lowestOutGuid = member->GetGUID().GetRawValue();
                    lowestOutMiss = missing;
                    lowestOutTank = isTank ? 1 : 0;
                    lowestOutUnit = member;
                }
            }

            snap.injured_ally_count = injuredCount;
            if (lowestInLosUnit && (lowestInLosPct < 92.0f || !lowestOutUnit || lowestOutPct >= 80.0f))
            {
                lowestAllyUnit              = lowestInLosUnit;
                snap.lowest_ally_guid       = lowestInLosGuid;
                snap.lowest_ally_hp_pct     = lowestInLosPct;
                snap.lowest_ally_missing_hp = lowestInLosMiss;
                snap.lowest_ally_is_tank    = lowestInLosTank;
                snap.lowest_ally_in_los     = 1;
            }
            else if (lowestOutUnit)
            {
                lowestAllyUnit              = lowestOutUnit;
                snap.lowest_ally_guid       = lowestOutGuid;
                snap.lowest_ally_hp_pct     = lowestOutPct;
                snap.lowest_ally_missing_hp = lowestOutMiss;
                snap.lowest_ally_is_tank    = lowestOutTank;
                snap.lowest_ally_in_los     = 0;
            }
        }

        // Detectar Crowd Control rompible cercano (<= 12y del target/bot) y mob suelto atacando a sanador/caster (para Tanque)
        uint8_t ccNearby = 0;
        GuidVector const& possibleTargets = ctx->GetValue<GuidVector>("possible targets")->Get();
        Unit* ccAnchor = target ? target : static_cast<Unit*>(bot);
        for (ObjectGuid const& g : possibleTargets)
        {
            if (target && g == target->GetGUID())
                continue;
            Unit* u = botAI->GetUnit(g);
            if (!u || !u->IsInWorld() || !u->IsAlive() || u->IsFriendlyTo(bot))
                continue;
            if (ccAnchor->GetDistance2d(u) <= 12.0f && u->HasBreakableByDamageCrowdControlAura())
            {
                if (ccNearby < 255)
                    ++ccNearby;
            }
        }
        snap.nearby_cc_count = ccNearby;

        if (snap.role == 0 && grp)
        {
            uint64_t nonHealerLooseGuid = 0;
            GuidVector const& attackers = ctx->GetValue<GuidVector>("attackers")->Get();
            for (ObjectGuid const& g : attackers)
            {
                Unit* att = botAI->GetUnit(g);
                if (!att || !att->IsInWorld() || !att->IsAlive() || att->IsFriendlyTo(bot))
                    continue;
                if (bot->GetDistance2d(att) > 30.0f)
                    continue;
                Unit* vic = att->GetVictim();
                Player* vp = vic ? vic->ToPlayer() : nullptr;
                if (!vp || vp == bot || PlayerbotAI::IsTank(vp) || !grp->IsMember(vp->GetGUID()))
                    continue;
                if (PlayerbotAI::IsHeal(vp))
                {
                    snap.loose_mob_on_healer_guid = att->GetGUID().GetRawValue();
                    break;
                }
                if (!nonHealerLooseGuid)
                    nonHealerLooseGuid = att->GetGUID().GetRawValue();
            }
            if (snap.loose_mob_on_healer_guid == 0)
                snap.loose_mob_on_healer_guid = nonHealerLooseGuid;
        }

        if (target && target->IsInWorld() && target->IsAlive() && !target->IsFriendlyTo(bot))
        {
            snap.target_guid               = target->GetGUID().GetRawValue();
            snap.target_hp_pct             = target->GetHealthPct();
            snap.target_distance           = bot->GetDistance(target);
            snap.target_in_los             = bot->IsWithinLOSInMap(target) ? 1 : 0;
            snap.target_has_breakable_cc   = target->HasBreakableByDamageCrowdControlAura() ? 1 : 0;
            snap.combo_points              = bot->GetComboPoints(target);

            bool isCastingInterruptible = false;
            if (target->IsNonMeleeSpellCast(true))
            {
                Spell* currentCasting = target->GetCurrentSpell(CURRENT_GENERIC_SPELL);
                if (!currentCasting)
                    currentCasting = target->GetCurrentSpell(CURRENT_CHANNELED_SPELL);
                if (currentCasting && currentCasting->GetSpellInfo())
                {
                    isCastingInterruptible = (currentCasting->GetSpellInfo()->InterruptFlags & SPELL_INTERRUPT_FLAG_INTERRUPT) != 0;
                    int32_t totalMs = currentCasting->GetCastTime();
                    int32_t remMs   = currentCasting->GetCastTimeRemaining();
                    if (totalMs > 0)
                    {
                        snap.enemy_cast_total_ms   = static_cast<uint32_t>(totalMs);
                        snap.enemy_cast_elapsed_ms = (remMs >= 0 && totalMs > remMs) ? static_cast<uint32_t>(totalMs - remMs) : 0;
                    }
                }
                else
                {
                    isCastingInterruptible = true;
                }
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

        // Procs, enfermedades y auras multi-clase (14 bits)
        uint32_t procs = 0;
        if (snap.class_id == CLASS_MAGE)
        {
            bool hasFoF = bot->HasAura(44544u) || (target && (target->HasAura(122u) || target->HasAura(44572u) || target->HasAura(33395u)));
            snap.target_is_frozen_or_fof = hasFoF ? 1 : 0;
            snap.has_brain_freeze        = bot->HasAura(57761u) ? 1 : 0;
            snap.has_missile_barrage     = bot->HasAura(44401u) ? 1 : 0;
            snap.has_hot_streak          = bot->HasAura(48108u) ? 1 : 0;
            snap.has_ice_barrier         = (bot->HasAura(11426u) || bot->HasAura(43039u)) ? 1 : 0;
            snap.has_target_dot          = (target && (target->HasAura(44457u) || target->HasAura(55360u))) ? 1 : 0;
            if (snap.has_hot_streak)
                procs |= (1u << 12);
            if (bot->HasAura(12536u))
                procs |= (1u << 10);
        }
        else if (snap.class_id == CLASS_DRUID)
        {
            if (bot->HasAura(48517u)) procs |= (1u << 0);  // Solar Eclipse
            if (bot->HasAura(48518u)) procs |= (1u << 1);  // Lunar Eclipse
            if (bot->HasAura(52610u)) procs |= (1u << 9);  // Savage Roar
            if (bot->HasAura(16870u)) procs |= (1u << 10); // Clearcasting
            if (bot->HasAura(69369u)) procs |= (1u << 11); // Predatory Strikes
        }
        else if (snap.class_id == CLASS_SHAMAN)
        {
            if (Aura* ms = bot->GetAura(53817u))
            {
                if (ms->GetStackAmount() >= 5)
                    procs |= (1u << 2); // Maelstrom Weapon x5
            }
            if (bot->HasAura(974u) || bot->HasAura(49284u))
                procs |= (1u << 13);
        }
        else if (snap.class_id == CLASS_PALADIN)
        {
            if (bot->HasAura(59578u)) procs |= (1u << 3);  // The Art of War
            if (bot->HasAura(53563u)) procs |= (1u << 13); // Beacon of Light
        }
        else if (snap.class_id == CLASS_DEATH_KNIGHT)
        {
            if (bot->HasAura(51124u)) procs |= (1u << 4);  // Killing Machine
            if (bot->HasAura(59052u)) procs |= (1u << 5);  // Rime (Freezing Fog)
            if (target)
            {
                uint8_t dmask = 0;
                if (target->HasAura(55095u, bot->GetGUID())) dmask |= 0x01; // Frost Fever
                if (target->HasAura(55078u, bot->GetGUID())) dmask |= 0x02; // Blood Plague
                if (target->HasAura(51735u) || target->HasAura(51726u)) dmask |= 0x04;
                snap.target_disease_mask = dmask;
            }
        }
        else if (snap.class_id == CLASS_WARRIOR)
        {
            if (bot->HasAura(52437u) || bot->HasAura(60503u) || bot->HasAura(46916u))
                procs |= (1u << 6); // Sudden Death / Taste for Blood / Bloodsurge
            if (bot->HasAura(50227u))
                procs |= (1u << 12); // Sword and Board
        }
        else if (snap.class_id == CLASS_HUNTER)
        {
            if (bot->HasAura(56453u))
                procs |= (1u << 7); // Lock and Load
        }
        else if (snap.class_id == CLASS_WARLOCK)
        {
            if (bot->HasAura(47383u) || bot->HasAura(71165u) || bot->HasAura(63167u) || bot->HasAura(34936u) || bot->HasAura(17941u))
                procs |= (1u << 8); // Molten Core / Decimation / Backlash / Nightfall
        }
        else if (snap.class_id == CLASS_ROGUE)
        {
            if (bot->HasAura(5171u) || bot->HasAura(6774u))
                procs |= (1u << 9); // Slice and Dice
        }
        else if (snap.class_id == CLASS_PRIEST)
        {
            if (bot->HasAura(33151u) || bot->HasAura(63734u))
                procs |= (1u << 11); // Surge of Light / Serendipity
        }
        snap.active_procs_mask = procs;

        PopulateClassSpellSlots(bot, ctx, spellBook, snap.class_id, target, lowestAllyUnit, snap);

        s_cachedBotGuid = snap.bot_guid;
        s_cachedNowMs   = snap.now_ms;
        s_cachedSnap    = snap;
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

    /// Modo Activo (F3/F5): Ejecutado por `RustTacticalAction` (29.5f) y `RustRotationAction` (19.5f).
    inline bool TryExecuteActiveDecision(PlayerbotAI* botAI, AiObjectContext* ctx, uint32_t mode, uint8_t evalPhase)
    {
        if (mode < 2 || !EnsureInitialized())
            return false;

        Player* bot = botAI->GetBot();
        if (!bot || !bot->GetSession() || bot->GetSession()->IsLoggingOut())
            return false;

        if (mode == 2 && bot->getClass() != CLASS_MAGE)
            return false;

        CombatSnapshotPOD snap;
        if (!BuildCombatSnapshot(botAI, ctx, snap, evalPhase) || !snap.is_pve)
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
                    "[jAInaCore-RustCombat][ACTIVE] Executions={} AvgLatency={:.3f}us | Bot={} Class={} Spec={} Phase={} SpellId={} Category={} Slot={}",
                    execs,
                    avgUs,
                    bot->GetName(),
                    static_cast<uint32_t>(snap.class_id),
                    static_cast<uint32_t>(snap.spec_id),
                    static_cast<uint32_t>(evalPhase),
                    dec.spell_id,
                    static_cast<uint32_t>(dec.decision_category),
                    static_cast<uint32_t>(dec.slot_index));
            }
            return true;
        }
        return false;
    }

    /// Fase Táctica ABI v3 (`ACTION_MOVE - 0.5f = 29.5f`): Emergencia, Interrupción, Dispel, Triaje Healer y Taunt/Peel.
    class RustTacticalAction : public Action
    {
    public:
        explicit RustTacticalAction(PlayerbotAI* botAI) : Action(botAI, "rust combat tactical") {}

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
            return TryExecuteActiveDecision(botAI, botAI->GetAiObjectContext(), sPlayerbotAIConfig.rustCombatMode, 1);
        }
    };

    /// Fase de Rotación ABI v3 (`ACTION_HIGH - 0.5f = 19.5f`): Permite ejecutar utilidades C++ `ACTION_HIGH = 20.0f` primero.
    class RustRotationAction : public Action
    {
    public:
        explicit RustRotationAction(PlayerbotAI* botAI) : Action(botAI, "rust combat rotation") {}

        bool isUseful() override
        {
            Player* bot = botAI ? botAI->GetBot() : nullptr;
            return bot && botAI->GetState() == BOT_STATE_COMBAT &&
                   !bot->InBattleground() && !bot->InArena() &&
                   !PlayerbotAI::IsHeal(bot) &&
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
            return TryExecuteActiveDecision(botAI, botAI->GetAiObjectContext(), sPlayerbotAIConfig.rustCombatMode, 2);
        }
    };

    /// Modo Sombra (F2): Calcula la decisión en Rust pasando `nullptr` como pizarra (100% libre de efectos secundarios),
    /// mide la latencia en nanosegundos y registra discrepancias muestreadas.
    inline void ObserveShadowDecision(PlayerbotAI* botAI, AiObjectContext* ctx, Action* cppAction)
    {
        if (!cppAction || !EnsureInitialized())
            return;

        CombatSnapshotPOD snap;
        if (!BuildCombatSnapshot(botAI, ctx, snap, 0))
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
