/*
 * Project Jaina — jAInaCore Rust Combat Core Bridge (F2 Shadow Mode / F3 Pilot / F4 Squad Blackboard)
 * Connects mod-playerbots Engine::DoNextAction to jaina_combat_core.dll via versioned C-ABI POD structs.
 *
 * Design Invariants (PLAN_JAINA_COMBAT_CORE_RUST):
 * 1. Stateless & Re-entrant Rust: SquadBlackboardPOD (600B) is owned by C++ per map worker thread (thread_local),
 *    eliminating all Mutexes and preventing unbounded HashMap memory growth.
 * 2. Engine Action & Multiplier Architecture (R2): Integrated as `RustCombatAction` inside `Engine::DoNextAction`'s
 *    while-loop at relevance `ACTION_MOVE - 1.0f` (29.0f), so all Multipliers (BossAI, Passive) and all high-priority
 *    actions (`>= 30.0f`: Emergency 90, Raid 60, Dispel 50, Interrupt 40, Move 30) are evaluated first.
 * 3. Per-Bot Spell ID Cache: Resolves `"spell id"` strings once per (bot_guid, level, spec_id) and caches the `uint32_t`
 *    IDs on the worker thread so subsequent ticks perform zero string map lookups.
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

    inline uint32_t ResolveKnownSpellId(Player* bot, AiObjectContext* ctx, char const* spellName)
    {
        if (!bot || !ctx || !spellName || !*spellName)
            return 0;

        uint32_t spellId = ctx->GetValue<uint32>("spell id", spellName)->Get();
        if (!spellId)
            return 0;

        // Regla R3: Nunca pasar hechizos de mascota a CastSpell (evita alternar autocast de mascota)
        if (Pet* pet = bot->GetPet())
        {
            if (pet->HasSpell(spellId))
                return 0;
        }

        if (!bot->HasSpell(spellId))
            return 0;

        return spellId;
    }

    inline void ResolveClassSpellBookIds(Player* bot, AiObjectContext* ctx, uint8_t classId, uint8_t specTab, uint32_t (&outSpells)[SLOT_MAX])
    {
        std::memset(outSpells, 0, sizeof(outSpells));
        switch (classId)
        {
            case CLASS_MAGE:
            {
                char const* filler = (specTab == MAGE_TAB_ARCANE) ? "arcane blast" : (specTab == MAGE_TAB_FIRE ? "fireball" : "frostbolt");
                outSpells[SLOT_PRIMARY_FILLER]    = ResolveKnownSpellId(bot, ctx, filler);
                if (!outSpells[SLOT_PRIMARY_FILLER])
                    outSpells[SLOT_PRIMARY_FILLER] = ResolveKnownSpellId(bot, ctx, "frostbolt");

                outSpells[SLOT_INSTANT_PROC_1]    = ResolveKnownSpellId(bot, ctx, "ice lance");
                uint32_t ffb = ResolveKnownSpellId(bot, ctx, "frostfire bolt");
                outSpells[SLOT_INSTANT_PROC_2]    = ffb ? ffb : ResolveKnownSpellId(bot, ctx, "fireball");
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "deep freeze");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "fire blast");
                outSpells[SLOT_BURST_COOLDOWN]    = ResolveKnownSpellId(bot, ctx, "icy veins");
                outSpells[SLOT_SUMMON_OR_MIRROR]  = ResolveKnownSpellId(bot, ctx, "mirror image");
                outSpells[SLOT_DEFENSIVE_BARRIER] = ResolveKnownSpellId(bot, ctx, "ice barrier");
                outSpells[SLOT_INTERRUPT_SPELL]   = ResolveKnownSpellId(bot, ctx, "counterspell");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "blizzard");
                outSpells[SLOT_DOT_MAINTENANCE]   = ResolveKnownSpellId(bot, ctx, "living bomb");
                outSpells[SLOT_EMERGENCY_SAVE]    = ResolveKnownSpellId(bot, ctx, "evocation");
                break;
            }
            case CLASS_WARLOCK:
            {
                outSpells[SLOT_PRIMARY_FILLER]    = ResolveKnownSpellId(bot, ctx, "shadow bolt");
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "haunt");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "chaos bolt");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "shadowburn");
                outSpells[SLOT_DOT_MAINTENANCE]   = ResolveKnownSpellId(bot, ctx, "corruption");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "seed of corruption");
                break;
            }
            case CLASS_PRIEST:
            {
                outSpells[SLOT_PRIMARY_FILLER]    = ResolveKnownSpellId(bot, ctx, "mind flay");
                if (!outSpells[SLOT_PRIMARY_FILLER])
                    outSpells[SLOT_PRIMARY_FILLER] = ResolveKnownSpellId(bot, ctx, "smite");
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "mind blast");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "shadow word: death");
                outSpells[SLOT_DEFENSIVE_BARRIER] = ResolveKnownSpellId(bot, ctx, "power word: shield");
                outSpells[SLOT_DOT_MAINTENANCE]   = ResolveKnownSpellId(bot, ctx, "shadow word: pain");
                outSpells[SLOT_HEAL_FAST_INSTANT] = ResolveKnownSpellId(bot, ctx, "flash heal");
                outSpells[SLOT_HEAL_BIG_DIRECT]   = ResolveKnownSpellId(bot, ctx, "greater heal");
                outSpells[SLOT_HEAL_GROUP_CHAIN]  = ResolveKnownSpellId(bot, ctx, "prayer of healing");
                break;
            }
            case CLASS_SHAMAN:
            {
                outSpells[SLOT_PRIMARY_FILLER]    = ResolveKnownSpellId(bot, ctx, "lightning bolt");
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "lava burst");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "stormstrike");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "earth shock");
                outSpells[SLOT_INTERRUPT_SPELL]   = ResolveKnownSpellId(bot, ctx, "wind shear");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "chain lightning");
                outSpells[SLOT_DOT_MAINTENANCE]   = ResolveKnownSpellId(bot, ctx, "flame shock");
                outSpells[SLOT_HEAL_FAST_INSTANT] = ResolveKnownSpellId(bot, ctx, "riptide");
                if (!outSpells[SLOT_HEAL_FAST_INSTANT])
                    outSpells[SLOT_HEAL_FAST_INSTANT] = ResolveKnownSpellId(bot, ctx, "lesser healing wave");
                outSpells[SLOT_HEAL_BIG_DIRECT]   = ResolveKnownSpellId(bot, ctx, "healing wave");
                outSpells[SLOT_HEAL_GROUP_CHAIN]  = ResolveKnownSpellId(bot, ctx, "chain heal");
                break;
            }
            case CLASS_WARRIOR:
            {
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "shield slam");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "mortal strike");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "bloodthirst");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "execute");
                outSpells[SLOT_DEFENSIVE_BARRIER] = ResolveKnownSpellId(bot, ctx, "shield block");
                outSpells[SLOT_INTERRUPT_SPELL]   = ResolveKnownSpellId(bot, ctx, "pummel");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "thunder clap");
                outSpells[SLOT_EMERGENCY_SAVE]    = ResolveKnownSpellId(bot, ctx, "shield wall");
                outSpells[SLOT_TAUNT_SPELL]       = ResolveKnownSpellId(bot, ctx, "taunt");
                break;
            }
            case CLASS_PALADIN:
            {
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "crusader strike");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "judgement of light");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "hammer of wrath");
                outSpells[SLOT_INTERRUPT_SPELL]   = ResolveKnownSpellId(bot, ctx, "hammer of justice");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "consecration");
                outSpells[SLOT_EMERGENCY_SAVE]    = ResolveKnownSpellId(bot, ctx, "divine shield");
                outSpells[SLOT_HEAL_FAST_INSTANT] = ResolveKnownSpellId(bot, ctx, "flash of light");
                outSpells[SLOT_HEAL_BIG_DIRECT]   = ResolveKnownSpellId(bot, ctx, "holy light");
                outSpells[SLOT_TAUNT_SPELL]       = ResolveKnownSpellId(bot, ctx, "hand of reckoning");
                break;
            }
            case CLASS_ROGUE:
            {
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "sinister strike");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "eviscerate");
                outSpells[SLOT_INTERRUPT_SPELL]   = ResolveKnownSpellId(bot, ctx, "kick");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "fan of knives");
                outSpells[SLOT_EMERGENCY_SAVE]    = ResolveKnownSpellId(bot, ctx, "cloak of shadows");
                break;
            }
            case CLASS_HUNTER:
            {
                outSpells[SLOT_PRIMARY_FILLER]    = ResolveKnownSpellId(bot, ctx, "steady shot");
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "aimed shot");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "arcane shot");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "kill shot");
                outSpells[SLOT_DOT_MAINTENANCE]   = ResolveKnownSpellId(bot, ctx, "serpent sting");
                outSpells[SLOT_AOE_SPELL]         = ResolveKnownSpellId(bot, ctx, "multi-shot");
                break;
            }
            case CLASS_DEATH_KNIGHT:
            {
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "scourge strike");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "heart strike");
                outSpells[SLOT_EXECUTE_FINISHER]  = ResolveKnownSpellId(bot, ctx, "death coil");
                outSpells[SLOT_INTERRUPT_SPELL]   = ResolveKnownSpellId(bot, ctx, "mind freeze");
                outSpells[SLOT_EMERGENCY_SAVE]    = ResolveKnownSpellId(bot, ctx, "icebound fortitude");
                outSpells[SLOT_TAUNT_SPELL]       = ResolveKnownSpellId(bot, ctx, "dark command");
                break;
            }
            case CLASS_DRUID:
            {
                outSpells[SLOT_PRIMARY_FILLER]    = ResolveKnownSpellId(bot, ctx, "wrath");
                outSpells[SLOT_CORE_SIGNATURE]    = ResolveKnownSpellId(bot, ctx, "mangle (cat)");
                if (!outSpells[SLOT_CORE_SIGNATURE])
                    outSpells[SLOT_CORE_SIGNATURE] = ResolveKnownSpellId(bot, ctx, "starfire");
                outSpells[SLOT_DOT_MAINTENANCE]   = ResolveKnownSpellId(bot, ctx, "moonfire");
                outSpells[SLOT_DEFENSIVE_BARRIER] = ResolveKnownSpellId(bot, ctx, "barkskin");
                outSpells[SLOT_HEAL_FAST_INSTANT] = ResolveKnownSpellId(bot, ctx, "rejuvenation");
                outSpells[SLOT_HEAL_BIG_DIRECT]   = ResolveKnownSpellId(bot, ctx, "nourish");
                outSpells[SLOT_HEAL_GROUP_CHAIN]  = ResolveKnownSpellId(bot, ctx, "wild growth");
                outSpells[SLOT_TAUNT_SPELL]       = ResolveKnownSpellId(bot, ctx, "growl");
                break;
            }
            default:
                break;
        }
    }

    inline void PopulateClassSpellSlots(Player* bot, AiObjectContext* ctx, uint8_t classId, uint8_t specTab, CombatSnapshotPOD& snap)
    {
        struct BotSpellCacheSlot
        {
            uint64_t botGuid = 0;
            uint8_t  level   = 0;
            uint8_t  specTab = 0;
            uint8_t  classId = 0;
            uint32_t spellIds[SLOT_MAX]{};
        };

        thread_local BotSpellCacheSlot spellCache[64]{};
        size_t idx = static_cast<size_t>(snap.bot_guid ^ (snap.bot_guid >> 17)) & 63u;
        BotSpellCacheSlot& entry = spellCache[idx];

        if (entry.botGuid != snap.bot_guid || entry.level != snap.level || entry.specTab != specTab || entry.classId != classId)
        {
            entry.botGuid = snap.bot_guid;
            entry.level   = snap.level;
            entry.specTab = specTab;
            entry.classId = classId;
            ResolveClassSpellBookIds(bot, ctx, classId, specTab, entry.spellIds);
        }

        for (size_t s = 0; s < SLOT_MAX; ++s)
        {
            uint32_t spellId = entry.spellIds[s];
            snap.ready_spells[s] = (spellId && !bot->HasSpellCooldown(spellId)) ? spellId : 0u;
        }
    }

    inline bool BuildCombatSnapshot(PlayerbotAI* botAI, AiObjectContext* ctx, CombatSnapshotPOD& snap)
    {
        if (!botAI || !ctx)
            return false;

        Player* bot = botAI->GetBot();
        if (!bot || !bot->IsInWorld() || !bot->IsAlive() || !bot->IsInCombat())
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
        {
            if (!bot->getAttackers().empty())
                target = *bot->getAttackers().begin();
        }

        bool isPvP = bot->InBattleground() || bot->InArena() || (target && target->IsPlayer());
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

        if (Group* grp = bot->GetGroup())
        {
            snap.group_guid = grp->GetGUID().GetRawValue();
            uint8_t injuredCount = 0;
            float lowestPct = 100.0f;
            uint64_t lowestGuid = 0;
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
                if (hpPct < lowestPct)
                {
                    lowestPct = hpPct;
                    lowestGuid = member->GetGUID().GetRawValue();
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

        PopulateClassSpellSlots(bot, ctx, snap.class_id, snap.spec_id, snap);
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
            if (execs % sampleRate == 0)
            {
                uint64_t totalNs = GetState().totalLatencyNs.load(std::memory_order_relaxed);
                double avgUs = execs > 0 ? (static_cast<double>(totalNs) / static_cast<double>(execs)) / 1000.0 : 0.0;
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

    /// Acción nativa de `mod-playerbots` (R2 y Sección 3.10): se encola con relevancia `ACTION_MOVE - 1.0f` (`29.0f`)
    /// para evaluarse dentro del bucle `while` de `Engine::DoNextAction`, respetando todos los `Multipliers` y todas
    /// las acciones `>= 30.0f` (Emergencias 90, BossAI 60, Dispel 50, Interrupciones 40, Movimiento 30).
    class RustCombatAction : public Action
    {
    public:
        explicit RustCombatAction(PlayerbotAI* botAI) : Action(botAI, "rust combat core") {}

        bool isUseful() override
        {
            return botAI && botAI->GetState() == BOT_STATE_COMBAT &&
                   IsBotControlledByRust(botAI->GetBot(), sPlayerbotAIConfig.rustCombatMode);
        }

        bool isPossible() override
        {
            Player* bot = botAI ? botAI->GetBot() : nullptr;
            return bot && bot->IsAlive() && bot->IsInWorld() && bot->IsInCombat() &&
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
