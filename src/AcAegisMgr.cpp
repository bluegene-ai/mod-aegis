#include "AcAegisMgr.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "AccountMgr.h"
#include "BanMgr.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraDefines.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

namespace
{
    float Dist2D(AegisMoveSample const& a, AegisMoveSample const& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float Dist3D(AegisMoveSample const& a, AegisMoveSample const& b)
    {
        float dx = a.x - b.x;
        float dy = a.y - b.y;
        float dz = a.z - b.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float Dist3DToPoint(float ax, float ay, float az, float bx, float by, float bz)
    {
        float dx = ax - bx;
        float dy = ay - by;
        float dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float MaxAxisDelta(AegisMoveSample const& a, AegisMoveSample const& b)
    {
        return std::max({ std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z) });
    }

    float ClampRiskDelta(float value)
    {
        return std::clamp(value, 0.0f, sAcAegisConfig->Get().riskMaxDeltaPerMove);
    }

    struct QueuedLogLine
    {
        std::string path;
        std::string line;
        bool prepareOnly = false;
    };

    class AsyncFileAppender
    {
    public:
        AsyncFileAppender() : _worker(&AsyncFileAppender::Run, this) { }

        ~AsyncFileAppender()
        {
            Shutdown();
        }

        void PreparePath(std::string const& path)
        {
            Enqueue(path, std::string(), true);
        }

        void Append(std::string const& path, std::string const& line)
        {
            Enqueue(path, line, false);
        }

    private:
        void Enqueue(std::string const& path, std::string const& line,
            bool prepareOnly)
        {
            if (path.empty())
                return;

            {
                std::lock_guard<std::mutex> lock(_mutex);
                _queue.push_back({ path, line, prepareOnly });
            }

            _condition.notify_one();
        }

        void Shutdown()
        {
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_stopping)
                    return;

                _stopping = true;
            }

            _condition.notify_one();
            if (_worker.joinable())
                _worker.join();

            FlushAll();
        }

        void Run()
        {
            for (;;)
            {
                QueuedLogLine entry;

                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    _condition.wait(lock, [this]()
                    {
                        return _stopping || !_queue.empty();
                    });

                    if (_queue.empty())
                    {
                        if (_stopping)
                            break;

                        continue;
                    }

                    entry = std::move(_queue.front());
                    _queue.pop_front();
                }

                Write(entry);

                std::lock_guard<std::mutex> lock(_mutex);
                if (_queue.empty())
                    FlushAll();
            }
        }

        std::ofstream& EnsureStream(std::string const& path)
        {
            auto it = _streams.find(path);
            if (it != _streams.end() && it->second.is_open())
                return it->second;

            std::filesystem::path filePath(path);
            std::error_code error;
            std::filesystem::create_directories(filePath.parent_path(), error);

            std::ofstream& stream = _streams[path];
            if (!stream.is_open())
                stream.open(path, std::ios::out | std::ios::app);

            return stream;
        }

        void Write(QueuedLogLine const& entry)
        {
            std::ofstream& stream = EnsureStream(entry.path);
            if (!stream.is_open())
                return;

            if (entry.prepareOnly)
                return;

            stream << entry.line;
            if (entry.line.empty() || entry.line.back() != '\n')
                stream << '\n';
        }

        void FlushAll()
        {
            for (auto& [path, stream] : _streams)
            {
                (void)path;
                if (stream.is_open())
                    stream.flush();
            }
        }

    private:
        std::mutex _mutex;
        std::condition_variable _condition;
        std::deque<QueuedLogLine> _queue;
        std::unordered_map<std::string, std::ofstream> _streams;
        std::thread _worker;
        bool _stopping = false;
    };

    AsyncFileAppender& GetAsyncFileAppender()
    {
        static AsyncFileAppender appender;
        return appender;
    }

    bool ShouldCountOffenseForPunishStage(AegisPunishStage stage,
        AegisConfig const& cfg)
    {
        return cfg.offenseEnabled && stage != AegisPunishStage::Observe;
    }

    bool ShouldPersistOffense(AegisPunishStage stage, bool shouldRollback,
        AegisConfig const& cfg)
    {
        if (!cfg.offenseEnabled)
            return false;

        if (cfg.offenseCountOnlyOnPunish)
            return stage != AegisPunishStage::Observe;

        return shouldRollback || stage != AegisPunishStage::Observe;
    }

    bool PersistentPunishStateChanged(AegisPunishState const& before,
        AegisPunishState const& after)
    {
        return before.offenseCount != after.offenseCount ||
            before.offenseTier != after.offenseTier ||
            before.punishStage != after.punishStage ||
            before.debuffUntilEpoch != after.debuffUntilEpoch ||
            before.jailUntilEpoch != after.jailUntilEpoch ||
            before.banUntilEpoch != after.banUntilEpoch ||
            before.lastOffenseEpoch != after.lastOffenseEpoch ||
            before.permanentBan != after.permanentBan ||
            before.lastCheatType != after.lastCheatType ||
            before.lastReason != after.lastReason ||
            before.lastBanMode != after.lastBanMode ||
            before.lastBanResult != after.lastBanResult;
    }

    char const* CheatTypeText(AegisCheatType type)
    {
        switch (type)
        {
        case AegisCheatType::Speed:
            return "speed";
        case AegisCheatType::Teleport:
            return "teleport";
        case AegisCheatType::NoClip:
            return "noclip";
        case AegisCheatType::Fly:
            return "fly";
        case AegisCheatType::Climb:
            return "climb";
        case AegisCheatType::Afk:
            return "afk";
        case AegisCheatType::Mount:
            return "mount";
        case AegisCheatType::Time:
            return "time";
        case AegisCheatType::Control:
            return "control";
        default:
            return "unknown";
        }
    }

    char const* CheatTypeTextZh(AegisCheatType type)
    {
        switch (type)
        {
        case AegisCheatType::Speed:
            return "速度";
        case AegisCheatType::Teleport:
            return "瞬移";
        case AegisCheatType::NoClip:
            return "穿墙";
        case AegisCheatType::Fly:
            return "飞行";
        case AegisCheatType::Climb:
            return "爬坡";
        case AegisCheatType::Afk:
            return "挂机";
        case AegisCheatType::Mount:
            return "坐骑";
        case AegisCheatType::Time:
            return "时间倍率";
        case AegisCheatType::Control:
            return "受控位移";
        default:
            return "未知";
        }
    }

    char const* ActionTypeTextZh(AegisActionType action)
    {
        switch (action)
        {
        case AegisActionType::Rollback:
            return "回滚";
        case AegisActionType::Debuff:
            return "减益处罚";
        case AegisActionType::Jail:
            return "监禁";
        case AegisActionType::Kick:
            return "踢下线";
        case AegisActionType::BanCharacter:
            return "角色封禁";
        case AegisActionType::BanAccountByCharacter:
        case AegisActionType::BanAccount:
            return "账号封禁";
        default:
            return "处罚";
        }
    }

    char const* BanReturnText(BanReturn code)
    {
        switch (code)
        {
        case BAN_SUCCESS:
            return "BAN_SUCCESS";
        case BAN_SYNTAX_ERROR:
            return "BAN_SYNTAX_ERROR";
        case BAN_NOTFOUND:
            return "BAN_NOTFOUND";
        case BAN_LONGER_EXISTS:
            return "BAN_LONGER_EXISTS";
        default:
            return "BAN_UNKNOWN";
        }
    }

    void ReplaceAll(std::string& text, std::string const& from, std::string const& to)
    {
        if (from.empty())
            return;

        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos)
        {
            text.replace(pos, from.length(), to);
            pos += to.length();
        }
    }

    bool ContainsId(std::vector<uint32> const& ids, uint32 id)
    {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }

    bool IsRecentTimestamp(uint32 nowMs, uint32 markMs, uint32 graceMs)
    {
        return markMs && nowMs >= markMs && (nowMs - markMs) <= graceMs;
    }

    bool IsServerAuthorizedAerialAura(Aura const* aura)
    {
        if (!aura)
            return false;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo)
            return false;

        if (aura->HasEffectType(SPELL_AURA_FLY) ||
            aura->HasEffectType(SPELL_AURA_HOVER) ||
            aura->HasEffectType(SPELL_AURA_FEATHER_FALL) ||
            aura->HasEffectType(SPELL_AURA_SAFE_FALL) ||
            aura->HasEffectType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED))
            return true;

        return spellInfo->HasAura(SPELL_AURA_FLY) ||
            spellInfo->HasAura(SPELL_AURA_HOVER) ||
            spellInfo->HasAura(SPELL_AURA_FEATHER_FALL) ||
            spellInfo->HasAura(SPELL_AURA_SAFE_FALL) ||
            spellInfo->HasAura(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    }

    bool IsServerAuthorizedAerialState(Player* player)
    {
        if (!player)
            return false;

        for (auto const& [spellId, aurApp] : player->GetAppliedAuras())
        {
            (void)spellId;

            if (!aurApp)
                continue;

            Aura const* aura = aurApp->GetBase();
            if (!IsServerAuthorizedAerialAura(aura))
                continue;

            ObjectGuid casterGuid = aura->GetCasterGUID();
            if (casterGuid.IsEmpty())
                return true;

            Unit* caster = aura->GetCaster();
            if (!caster)
                return true;

            if (caster == player || casterGuid.IsPlayer() || casterGuid.IsCreatureOrVehicle())
                return true;
        }

        return false;
    }

    bool IsExternalMobilityAura(Aura const* aura, Player* player)
    {
        if (!player || !IsServerAuthorizedAerialAura(aura))
            return false;

        ObjectGuid casterGuid = aura->GetCasterGUID();
        if (casterGuid.IsEmpty())
            return true;

        return casterGuid != player->GetGUID();
    }

    bool HasExternalMobilityAura(Player* player)
    {
        if (!player)
            return false;

        for (auto const& [spellId, aurApp] : player->GetAppliedAuras())
        {
            (void)spellId;

            if (aurApp && IsExternalMobilityAura(aurApp->GetBase(), player))
                return true;
        }

        return false;
    }

    uint8 StageRank(AegisPunishStage stage)
    {
        return static_cast<uint8>(stage);
    }

    AegisPunishStage StageFromTier(uint8 tier, AegisConfig const& cfg)
    {
        if (cfg.stage5Permanent && tier >= cfg.banPermanentTier)
            return AegisPunishStage::PermBan;
        if (tier >= 4)
            return AegisPunishStage::TempBan;
        if (tier >= 3)
            return AegisPunishStage::Kick;
        if (tier >= 2)
            return AegisPunishStage::Jail;
        if (tier >= 1)
            return AegisPunishStage::Debuff;
        return AegisPunishStage::Observe;
    }

    bool IsPositionMovementCheat(AegisCheatType type)
    {
        switch (type)
        {
        case AegisCheatType::Speed:
        case AegisCheatType::Teleport:
        case AegisCheatType::NoClip:
        case AegisCheatType::Fly:
        case AegisCheatType::Climb:
        case AegisCheatType::Mount:
            return true;
        default:
            return false;
        }
    }

    AegisPunishStage RaisePunishStage(AegisPunishStage baseStage, uint8 priorTier, AegisConfig const& cfg)
    {
        uint8 maxRank = cfg.stage5Permanent ?
            StageRank(AegisPunishStage::PermBan) :
            StageRank(AegisPunishStage::TempBan);
        uint8 targetRank = std::min<uint8>(maxRank,
            StageRank(baseStage) + priorTier);
        return static_cast<AegisPunishStage>(targetRank);
    }

    AegisPunishStage RiskThresholdStage(float risk,
        AegisConfig const& cfg)
    {
        if (risk >= cfg.banThreshold)
            return AegisPunishStage::TempBan;
        if (risk >= cfg.kickThreshold)
            return AegisPunishStage::Kick;
        if (risk >= cfg.jailThreshold)
            return AegisPunishStage::Jail;
        if (risk >= cfg.debuffThreshold)
            return AegisPunishStage::Debuff;
        return AegisPunishStage::Observe;
    }

    AegisPunishStage ApplyRiskGate(AegisPunishStage stageByEvidence,
        AegisPunishStage stageByRisk)
    {
        if (stageByRisk == AegisPunishStage::Observe)
            return AegisPunishStage::Observe;

        if (stageByEvidence == AegisPunishStage::Observe)
            return stageByRisk;

        return static_cast<AegisPunishStage>(std::min(
            StageRank(stageByEvidence), StageRank(stageByRisk)));
    }

    AegisPunishStage BasePunishStage(AegisEvidenceEvent const& evidence)
    {
        switch (evidence.cheatType)
        {
        case AegisCheatType::Teleport:
            if (evidence.tag == "CoordinateTeleport" ||
                evidence.tag == "StationaryCoordinateShift")
                return AegisPunishStage::Jail;
            return evidence.level == AegisEvidenceLevel::Strong ?
                AegisPunishStage::Debuff : AegisPunishStage::Observe;

        case AegisCheatType::NoClip:
            return evidence.level == AegisEvidenceLevel::Strong ?
                AegisPunishStage::Jail : AegisPunishStage::Debuff;

        case AegisCheatType::Fly:
            if (evidence.tag == "LowGravityJump")
                return AegisPunishStage::Jail;
            return evidence.level == AegisEvidenceLevel::Strong ?
                AegisPunishStage::Debuff : AegisPunishStage::Observe;

        case AegisCheatType::Climb:
            if (evidence.tag == "SuperJumpApex" &&
                evidence.level == AegisEvidenceLevel::Strong)
                return AegisPunishStage::Jail;
            return evidence.level == AegisEvidenceLevel::Strong ?
                AegisPunishStage::Debuff : AegisPunishStage::Observe;

        case AegisCheatType::Afk:
            return evidence.level == AegisEvidenceLevel::Strong ?
                AegisPunishStage::Jail : AegisPunishStage::Debuff;

        case AegisCheatType::Speed:
        case AegisCheatType::Control:
        case AegisCheatType::Time:
        case AegisCheatType::Mount:
            return evidence.level == AegisEvidenceLevel::Strong ?
                AegisPunishStage::Debuff : AegisPunishStage::Observe;

        default:
            return AegisPunishStage::Observe;
        }
    }

    bool HasControlledTeleportEffect(SpellInfo const* spellInfo)
    {
        return spellInfo && (spellInfo->HasEffect(SPELL_EFFECT_TELEPORT_UNITS) ||
            spellInfo->HasEffect(SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER));
    }

    bool HasControlledChargeEffect(SpellInfo const* spellInfo)
    {
        return spellInfo && (spellInfo->HasEffect(SPELL_EFFECT_CHARGE) ||
            spellInfo->HasEffect(SPELL_EFFECT_CHARGE_DEST));
    }

    bool HasControlledJumpEffect(SpellInfo const* spellInfo)
    {
        return spellInfo && (spellInfo->HasEffect(SPELL_EFFECT_JUMP) ||
            spellInfo->HasEffect(SPELL_EFFECT_JUMP_DEST) ||
            spellInfo->HasEffect(SPELL_EFFECT_LEAP_BACK));
    }

    bool HasControlledPullEffect(SpellInfo const* spellInfo)
    {
        return spellInfo && (spellInfo->HasEffect(SPELL_EFFECT_PULL_TOWARDS) ||
            spellInfo->HasEffect(SPELL_EFFECT_PULL_TOWARDS_DEST) ||
            spellInfo->HasEffect(SPELL_EFFECT_KNOCK_BACK) ||
            spellInfo->HasEffect(SPELL_EFFECT_KNOCK_BACK_DEST));
    }

    char const* ControlledMoveTag(AegisControlledMoveKind kind)
    {
        switch (kind)
        {
        case AegisControlledMoveKind::Charge:
            return "IgnoredCharge";
        case AegisControlledMoveKind::Jump:
            return "IgnoredJump";
        case AegisControlledMoveKind::Pull:
            return "IgnoredPull";
        case AegisControlledMoveKind::Teleport:
            return "IgnoredTeleport";
        default:
            return "IgnoredControlledMove";
        }
    }

    float TransportDist2D(AegisMoveSample const& a, AegisMoveSample const& b)
    {
        float dx = a.transportX - b.transportX;
        float dy = a.transportY - b.transportY;
        return std::sqrt(dx * dx + dy * dy);
    }

    void ArmControlledMoveExpectation(AegisPlayerContext& ctx,
        AegisControlledMoveKind kind, uint32 nowMs,
        float startX, float startY, float startZ,
        float minDistance2D, float minDeltaZ)
    {
        ctx.controlledMoveKind = kind;
        ctx.controlledMoveIssuedMs = nowMs;
        ctx.controlledMoveStartX = startX;
        ctx.controlledMoveStartY = startY;
        ctx.controlledMoveStartZ = startZ;
        ctx.controlledMoveMinDistance2D = minDistance2D;
        ctx.controlledMoveMinDeltaZ = minDeltaZ;
        ctx.controlledMoveWindowStartMs = 0;
        ctx.controlledMoveHits = 0;
    }

    void ClearControlledMoveExpectation(AegisPlayerContext& ctx)
    {
        ctx.controlledMoveKind = AegisControlledMoveKind::None;
        ctx.controlledMoveIssuedMs = 0;
        ctx.controlledMoveStartX = 0.0f;
        ctx.controlledMoveStartY = 0.0f;
        ctx.controlledMoveStartZ = 0.0f;
        ctx.controlledMoveMinDistance2D = 0.0f;
        ctx.controlledMoveMinDeltaZ = 0.0f;
        ctx.controlledMoveWindowStartMs = 0;
        ctx.controlledMoveHits = 0;
    }

    bool HasIllegalFlyFlags(uint32 moveFlags)
    {
        return (moveFlags & (MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_HOVER)) != 0;
    }

    bool HasActiveMovementIntent(uint32 moveFlags)
    {
        return (moveFlags & (MOVEMENTFLAG_MASK_MOVING |
            MOVEMENTFLAG_ASCENDING |
            MOVEMENTFLAG_DESCENDING |
            MOVEMENTFLAG_FALLING |
            MOVEMENTFLAG_FALLING_FAR |
            MOVEMENTFLAG_SWIMMING)) != 0;
    }

    bool IsNearPosition(Player* player, uint32 mapId, float x, float y, float z, float maxDistance)
    {
        if (!player || player->GetMapId() != mapId)
            return false;

        return Dist3DToPoint(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), x, y, z) <= maxDistance;
    }

    std::string FormatDurationShort(int64 seconds)
    {
        if (seconds <= 0)
            return "0s";

        int64 hours = seconds / HOUR;
        seconds %= HOUR;
        int64 minutes = seconds / MINUTE;
        seconds %= MINUTE;

        std::ostringstream text;
        if (hours > 0)
            text << hours << "h";
        if (minutes > 0)
        {
            if (text.tellp() > 0)
                text << " ";
            text << minutes << "m";
        }
        if (hours == 0 && seconds > 0)
        {
            if (text.tellp() > 0)
                text << " ";
            text << seconds << "s";
        }

        return text.str();
    }

    void ResetTeleportDetectionWindows(AegisPlayerContext& ctx)
    {
        ctx.stationaryMoveWindowStartMs = 0;
        ctx.stationaryMoveHits = 0;
        ctx.teleportBurstWindowStartMs = 0;
        ctx.teleportBurstHits = 0;
    }

    void ClearPendingTeleportExpectation(AegisPlayerContext& ctx);

    void ResetMovementDetectionState(AegisPlayerContext& ctx)
    {
        ctx.samples.Clear();
        ctx.safePosition = AegisSafePosition{};
        ctx.lastGeometryCheckMs = 0;
        ctx.flyWindowStartMs = 0;
        ctx.flySuspicionHits = 0;
        ctx.noClipWindowStartMs = 0;
        ctx.noClipBlockedHits = 0;
        ctx.speedWindowStartMs = 0;
        ctx.speedHits = 0;
        ctx.timeWindowStartMs = 0;
        ctx.timeHits = 0;
        ctx.airStallWindowStartMs = 0;
        ctx.airStallHits = 0;
        ctx.waterWalkWindowStartMs = 0;
        ctx.waterWalkHits = 0;
        ctx.mountWindowStartMs = 0;
        ctx.mountHits = 0;
        ctx.forceMoveWindowStartMs = 0;
        ctx.forceMoveHits = 0;
        ctx.climbWindowStartMs = 0;
        ctx.climbHitsInWindow = 0;
        ctx.superJumpWindowStartMs = 0;
        ctx.superJumpHitsInWindow = 0;
        ctx.doubleJumpWindowStartMs = 0;
        ctx.doubleJumpHits = 0;
        ctx.transportSpeedWindowStartMs = 0;
        ctx.transportSpeedHits = 0;
        ResetTeleportDetectionWindows(ctx);
        ClearControlledMoveExpectation(ctx);
    }

    void ResetMovementLifecycleState(AegisPlayerContext& ctx)
    {
        bool serverCanFly = ctx.serverCanFly;
        ResetMovementDetectionState(ctx);
        ctx.serverCanFly = serverCanFly;
        ctx.lastNotifyMs = 0;
        ctx.lastEvidenceMs = 0;
        ctx.lastEvidenceTag.clear();
        ctx.lastSpellGraceMs = 0;
        ctx.lastTeleportMs = 0;
        ctx.lastAuthorizedAerialMs = 0;
        ctx.lastMapChangeMs = 0;
        ctx.lastVehicleMs = 0;
        ctx.lastTransportMs = 0;
        ctx.lastTaxiFlightMs = 0;
        ctx.lastFallMs = 0;
        ctx.lastCanFlyServerMs = 0;
        ctx.lastAckMountMs = 0;
        ctx.lastControlledTeleportMs = 0;
        ctx.lastControlledChargeMs = 0;
        ctx.lastControlledJumpMs = 0;
        ctx.lastControlledPullMs = 0;
        ctx.lastKnockBackAckMs = 0;
        ctx.lastRootAckMs = 0;
        ctx.lastJumpOpcodeMs = 0;
        ctx.lastJailReturnCheckMs = 0;
        ctx.lastPunishNotifyMs = 0;
        ctx.observedAuthorizedAerialState = false;
        ctx.observedTaxiFlightState = false;
        ctx.observedTransportState = false;
        ctx.observedVehicleState = false;
        ctx.gather = AegisGatherState{};
        ClearPendingTeleportExpectation(ctx);
    }

    uint32 MovementSampleContinuityResetMs(AegisConfig const& cfg)
    {
        return std::max<uint32>(5000, cfg.speedMaxDtMs * 3);
    }

    bool SyncMovementBoundaryState(Player* player, MovementInfo const& movementInfo,
        AegisPlayerContext& ctx, uint32 nowMs)
    {
        if (!player)
            return false;

        bool hasAuthorizedAerialState =
            IsServerAuthorizedAerialState(player) || player->IsMounted() ||
            player->HasIncreaseMountedFlightSpeedAura() ||
            player->HasHoverAura() || ctx.serverCanFly;
        bool hasTaxiFlight = player->IsInFlight();
        bool hasTransport = player->GetTransport() != nullptr ||
            (movementInfo.GetMovementFlags() & MOVEMENTFLAG_ONTRANSPORT) != 0;
        bool hasVehicle = player->GetVehicle() != nullptr;
        bool movementBoundaryChanged =
            hasAuthorizedAerialState != ctx.observedAuthorizedAerialState ||
            hasTaxiFlight != ctx.observedTaxiFlightState ||
            hasTransport != ctx.observedTransportState ||
            hasVehicle != ctx.observedVehicleState;

        if (movementBoundaryChanged)
        {
            if (hasAuthorizedAerialState != ctx.observedAuthorizedAerialState)
                ctx.lastAuthorizedAerialMs = nowMs;
            if (hasTaxiFlight != ctx.observedTaxiFlightState)
                ctx.lastTaxiFlightMs = nowMs;
            if (hasTransport != ctx.observedTransportState)
                ctx.lastTransportMs = nowMs;
            if (hasVehicle != ctx.observedVehicleState)
                ctx.lastVehicleMs = nowMs;

            ResetMovementDetectionState(ctx);
        }

        if (hasAuthorizedAerialState)
            ctx.lastAuthorizedAerialMs = nowMs;
        if (hasTaxiFlight)
            ctx.lastTaxiFlightMs = nowMs;
        if (hasTransport)
            ctx.lastTransportMs = nowMs;
        if (hasVehicle)
            ctx.lastVehicleMs = nowMs;
        if ((movementInfo.GetMovementFlags() &
            (MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR |
                MOVEMENTFLAG_SWIMMING)) != 0)
            ctx.lastFallMs = nowMs;

        ctx.observedAuthorizedAerialState = hasAuthorizedAerialState;
        ctx.observedTaxiFlightState = hasTaxiFlight;
        ctx.observedTransportState = hasTransport;
        ctx.observedVehicleState = hasVehicle;
        return movementBoundaryChanged;
    }

    bool HasNearbyVerticalSupport(Player* player, AcAegisGeometry const& geometry,
        float x, float y, float z, float groundZ, float minHeightAboveGround)
    {
        if (!player)
            return false;

        if ((z - groundZ) < minHeightAboveGround)
            return false;

        float supportRadius = std::clamp(player->GetObjectSize() * 0.6f, 0.35f,
            1.1f);
        struct SupportOffset
        {
            float offsetX;
            float offsetY;
        };

        SupportOffset offsets[] = {
            { 0.0f, 0.0f },
            { supportRadius, 0.0f },
            { -supportRadius, 0.0f },
            { 0.0f, supportRadius },
            { 0.0f, -supportRadius },
        };

        for (SupportOffset const& offset : offsets)
        {
            float hitX = 0.0f;
            float hitY = 0.0f;
            float hitZ = 0.0f;
            if (geometry.RaycastStaticAndDynamic(player,
                x + offset.offsetX, y + offset.offsetY, z + 0.2f,
                x + offset.offsetX, y + offset.offsetY, groundZ + 0.2f,
                hitX, hitY, hitZ))
            {
                return true;
            }
        }

        return false;
    }

    void ClearPendingTeleportExpectation(AegisPlayerContext& ctx)
    {
        ctx.pendingTeleportExpireMs = 0;
        ctx.pendingTeleportMapId = 0;
        ctx.pendingTeleportX = 0.0f;
        ctx.pendingTeleportY = 0.0f;
        ctx.pendingTeleportZ = 0.0f;
    }

    void ArmPendingTeleportExpectation(AegisPlayerContext& ctx,
        uint32 nowMs, uint32 mapId, float x, float y, float z,
        AegisConfig const& cfg)
    {
        ctx.pendingTeleportExpireMs = nowMs +
            std::max(cfg.teleportGraceMs, cfg.teleportArrivalWindowMs);
        ctx.pendingTeleportMapId = mapId;
        ctx.pendingTeleportX = x;
        ctx.pendingTeleportY = y;
        ctx.pendingTeleportZ = z;
    }

    bool ConsumePendingTeleportArrival(AegisPlayerContext& ctx,
        AegisMoveSample const& sample, uint32 nowMs, AegisConfig const& cfg)
    {
        if (!ctx.pendingTeleportExpireMs)
            return false;

        if (nowMs > ctx.pendingTeleportExpireMs)
        {
            ClearPendingTeleportExpectation(ctx);
            return false;
        }

        if (sample.mapId != ctx.pendingTeleportMapId)
            return false;

        float dist = Dist3DToPoint(sample.x, sample.y, sample.z,
            ctx.pendingTeleportX, ctx.pendingTeleportY, ctx.pendingTeleportZ);
        if (dist > cfg.teleportArrivalRadius)
            return false;

        ctx.lastTeleportMs = nowMs;
        ResetTeleportDetectionWindows(ctx);
        ClearControlledMoveExpectation(ctx);
        ClearPendingTeleportExpectation(ctx);
        return true;
    }

    bool HasSharpDirectionChange2D(AegisMoveSample const& older,
        AegisMoveSample const& prev, AegisMoveSample const& cur)
    {
        float ax = prev.x - older.x;
        float ay = prev.y - older.y;
        float bx = cur.x - prev.x;
        float by = cur.y - prev.y;

        float lenA = std::sqrt(ax * ax + ay * ay);
        float lenB = std::sqrt(bx * bx + by * by);
        if (lenA < 0.75f || lenB < 0.75f)
            return false;

        float cosTurn = ((ax * bx) + (ay * by)) / (lenA * lenB);
        return cosTurn <= 0.25f;
    }
}

AcAegisMgr* AcAegisMgr::instance()
{
    static AcAegisMgr instance;
    return &instance;
}

AegisPlayerContext& AcAegisMgr::GetOrCreate(Player* player)
{
    uint32 guidLow = player->GetGUID().GetCounter();
    auto [it, inserted] = _players.try_emplace(guidLow);
    if (inserted)
        it->second.samples.Init(sAcAegisConfig->Get().samplingBufferSize);
    return it->second;
}

void AcAegisMgr::Touch(Player* player)
{
    AegisPlayerContext& ctx = GetOrCreate(player);
    ctx.online = true;
    ctx.lastSeenMs = _elapsedMs;
}

bool AcAegisMgr::IsEnabledFor(Player* player) const
{
    if (!player)
        return false;

    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.enabled)
        return false;

    if (!cfg.enabledOnGmAccounts && player->IsGameMaster())
        return false;

    return true;
}

bool AcAegisMgr::CanSafelyTeleportForPunish(Player* player) const
{
    return player && player->IsInWorld() && player->IsInGrid() &&
        !player->IsBeingTeleported() && !player->IsDuringRemoveFromWorld() &&
        !player->IsInFlight();
}

AegisMovementContext AcAegisMgr::BuildMovementContext(Player* player, AegisPlayerContext const& ctx, uint32 nowMs) const
{
    AegisMovementContext movementCtx;
    movementCtx.nowMs = nowMs;

    if (!player || !player->IsInWorld())
    {
        movementCtx.shouldSkipAllDetectors = true;
        movementCtx.shouldSkipAerialDetectors = true;
        return movementCtx;
    }

    AegisConfig const& cfg = sAcAegisConfig->Get();
    movementCtx.hasMap = player->GetMap() != nullptr;
    movementCtx.isAlive = player->IsAlive();
    movementCtx.isInCombat = player->IsInCombat();
    movementCtx.isMounted = player->IsMounted();
    movementCtx.isBeingTeleported = player->IsBeingTeleported();
    movementCtx.isTaxiFlight = player->IsInFlight();
    movementCtx.hasTransport = player->GetTransport() != nullptr;
    movementCtx.hasVehicle = player->GetVehicle() != nullptr;
    movementCtx.hasAuthorizedAerialState =
        IsServerAuthorizedAerialState(player) || ctx.serverCanFly;
    movementCtx.hasExternalAerialAura = HasExternalMobilityAura(player);
    movementCtx.hasWaterWalkAura = player->HasWaterWalkAura();
    movementCtx.hasGhostAura = player->HasGhostAura();
    movementCtx.hasHoverAura = player->HasHoverAura();

    for (uint32 auraId : cfg.auraWhitelist)
    {
        if (player->HasAura(auraId))
        {
            movementCtx.hasConfiguredAuraWhitelist = true;
            break;
        }
    }

    for (uint32 auraId : cfg.afkIgnoreAuras)
    {
        if (player->HasAura(auraId))
        {
            movementCtx.hasAfkIgnoreAura = true;
            break;
        }
    }

    movementCtx.recentSpellGrace = IsRecentTimestamp(nowMs, ctx.lastSpellGraceMs, cfg.mobilitySpellGraceMs);
    movementCtx.recentTeleportGrace = IsRecentTimestamp(nowMs, ctx.lastTeleportMs, cfg.teleportGraceMs);
    movementCtx.recentMapChangeGrace = IsRecentTimestamp(nowMs, ctx.lastMapChangeMs, cfg.mapChangeGraceMs);
    movementCtx.recentVehicleGrace = IsRecentTimestamp(nowMs, ctx.lastVehicleMs, cfg.vehicleGraceMs);
    movementCtx.recentTransportGrace = IsRecentTimestamp(nowMs, ctx.lastTransportMs, cfg.transportGraceMs);
    movementCtx.recentTaxiFlightGrace = IsRecentTimestamp(nowMs, ctx.lastTaxiFlightMs, cfg.transportGraceMs);
    movementCtx.recentRootAckGrace = IsRecentTimestamp(nowMs, ctx.lastRootAckMs, cfg.forceMoveGraceMs);
    movementCtx.recentControlledTeleportGrace = IsRecentTimestamp(nowMs, ctx.lastControlledTeleportMs, cfg.teleportGraceMs);
    movementCtx.recentControlledChargeGrace = IsRecentTimestamp(nowMs, ctx.lastControlledChargeMs, cfg.forceMoveGraceMs);
    movementCtx.recentControlledJumpGrace = IsRecentTimestamp(nowMs, ctx.lastControlledJumpMs, cfg.mobilitySpellGraceMs);
    movementCtx.recentControlledPullGrace = IsRecentTimestamp(nowMs, ctx.lastControlledPullMs, cfg.forceMoveGraceMs);
    movementCtx.recentServerCanFly = ctx.serverCanFly && IsRecentTimestamp(nowMs, ctx.lastCanFlyServerMs, cfg.flyCanFlyGraceMs);
    movementCtx.recentMountAck = IsRecentTimestamp(nowMs, ctx.lastAckMountMs, cfg.flyCanFlyGraceMs);
    movementCtx.recentMountGrace = IsRecentTimestamp(nowMs, ctx.lastAckMountMs, cfg.mountGraceMs);
    movementCtx.recentAerialExitGrace =
        !movementCtx.hasAuthorizedAerialState &&
        IsRecentTimestamp(nowMs, ctx.lastAuthorizedAerialMs,
            std::max(cfg.flyCanFlyGraceMs, cfg.mountGraceMs));
    movementCtx.recentJump = IsRecentTimestamp(nowMs, ctx.lastJumpOpcodeMs, cfg.superJumpWindowMs);
    movementCtx.recentExtendedJump = IsRecentTimestamp(nowMs, ctx.lastJumpOpcodeMs, cfg.superJumpWindowMs + 300);
    movementCtx.recentFall = IsRecentTimestamp(nowMs, ctx.lastFallMs, cfg.fallGraceMs);
    movementCtx.recentFallGrace = movementCtx.recentFall &&
        (!ctx.lastJumpOpcodeMs || nowMs < ctx.lastJumpOpcodeMs || (nowMs - ctx.lastJumpOpcodeMs) > (cfg.superJumpWindowMs + 300));
    movementCtx.recentKnockBackGrace = cfg.forceMoveEnabled && IsRecentTimestamp(nowMs, ctx.lastKnockBackAckMs, cfg.forceMoveGraceMs);

    movementCtx.shouldSkipAllDetectors =
        movementCtx.isBeingTeleported ||
        movementCtx.isTaxiFlight ||
        movementCtx.hasTransport ||
        movementCtx.hasVehicle ||
        movementCtx.recentTaxiFlightGrace ||
        movementCtx.recentMountGrace ||
        movementCtx.recentSpellGrace ||
        movementCtx.recentTeleportGrace ||
        movementCtx.recentControlledTeleportGrace ||
        movementCtx.recentControlledChargeGrace ||
        movementCtx.recentControlledJumpGrace ||
        movementCtx.recentControlledPullGrace ||
        movementCtx.recentMapChangeGrace ||
        movementCtx.recentVehicleGrace ||
        movementCtx.recentTransportGrace ||
        movementCtx.recentRootAckGrace ||
        movementCtx.recentFallGrace ||
        movementCtx.recentAerialExitGrace ||
        movementCtx.recentKnockBackGrace;

    movementCtx.shouldSkipAerialDetectors =
        movementCtx.shouldSkipAllDetectors ||
        HasWhitelistedAura(movementCtx) ||
        movementCtx.hasGhostAura ||
        movementCtx.recentServerCanFly ||
        movementCtx.recentMountAck;

    return movementCtx;
}

bool AcAegisMgr::HasWhitelistedAura(AegisMovementContext const& movementCtx) const
{
    return movementCtx.hasAuthorizedAerialState || movementCtx.hasConfiguredAuraWhitelist;
}

bool AcAegisMgr::ShouldSkipAllMovementDetectors(AegisMovementContext const& movementCtx) const
{
    return movementCtx.shouldSkipAllDetectors;
}

bool AcAegisMgr::ShouldSkipAerialMovementDetectors(AegisMovementContext const& movementCtx) const
{
    return movementCtx.shouldSkipAerialDetectors;
}

float AcAegisMgr::ComputeAllowedSpeed(Player* player, MovementInfo const& movementInfo) const
{
    if (!player)
        return 0.0f;

    uint32 moveFlags = movementInfo.GetMovementFlags();
    UnitMoveType speedType = movementInfo.GetSpeedType();
    float allowed = player->GetSpeed(speedType);

    bool hasAuthorizedFlight =
        (moveFlags & (MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY |
            MOVEMENTFLAG_DISABLE_GRAVITY)) != 0 ||
        player->IsInFlight() ||
        player->HasIncreaseMountedFlightSpeedAura() ||
        IsServerAuthorizedAerialState(player) ||
        player->HasHoverAura() ||
        player->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
    if (hasAuthorizedFlight)
    {
        UnitMoveType flightSpeedType = (moveFlags & MOVEMENTFLAG_BACKWARD) != 0 ?
            MOVE_FLIGHT_BACK : MOVE_FLIGHT;
        allowed = std::max(allowed, player->GetSpeed(flightSpeedType));
    }

    return allowed;
}

void AcAegisMgr::CaptureSample(Player* player, MovementInfo const& movementInfo, uint32 opcode, AegisPlayerContext& ctx)
{
    AegisMoveSample sample;
    sample.serverMs = _elapsedMs;
    sample.clientMs = movementInfo.time;
    sample.opcode = opcode;
    sample.moveFlags = movementInfo.GetMovementFlags();
    sample.moveFlags2 = movementInfo.GetExtraMovementFlags();
    sample.mapId = player->GetMapId();
    sample.zoneId = player->GetZoneId();
    sample.areaId = player->GetAreaId();
    sample.x = movementInfo.pos.GetPositionX();
    sample.y = movementInfo.pos.GetPositionY();
    sample.z = movementInfo.pos.GetPositionZ();
    sample.o = movementInfo.pos.GetOrientation();
    sample.jumpXySpeed = movementInfo.jump.xyspeed;
    sample.jumpZSpeed = movementInfo.jump.zspeed;
    sample.allowedSpeed = ComputeAllowedSpeed(player, movementInfo);
    sample.latencyMs = player->GetSession() ? player->GetSession()->GetLatency() : 0;
    sample.transportGuidCounter = movementInfo.transport.guid.GetCounter();
    sample.transportSeat = movementInfo.transport.seat;
    sample.transportX = movementInfo.transport.pos.GetPositionX();
    sample.transportY = movementInfo.transport.pos.GetPositionY();
    sample.transportZ = movementInfo.transport.pos.GetPositionZ();

    if ((sample.moveFlags & (MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_SWIMMING)) != 0)
        ctx.lastFallMs = _elapsedMs;
    if ((sample.moveFlags & MOVEMENTFLAG_ONTRANSPORT) != 0)
        ctx.lastTransportMs = _elapsedMs;

    ctx.samples.Push(sample);
    ctx.lastSeenMs = _elapsedMs;
    MaybeUpdateSafePosition(player, ctx, sample);
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectTime(Player* /*player*/, AegisPlayerContext& ctx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.timeEnabled || ctx.samples.Size() < 2)
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.mapId != prev.mapId || cur.serverMs <= prev.serverMs || cur.clientMs <= prev.clientMs)
        return std::nullopt;

    if ((cur.moveFlags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_SWIMMING)) != 0)
        return std::nullopt;

    uint32 serverDt = cur.serverMs - prev.serverMs;
    uint32 clientDt = cur.clientMs - prev.clientMs;
    if (serverDt < cfg.speedMinDtMs || serverDt > cfg.speedMaxDtMs)
        return std::nullopt;

    float dist2d = Dist2D(cur, prev);
    if (dist2d < cfg.timeMinDistance2D)
        return std::nullopt;

    float ratio = static_cast<float>(clientDt) / static_cast<float>(std::max<uint32>(1, serverDt));
    float leadMs = static_cast<float>(clientDt - serverDt);
    if (ratio < cfg.timeMinRatio || leadMs < cfg.timeMinClientLeadMs)
    {
        ctx.timeWindowStartMs = 0;
        ctx.timeHits = 0;
        return std::nullopt;
    }

    if (ctx.timeWindowStartMs == 0 || (cur.serverMs - ctx.timeWindowStartMs) > cfg.timeWindowMs)
    {
        ctx.timeWindowStartMs = cur.serverMs;
        ctx.timeHits = 0;
    }

    ++ctx.timeHits;
    if (ctx.timeHits < 2)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Time;
    evidence.level = (ctx.timeHits >= cfg.timeStrongHits && ratio >= (cfg.timeMinRatio + 0.15f)) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "ClientTimeLead";
    evidence.detail = "clientDt=" + std::to_string(clientDt) + ",serverDt=" + std::to_string(serverDt);
    evidence.riskDelta = ClampRiskDelta(7.0f + std::min(8.0f, leadMs / 75.0f) + (ratio - 1.0f) * 8.0f);
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = false;
    evidence.metricA = ratio;
    evidence.metricB = leadMs;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectForceMove(Player* /*player*/, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.forceMoveEnabled || ctx.samples.Size() < 2)
        return std::nullopt;

    if (ShouldSkipAllMovementDetectors(movementCtx))
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.opcode != CMSG_MOVE_KNOCK_BACK_ACK || cur.mapId != prev.mapId || cur.serverMs <= prev.serverMs)
        return std::nullopt;

    if (cur.jumpXySpeed < cfg.forceMoveMinAckSpeedXY && std::fabs(cur.jumpZSpeed) < cfg.forceMoveMinAckSpeedZ)
        return std::nullopt;

    uint32 dtMs = cur.serverMs - prev.serverMs;
    if (dtMs < 50 || dtMs > cfg.speedMaxDtMs)
        return std::nullopt;

    float dist2d = Dist2D(cur, prev);
    float deltaZ = std::fabs(cur.z - prev.z);
    float dtSeconds = static_cast<float>(dtMs) / 1000.0f;
    float expected2d = std::max(cfg.forceMoveMinDistance2D, cur.jumpXySpeed * dtSeconds * cfg.forceMoveExpectedFactor);
    float expectedZ = std::max(cfg.forceMoveMinDeltaZ, std::fabs(cur.jumpZSpeed) * dtSeconds * cfg.forceMoveExpectedFactor);

    if (dist2d >= expected2d || deltaZ >= expectedZ)
    {
        ctx.forceMoveWindowStartMs = 0;
        ctx.forceMoveHits = 0;
        return std::nullopt;
    }

    if (ctx.forceMoveWindowStartMs == 0 || (cur.serverMs - ctx.forceMoveWindowStartMs) > cfg.forceMoveWindowMs)
    {
        ctx.forceMoveWindowStartMs = cur.serverMs;
        ctx.forceMoveHits = 0;
    }

    ++ctx.forceMoveHits;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Control;
    evidence.level = (ctx.forceMoveHits >= cfg.forceMoveStrongHits || (dist2d <= (cfg.forceMoveMinDistance2D * 0.25f) && deltaZ <= (cfg.forceMoveMinDeltaZ * 0.25f))) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "IgnoredKnockback";
    evidence.detail = "xySpeed=" + std::to_string(cur.jumpXySpeed) + ",zSpeed=" + std::to_string(cur.jumpZSpeed);
    evidence.riskDelta = ClampRiskDelta(9.0f + std::min(8.0f, cur.jumpXySpeed) + std::min(6.0f, std::fabs(cur.jumpZSpeed)) * 0.5f);
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = false;
    evidence.metricA = dist2d;
    evidence.metricB = expected2d;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectRootBreak(Player* player,
    MovementInfo const& movementInfo, AegisPlayerContext& ctx,
    AegisMovementContext const& movementCtx) const
{
    if (!player || !player->IsRooted())
        return std::nullopt;

    if (movementCtx.isBeingTeleported || movementCtx.isTaxiFlight ||
        movementCtx.hasTransport || movementCtx.hasVehicle ||
        movementCtx.recentRootAckGrace ||
        movementCtx.recentControlledChargeGrace ||
        movementCtx.recentControlledJumpGrace ||
        movementCtx.recentControlledPullGrace ||
        movementCtx.recentControlledTeleportGrace ||
        movementCtx.recentKnockBackGrace)
        return std::nullopt;

    bool hasRootFlag = movementInfo.HasMovementFlag(MOVEMENTFLAG_ROOT);
    bool claimsMoving = movementInfo.HasMovementFlag(MOVEMENTFLAG_MASK_MOVING);
    float dist2d = Dist3DToPoint(movementInfo.pos.GetPositionX(),
        movementInfo.pos.GetPositionY(), player->GetPositionZ(),
        player->GetPositionX(), player->GetPositionY(), player->GetPositionZ());
    float deltaZ = std::fabs(movementInfo.pos.GetPositionZ() -
        player->GetPositionZ());

    if (!claimsMoving && hasRootFlag && dist2d < 0.15f && deltaZ < 0.15f)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Control;
    evidence.level = (!hasRootFlag || dist2d >= 1.0f || deltaZ >= 0.75f) ?
        AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "RootBreak";
    evidence.detail = "moving=" + std::to_string(claimsMoving ? 1 : 0) +
        ",rootFlag=" + std::to_string(hasRootFlag ? 1 : 0);
    evidence.riskDelta = ClampRiskDelta(10.0f +
        (claimsMoving ? 6.0f : 0.0f) + std::min(8.0f, dist2d * 4.0f));
    evidence.serverMs = _elapsedMs;
    evidence.shouldRollback = claimsMoving || dist2d >= 0.5f || deltaZ >= 0.5f;
    evidence.metricA = dist2d;
    evidence.metricB = deltaZ;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectControlledMove(Player* player,
    AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    if (!player || ctx.controlledMoveKind == AegisControlledMoveKind::None ||
        ctx.samples.Empty())
        return std::nullopt;

    if (movementCtx.isTaxiFlight || movementCtx.hasTransport ||
        movementCtx.hasVehicle || movementCtx.isBeingTeleported)
    {
        ClearControlledMoveExpectation(ctx);
        return std::nullopt;
    }

    AegisMoveSample const& cur = ctx.samples.Newest();
    if (cur.serverMs < ctx.controlledMoveIssuedMs)
        return std::nullopt;

    uint32 elapsedMs = cur.serverMs - ctx.controlledMoveIssuedMs;
    if (elapsedMs < 150)
        return std::nullopt;

    float moved2d = Dist3DToPoint(cur.x, cur.y, ctx.controlledMoveStartZ,
        ctx.controlledMoveStartX, ctx.controlledMoveStartY,
        ctx.controlledMoveStartZ);
    float deltaZ = std::fabs(cur.z - ctx.controlledMoveStartZ);
    if (moved2d >= ctx.controlledMoveMinDistance2D ||
        deltaZ >= ctx.controlledMoveMinDeltaZ)
    {
        ClearControlledMoveExpectation(ctx);
        return std::nullopt;
    }

    uint32 windowMs = sAcAegisConfig->Get().forceMoveWindowMs;
    if (ctx.controlledMoveKind == AegisControlledMoveKind::Jump)
        windowMs = std::max(windowMs, sAcAegisConfig->Get().mobilitySpellGraceMs);
    else if (ctx.controlledMoveKind == AegisControlledMoveKind::Teleport)
        windowMs = std::max(windowMs, sAcAegisConfig->Get().teleportGraceMs);

    if (ctx.controlledMoveWindowStartMs == 0 ||
        (cur.serverMs - ctx.controlledMoveWindowStartMs) > windowMs)
    {
        ctx.controlledMoveWindowStartMs = cur.serverMs;
        ctx.controlledMoveHits = 0;
    }

    ++ctx.controlledMoveHits;
    if (ctx.controlledMoveHits < 2 && elapsedMs < std::min<uint32>(600, windowMs))
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Control;
    evidence.level = (ctx.controlledMoveHits >= 3 || elapsedMs >= windowMs) ?
        AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = ControlledMoveTag(ctx.controlledMoveKind);
    evidence.detail = "moved2d=" + std::to_string(moved2d) +
        ",deltaZ=" + std::to_string(deltaZ);
    evidence.riskDelta = ClampRiskDelta(8.0f +
        static_cast<float>(ctx.controlledMoveHits) * 3.0f);
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = false;
    evidence.metricA = moved2d;
    evidence.metricB = ctx.controlledMoveMinDistance2D;
    ClearControlledMoveExpectation(ctx);
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectTransportRelativeSpeed(Player* /*player*/,
    AegisPlayerContext& ctx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.speedEnabled || ctx.samples.Size() < 2)
        return std::nullopt;

    auto resetTransportSpeedWindow = [&ctx]()
    {
        ctx.transportSpeedWindowStartMs = 0;
        ctx.transportSpeedHits = 0;
    };

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if ((cur.moveFlags & MOVEMENTFLAG_ONTRANSPORT) == 0 ||
        (prev.moveFlags & MOVEMENTFLAG_ONTRANSPORT) == 0)
    {
        resetTransportSpeedWindow();
        return std::nullopt;
    }

    if (cur.transportGuidCounter == 0 ||
        cur.transportGuidCounter != prev.transportGuidCounter ||
        cur.transportSeat != prev.transportSeat ||
        cur.serverMs <= prev.serverMs)
    {
        resetTransportSpeedWindow();
        return std::nullopt;
    }

    uint32 dtMs = cur.serverMs - prev.serverMs;
    if (dtMs < cfg.speedMinDtMs || dtMs > cfg.speedMaxDtMs)
    {
        resetTransportSpeedWindow();
        return std::nullopt;
    }

    float dist2d = TransportDist2D(cur, prev);
    if (dist2d < 0.5f)
    {
        resetTransportSpeedWindow();
        return std::nullopt;
    }

    float observed = dist2d * 1000.0f / static_cast<float>(dtMs);
    float allowed = std::max(0.01f, std::max(prev.allowedSpeed, cur.allowedSpeed));
    float threshold = allowed * (1.0f + cfg.speedTolerancePct / 100.0f) +
        cfg.speedFlatMargin + 0.5f;
    if (observed <= threshold)
    {
        resetTransportSpeedWindow();
        return std::nullopt;
    }

    float excess = observed - threshold;
    float minExcess = std::max(0.75f, threshold * 0.08f);
    if (excess < minExcess)
    {
        resetTransportSpeedWindow();
        return std::nullopt;
    }

    float ratio = std::min(4.0f, observed / std::max(0.01f, threshold));
    bool massiveSpike = ratio >= std::max(cfg.speedStrongRatio + 0.6f, 2.25f);

    if (ctx.transportSpeedWindowStartMs == 0 ||
        (cur.serverMs - ctx.transportSpeedWindowStartMs) > cfg.speedWindowMs)
    {
        ctx.transportSpeedWindowStartMs = cur.serverMs;
        ctx.transportSpeedHits = 0;
    }

    ++ctx.transportSpeedHits;
    if (!massiveSpike && ctx.transportSpeedHits < 3)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Speed;
    evidence.level = (massiveSpike || ctx.transportSpeedHits >= 4) ?
        AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "TransportRelativeSpeed";
    evidence.detail = "observed=" + std::to_string(observed) +
        ",allowed=" + std::to_string(threshold) +
        ",hits=" + std::to_string(ctx.transportSpeedHits);
    evidence.riskDelta = ClampRiskDelta(6.0f +
        std::min(10.0f, ratio * 4.0f) +
        std::min(4.0f,
            static_cast<float>(ctx.transportSpeedHits - 1) * 1.5f));
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = false;
    evidence.metricA = observed;
    evidence.metricB = threshold;
    return evidence;
}

void AcAegisMgr::MaybeUpdateSafePosition(Player* player, AegisPlayerContext& ctx, AegisMoveSample const& sample)
{
    if (!player)
        return;

    if ((sample.moveFlags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR)) != 0)
        return;

    float groundZ = 0.0f;
    if (!_geometry.GetGroundHeight(player, sample.x, sample.y, sample.z, groundZ))
        return;

    if (std::fabs(sample.z - groundZ) > 3.0f)
        return;

    ctx.safePosition.valid = true;
    ctx.safePosition.mapId = sample.mapId;
    ctx.safePosition.x = sample.x;
    ctx.safePosition.y = sample.y;
    ctx.safePosition.z = sample.z;
    ctx.safePosition.o = sample.o;
}

void AcAegisMgr::DecayRisk(AegisPlayerContext& ctx, uint32 nowMs) const
{
    if (ctx.lastDecayMs == 0)
    {
        ctx.lastDecayMs = nowMs;
        return;
    }

    if (ctx.riskScore <= 0.0f || nowMs <= ctx.lastDecayMs)
    {
        ctx.lastDecayMs = nowMs;
        return;
    }

    float halfLifeMs = sAcAegisConfig->Get().riskHalfLifeSeconds * 1000.0f;
    float elapsed = static_cast<float>(nowMs - ctx.lastDecayMs);
    float factor = std::pow(0.5f, elapsed / halfLifeMs);
    ctx.riskScore *= factor;
    if (ctx.riskScore < 0.25f)
        ctx.riskScore = 0.0f;
    ctx.lastDecayMs = nowMs;
}

std::string AcAegisMgr::EscapeJson(std::string const& value) const
{
    std::ostringstream out;
    for (char ch : value)
    {
        switch (ch)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            out << ch;
            break;
        }
    }

    return out.str();
}

std::string AcAegisMgr::ToIsoTimestamp() const
{
    std::time_t now = std::time(nullptr);
    std::tm timeInfo{};
#ifdef _WIN32
    localtime_s(&timeInfo, &now);
#else
    localtime_r(&now, &timeInfo);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
    return buffer;
}

std::string AcAegisMgr::EvidenceLevelText(AegisEvidenceLevel level) const
{
    switch (level)
    {
    case AegisEvidenceLevel::Weak:
        return "weak";
    case AegisEvidenceLevel::Medium:
        return "medium";
    case AegisEvidenceLevel::Strong:
        return "strong";
    default:
        return "info";
    }
}

std::string AcAegisMgr::ActionText(AegisActionDecision const& decision) const
{
    switch (decision.primaryAction)
    {
    case AegisActionType::Debuff:
        return "debuff";
    case AegisActionType::Jail:
        return "jail";
    case AegisActionType::Kick:
        return "kick";
    case AegisActionType::BanCharacter:
        return "ban-character";
    case AegisActionType::BanAccount:
        return "ban-account";
    case AegisActionType::BanAccountByCharacter:
        return "ban-account-by-character";
    default:
        if (decision.shouldRollback)
            return "rollback";
        if (decision.shouldNotify)
            return "notify";
        return "observe";
    }
}

std::string AcAegisMgr::StageText(AegisPunishStage stage) const
{
    switch (stage)
    {
    case AegisPunishStage::Debuff:
        return "debuff";
    case AegisPunishStage::Jail:
        return "jail";
    case AegisPunishStage::Kick:
        return "kick";
    case AegisPunishStage::TempBan:
        return "temp-ban";
    case AegisPunishStage::PermBan:
        return "perm-ban";
    default:
        return "observe";
    }
}

std::string AcAegisMgr::BuildAuditCommonFields(Player* player, AegisPlayerContext const* ctx) const
{
    std::ostringstream payload;
    if (player)
    {
        payload << "\"player\":\"" << EscapeJson(player->GetName()) << "\""
                << ",\"guid\":" << player->GetGUID().GetCounter()
                << ",\"account\":"
                << (player->GetSession() ? player->GetSession()->GetAccountId() : 0)
                << ",\"ip\":\""
                << EscapeJson(player->GetSession() ? player->GetSession()->GetRemoteAddress() : "")
                << "\""
                << ",\"map\":" << player->GetMapId()
                << ",\"zone\":" << player->GetZoneId()
                << ",\"area\":" << player->GetAreaId()
                << ",\"x\":" << player->GetPositionX()
                << ",\"y\":" << player->GetPositionY()
                << ",\"z\":" << player->GetPositionZ()
                << ",\"o\":" << player->GetOrientation();
    }

    if (ctx)
    {
        if (payload.tellp() > 0)
            payload << ',';

        payload << "\"online\":" << (ctx->online ? "true" : "false")
                << ",\"risk\":" << static_cast<uint32>(ctx->riskScore)
                << ",\"punishStage\":\"" << StageText(ctx->punish.punishStage) << "\""
                << ",\"offenseCount\":" << ctx->punish.offenseCount
                << ",\"offenseTier\":" << static_cast<uint32>(ctx->punish.offenseTier)
                << ",\"lastEvidenceTag\":\"" << EscapeJson(ctx->lastEvidenceTag) << "\""
                << ",\"debuffUntil\":" << ctx->punish.debuffUntilEpoch
                << ",\"jailUntil\":" << ctx->punish.jailUntilEpoch
                << ",\"banUntil\":" << ctx->punish.banUntilEpoch
                << ",\"permanentBan\":" << (ctx->punish.permanentBan ? "true" : "false");

        if (sAcAegisConfig->Get().verboseLog)
        {
            payload << ",\"gatherActions\":" << ctx->gather.actionCount
                    << ",\"gatherLoot\":" << ctx->gather.lootCount
                    << ",\"gatherNodes\":" << ctx->gather.gatherCount
                    << ",\"gatherSuspiciousWindows\":"
                    << ctx->gather.suspiciousWindows
                    << ",\"lastSeenMs\":" << ctx->lastSeenMs
                    << ",\"lastNotifyMs\":" << ctx->lastNotifyMs
                    << ",\"lastTeleportMs\":" << ctx->lastTeleportMs;

            if (ctx->safePosition.valid)
            {
                payload << ",\"safeMap\":" << ctx->safePosition.mapId
                        << ",\"safeX\":" << ctx->safePosition.x
                        << ",\"safeY\":" << ctx->safePosition.y
                        << ",\"safeZ\":" << ctx->safePosition.z
                        << ",\"safeO\":" << ctx->safePosition.o;
            }
        }
    }

    return payload.str();
}

std::string AcAegisMgr::BuildPunishPlayerMessage(std::string const& templ, std::string const& timeText) const
{
    std::string text = templ;
    ReplaceAll(text, "{time}", "|cffff0000" + timeText + "|r");
    return text;
}

std::string AcAegisMgr::BuildGmMessage(Player* player, AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx) const
{
    AegisActionDecision decision = DetermineAction(player, ctx, evidence);
    std::ostringstream message;
    if (sAcAegisConfig->Get().gmNotifyFormat == "verbose")
    {
        message << "[AcAegis] player=" << player->GetName()
                << " guid=" << player->GetGUID().GetCounter()
                << " type=" << CheatTypeText(evidence.cheatType)
                << " level=" << EvidenceLevelText(evidence.level)
                << " risk=" << static_cast<uint32>(ctx.riskScore)
                << " offense=" << ctx.punish.offenseCount
                << " tier=" << static_cast<uint32>(ctx.punish.offenseTier)
                << " action=" << ActionText(decision)
                << " map=" << player->GetMapId()
                << " pos=" << player->GetPositionX() << "," << player->GetPositionY() << "," << player->GetPositionZ()
                << " tag=" << evidence.tag
                << " detail=" << evidence.detail;
    }
    else
    {
        message << "[AcAegis] " << player->GetName()
                << "#" << player->GetGUID().GetCounter()
                << " " << CheatTypeText(evidence.cheatType)
                << "/" << EvidenceLevelText(evidence.level)
                << " risk=" << static_cast<uint32>(ctx.riskScore)
                << " tier=" << static_cast<uint32>(ctx.punish.offenseTier)
                << " action=" << ActionText(decision)
                << " tag=" << evidence.tag;
    }

    return message.str();
}

std::string AcAegisMgr::BuildPanelReason(AegisEvidenceEvent const& evidence) const
{
    return evidence.cheatType == AegisCheatType::Afk ?
        sAcAegisConfig->Get().panelAfkReason :
        sAcAegisConfig->Get().panelMovementReason;
}

std::string AcAegisMgr::ResolveLogPath(std::string const& configuredPath) const
{
    std::filesystem::path path = configuredPath.empty() ? std::filesystem::path("./logs/aegis.log") : std::filesystem::path(configuredPath);
    if (path.is_relative())
        path = std::filesystem::current_path() / path;
    return path.lexically_normal().string();
}

void AcAegisMgr::EnsureFileLogReady() const
{
    if (!sAcAegisConfig->Get().logEnabled)
        return;

    GetAsyncFileAppender().PreparePath(
        ResolveLogPath(sAcAegisConfig->Get().fileLogPath));
}

void AcAegisMgr::WriteAegisLog(std::string const& level, std::string const& message) const
{
    if (!sAcAegisConfig->Get().logEnabled)
        return;

    EnsureFileLogReady();
    AppendTextLine(ResolveLogPath(sAcAegisConfig->Get().fileLogPath),
        "[" + ToIsoTimestamp() + "] [" + level + "] " + message);
}

void AcAegisMgr::WriteAuditLog(std::string const& event, Player* player,
    AegisPlayerContext const* ctx, std::string const& fields) const
{
    std::ostringstream payload;
    payload << "{\"event\":\"" << EscapeJson(event) << "\"";

    std::string commonFields = BuildAuditCommonFields(player, ctx);
    if (!commonFields.empty())
        payload << ',' << commonFields;
    if (!fields.empty())
        payload << ',' << fields;

    payload << '}';
    WriteAegisLog("audit", payload.str());
}

void AcAegisMgr::AppendTextLine(std::string const& path, std::string const& line) const
{
    if (path.empty())
        return;

    GetAsyncFileAppender().Append(path, line);
}

void AcAegisMgr::WritePanelCharacterLog(std::string const& action, std::string const& stage, std::string const& payload) const
{
    if (!sAcAegisConfig->Get().panelOutputEnabled || sAcAegisConfig->Get().panelCharacterLogPath.empty())
        return;

    AppendTextLine(sAcAegisConfig->Get().panelCharacterLogPath,
        ToIsoTimestamp() + " [" + action + "|" + stage + "] " + payload);
}

void AcAegisMgr::WritePanelAccountBanLog(Player* player, AegisEvidenceEvent const& evidence, AegisActionDecision const& decision) const
{
    if (!player)
        return;

    WritePanelAccountBanLog(player->GetGUID().GetCounter(),
        player->GetSession() ? player->GetSession()->GetAccountId() : 0,
        player->GetSession() ? player->GetSession()->GetRemoteAddress() : "",
        evidence, decision);
}

void AcAegisMgr::WritePanelAccountBanLog(uint32 guidLow, uint32 accountId,
    std::string const& ip, AegisEvidenceEvent const& evidence,
    AegisActionDecision const& decision) const
{
    if (!sAcAegisConfig->Get().panelOutputEnabled)
        return;

    uint32 hours = decision.banSeconds == 0 ? 0 : static_cast<uint32>((decision.banSeconds + HOUR - 1) / HOUR);

    if (decision.banMode == "character")
    {
        if (sAcAegisConfig->Get().panelCharacterLogPath.empty())
            return;

        std::ostringstream payload;
        payload << "{\"guid\":" << guidLow
                << ",\"hours\":" << hours
                << ",\"reason\":\"" << EscapeJson(BuildPanelReason(evidence))
            << "\",\"ip\":\"" << EscapeJson(ip)
                << "\"}";
        WritePanelCharacterLog("aegis_ban_character", "success", payload.str());
        return;
    }

        if (sAcAegisConfig->Get().panelAccountLogPath.empty() || accountId == 0)
        return;

    std::ostringstream payload;
    payload << "{\"admin\":\"" << EscapeJson(sAcAegisConfig->Get().panelAdmin)
            << "\",\"server\":" << sAcAegisConfig->Get().panelServerId
            << ",\"id\":" << accountId
            << ",\"hours\":" << hours
            << ",\"reason\":\"" << EscapeJson(BuildPanelReason(evidence))
            << "\",\"ip\":\"" << EscapeJson(ip)
            << "\"}";
    AppendTextLine(sAcAegisConfig->Get().panelAccountLogPath,
        "[" + ToIsoTimestamp() + "] ban.success " + payload.str());
}

void AcAegisMgr::WritePanelDetectionLog(Player* player, AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx) const
{
    if (!player || !sAcAegisConfig->Get().panelOutputEnabled || !sAcAegisConfig->Get().panelWriteDetections)
        return;

    std::ostringstream payload;
    payload << "{\"guid\":" << player->GetGUID().GetCounter()
            << ",\"account\":" << (player->GetSession() ? player->GetSession()->GetAccountId() : 0)
            << ",\"type\":\"" << CheatTypeText(evidence.cheatType)
            << "\",\"level\":\"" << EvidenceLevelText(evidence.level)
            << "\",\"risk\":" << static_cast<uint32>(ctx.riskScore)
            << ",\"offense\":" << ctx.punish.offenseCount
            << ",\"tag\":\"" << EscapeJson(evidence.tag)
            << "\",\"detail\":\"" << EscapeJson(evidence.detail)
            << "\",\"ip\":\"" << EscapeJson(player->GetSession() ? player->GetSession()->GetRemoteAddress() : "")
            << "\"}";
    WritePanelCharacterLog("aegis_detect", "info", payload.str());
}

void AcAegisMgr::EnforceJail(Player* player, AegisPlayerContext& ctx, bool silent) const
{
    if (!player)
        return;

    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!IsNearPosition(player, cfg.jailMapId, cfg.jailX, cfg.jailY, cfg.jailZ, cfg.jailLeashRadius))
    {
        if (!CanSafelyTeleportForPunish(player))
            return;

        float distance = Dist3DToPoint(player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ(), cfg.jailX, cfg.jailY, cfg.jailZ);
        std::ostringstream audit;
        audit << "\"reason\":\"jail-leash\""
              << ",\"distance\":" << distance
              << ",\"leashRadius\":" << cfg.jailLeashRadius
              << ",\"silent\":" << (silent ? "true" : "false");
        WriteAuditLog("jail_return", player, &ctx, audit.str());

        if (!silent && player->GetSession())
            ChatHandler(player->GetSession()).SendSysMessage("AcAegis: jailed players cannot leave the jail zone.");
        Jail(player, ctx);
    }
}

void AcAegisMgr::ResetGatherWindow(Player* player, AegisPlayerContext& ctx, char const* /*reason*/) const
{
    if (!player)
        return;

    ctx.gather.windowStartMs = 0;
    ctx.gather.lastActionMs = 0;
    ctx.gather.actionCount = 0;
    ctx.gather.lootCount = 0;
    ctx.gather.gatherCount = 0;
    ctx.gather.startX = player->GetPositionX();
    ctx.gather.startY = player->GetPositionY();
    ctx.gather.startZ = player->GetPositionZ();
    ctx.gather.lastSource.clear();
}

void AcAegisMgr::TouchGatherWindow(Player* player, AegisPlayerContext& ctx, char const* source)
{
    if (!player || !sAcAegisConfig->Get().afkEnabled)
        return;

    uint32 nowMs = _elapsedMs;
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (ctx.gather.lastActionMs && nowMs >= ctx.gather.lastActionMs && (nowMs - ctx.gather.lastActionMs) < cfg.afkMinActionGapMs)
        return;

    if (ctx.gather.windowStartMs == 0)
    {
        ctx.gather.windowStartMs = nowMs;
        ctx.gather.startX = player->GetPositionX();
        ctx.gather.startY = player->GetPositionY();
        ctx.gather.startZ = player->GetPositionZ();
    }

    float moved = Dist3DToPoint(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), ctx.gather.startX, ctx.gather.startY, ctx.gather.startZ);
    if (moved > (cfg.afkMaxMoveDistance * 2.0f))
    {
        ResetGatherWindow(player, ctx, source);
        ctx.gather.windowStartMs = nowMs;
    }

    ctx.gather.lastActionMs = nowMs;
    ++ctx.gather.actionCount;
    if (std::string(source) == "loot")
        ++ctx.gather.lootCount;
    else
        ++ctx.gather.gatherCount;
    ctx.gather.lastSource = source;

    AegisMovementContext movementCtx = BuildMovementContext(player, ctx, nowMs);
    if (std::optional<AegisEvidenceEvent> evidence = DetectAfk(player, ctx, movementCtx))
        HandleEvidence(player, ctx, *evidence);
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectSpeed(Player* /*player*/, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.speedEnabled || ctx.samples.Size() < 2)
        return std::nullopt;

    if (ShouldSkipAllMovementDetectors(movementCtx))
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.mapId != prev.mapId || cur.serverMs <= prev.serverMs)
        return std::nullopt;

    uint32 dtMs = cur.serverMs - prev.serverMs;
    if (dtMs < cfg.speedMinDtMs || dtMs > cfg.speedMaxDtMs)
        return std::nullopt;

    if (!HasActiveMovementIntent(cur.moveFlags))
        return std::nullopt;

    float dist2d = Dist2D(cur, prev);
    if (dist2d < 0.1f)
        return std::nullopt;

    uint32 effectiveDtMs = dtMs;
    if (cur.clientMs > prev.clientMs)
        effectiveDtMs = std::max(effectiveDtMs, cur.clientMs - prev.clientMs);

    float observed = dist2d * 1000.0f / static_cast<float>(effectiveDtMs);
    float allowed = std::max(0.01f, std::max(prev.allowedSpeed, cur.allowedSpeed));
    float latencyMargin = std::min(2.0f,
        static_cast<float>(std::max(prev.latencyMs, cur.latencyMs)) / 180.0f);
    float threshold = allowed * (1.0f + cfg.speedTolerancePct / 100.0f) +
        cfg.speedFlatMargin + latencyMargin;
    if (observed <= threshold)
    {
        ctx.speedWindowStartMs = 0;
        ctx.speedHits = 0;
        return std::nullopt;
    }

    float excess = observed - threshold;
    float minExcess = std::max(0.75f, threshold * 0.06f);
    if (excess < minExcess)
    {
        ctx.speedWindowStartMs = 0;
        ctx.speedHits = 0;
        return std::nullopt;
    }

    float ratio = std::min(4.0f, observed / threshold);
    bool massiveSpike = ratio >= (cfg.speedStrongRatio + 0.9f);

    if (ctx.speedWindowStartMs == 0 ||
        (cur.serverMs - ctx.speedWindowStartMs) > cfg.speedWindowMs)
    {
        ctx.speedWindowStartMs = cur.serverMs;
        ctx.speedHits = 0;
    }

    ++ctx.speedHits;
    if (!massiveSpike && ctx.speedHits < cfg.speedMediumHits)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Speed;
    evidence.level = (massiveSpike ||
        (ratio >= cfg.speedStrongRatio && ctx.speedHits >= cfg.speedStrongHits)) ?
        AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "SpeedEnvelope";
    evidence.detail = "observed=" + std::to_string(observed) +
        ",allowed=" + std::to_string(threshold) +
        ",hits=" + std::to_string(ctx.speedHits);
    evidence.riskDelta = ClampRiskDelta(3.0f +
        (ratio - 1.0f) * 10.0f +
        std::min(6.0f, static_cast<float>(ctx.speedHits - 1) * 1.5f));
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong &&
        (massiveSpike || ctx.speedHits >= cfg.speedStrongHits);
    evidence.metricA = observed;
    evidence.metricB = threshold;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectTeleport(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.teleportEnabled || !player || ctx.samples.Size() < 2)
        return std::nullopt;

    if (ShouldSkipAllMovementDetectors(movementCtx))
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.mapId != prev.mapId || cur.serverMs <= prev.serverMs)
        return std::nullopt;

    if ((cur.moveFlags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR)) != 0)
        return std::nullopt;

    uint32 dtMs = cur.serverMs - prev.serverMs;
    if (dtMs == 0)
        return std::nullopt;

    float dist = Dist3D(cur, prev);

    float observed = dist * 1000.0f / static_cast<float>(dtMs);
    float allowed = std::max(0.01f, std::max(prev.allowedSpeed, cur.allowedSpeed));
    float threshold = allowed * cfg.teleportSpeedMultiplier;
    float dist2d = Dist2D(cur, prev);
    float deltaZ = std::fabs(cur.z - prev.z);

    bool stationaryCoordinateShift = false;
    bool noIntentNow = !HasActiveMovementIntent(cur.moveFlags);
    bool noIntentPrev = !HasActiveMovementIntent(prev.moveFlags);
    if (noIntentNow && noIntentPrev && dtMs >= 80 && dtMs <= cfg.speedMaxDtMs)
    {
        float stationaryMinDistance2D = std::max({
            cfg.teleportStationaryMinDistance2D,
            cfg.noClipMinSegmentDistance * 0.5f,
            cfg.teleportMinDistance * 0.25f
        });
        float stationaryMinDeltaZ = std::max(cfg.teleportStationaryMinDeltaZ,
            cfg.climbMinRise * 2.0f);
        bool meaningfulShift = dist2d >= stationaryMinDistance2D ||
            deltaZ >= stationaryMinDeltaZ;
        if (meaningfulShift)
        {
            if (ctx.stationaryMoveWindowStartMs == 0 || (cur.serverMs - ctx.stationaryMoveWindowStartMs) > cfg.teleportStationaryWindowMs)
            {
                ctx.stationaryMoveWindowStartMs = cur.serverMs;
                ctx.stationaryMoveHits = 0;
            }

            ++ctx.stationaryMoveHits;
            stationaryCoordinateShift = ctx.stationaryMoveHits >= cfg.teleportStationaryStrongHits;
        }
    }
    else
    {
        ctx.stationaryMoveWindowStartMs = 0;
        ctx.stationaryMoveHits = 0;
    }

    uint32 effectiveDt = dtMs;
    if (cur.clientMs > prev.clientMs)
        effectiveDt = std::min(effectiveDt, cur.clientMs - prev.clientMs);

    float coordinateObserved = dist * 1000.0f / static_cast<float>(std::max<uint32>(1, effectiveDt));
    float coordinateThreshold = allowed * cfg.teleportCoordinateSpeedMultiplier + cfg.speedFlatMargin;
    float axisDelta = MaxAxisDelta(cur, prev);
    bool coordinateJump = axisDelta >= cfg.teleportAxisStrongDelta ||
        (dist >= cfg.teleportCoordinateMinDistance && coordinateObserved > coordinateThreshold);

    bool burstTeleport = !coordinateJump && !stationaryCoordinateShift && observed > threshold;
    bool burstEscalated = false;
    if (burstTeleport)
    {
        if (ctx.teleportBurstWindowStartMs == 0 ||
            (cur.serverMs - ctx.teleportBurstWindowStartMs) >
            cfg.teleportBurstWindowMs)
        {
            ctx.teleportBurstWindowStartMs = cur.serverMs;
            ctx.teleportBurstHits = 0;
        }

        ++ctx.teleportBurstHits;
        burstEscalated =
            ctx.teleportBurstHits >= cfg.teleportBurstStrongHits;
    }
    else
    {
        ctx.teleportBurstWindowStartMs = 0;
        ctx.teleportBurstHits = 0;
    }

    if (!stationaryCoordinateShift && dist < cfg.teleportMinDistance)
        return std::nullopt;

    if (!coordinateJump && !stationaryCoordinateShift && !burstTeleport)
        return std::nullopt;

    float ratio = std::min(6.0f, observed / std::max(0.01f, threshold));
    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Teleport;
    evidence.level =
        (coordinateJump || stationaryCoordinateShift || burstEscalated ||
            ratio > 3.0f) ?
        AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    if (stationaryCoordinateShift)
        evidence.tag = "StationaryCoordinateShift";
    else
        evidence.tag = coordinateJump ? "CoordinateTeleport" : "TeleportBurst";
    evidence.detail =
        "distance=" + std::to_string(dist) +
        ",axis=" + std::to_string(axisDelta) +
        ",burstHits=" + std::to_string(ctx.teleportBurstHits);
    evidence.riskDelta = ClampRiskDelta(
        20.0f + (ratio - 1.0f) * 12.0f +
        (coordinateJump ? 10.0f : 0.0f) +
        (stationaryCoordinateShift ? 8.0f : 0.0f) +
        (burstEscalated ? 6.0f : 0.0f));
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = true;
    evidence.metricA = dist;
    evidence.metricB = axisDelta;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectNoClip(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.noClipEnabled || !cfg.geometryEnabled || !player || ctx.samples.Size() < 2)
        return std::nullopt;

    auto resetWindow = [&ctx]()
    {
        ctx.noClipWindowStartMs = 0;
        ctx.noClipBlockedHits = 0;
    };

    if (movementCtx.isTaxiFlight || movementCtx.recentTaxiFlightGrace ||
        movementCtx.hasTransport || movementCtx.hasVehicle ||
        movementCtx.hasAuthorizedAerialState ||
        movementCtx.recentServerCanFly || movementCtx.recentMountAck)
    {
        resetWindow();
        return std::nullopt;
    }

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.mapId != prev.mapId || cur.serverMs <= prev.serverMs)
    {
        resetWindow();
        return std::nullopt;
    }

    if ((cur.moveFlags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR | MOVEMENTFLAG_SWIMMING)) != 0)
    {
        resetWindow();
        return std::nullopt;
    }

    if (ctx.lastGeometryCheckMs && cur.serverMs > ctx.lastGeometryCheckMs && (cur.serverMs - ctx.lastGeometryCheckMs) < cfg.noClipCheckIntervalMs)
        return std::nullopt;

    AegisMoveSample const* start = &prev;
    uint32 dtMs = cur.serverMs - prev.serverMs;
    float dist2d = Dist2D(cur, prev);
    bool cumulativePath = false;

    if (dtMs < 80 || dtMs > 800)
    {
        resetWindow();
        return std::nullopt;
    }

    if (dist2d < cfg.noClipMinSegmentDistance)
    {
        for (size_t offset = 1; offset < ctx.samples.Size(); ++offset)
        {
            AegisMoveSample const& older = ctx.samples.GetFromNewest(offset);
            if (older.mapId != cur.mapId || cur.serverMs <= older.serverMs)
                break;

            uint32 cumulativeDtMs = cur.serverMs - older.serverMs;
            if (cumulativeDtMs > cfg.noClipCumulativeWindowMs)
                break;

            float cumulativeDist2d = Dist2D(cur, older);
            if (cumulativeDist2d >= cfg.noClipCumulativeMinDistance)
            {
                start = &older;
                dtMs = cumulativeDtMs;
                dist2d = cumulativeDist2d;
                cumulativePath = true;
            }
        }
    }

    if (!cumulativePath && ctx.samples.Size() >= 3)
    {
        AegisMoveSample const& older = ctx.samples.GetFromNewest(2);
        if (older.mapId == prev.mapId && HasSharpDirectionChange2D(older, prev, cur))
        {
            resetWindow();
            return std::nullopt;
        }
    }

    if (dist2d < cfg.noClipMinSegmentDistance || dist2d > cfg.noClipMaxDirectDistance)
    {
        resetWindow();
        return std::nullopt;
    }

    float observed = dist2d * 1000.0f / static_cast<float>(dtMs);
    float allowed = std::max(0.01f, std::max(start->allowedSpeed, cur.allowedSpeed));
    if (observed > allowed * cfg.noClipMaxSpeedMultiplier)
    {
        resetWindow();
        return std::nullopt;
    }

    if (std::fabs(cur.z - start->z) > std::max(3.0f, cfg.climbMinRise * 2.0f))
    {
        resetWindow();
        return std::nullopt;
    }

    ctx.lastGeometryCheckMs = cur.serverMs;
    AegisGeometryResult geometry = _geometry.CheckShortSegment(player, *start, cur, cfg.allowHotPathReachability);
    if (!geometry.blocked)
    {
        resetWindow();
        return std::nullopt;
    }

    AegisGeometryResult longPath;
    bool hasLongPath = false;
    if (cumulativePath && cfg.useMmaps)
    {
        longPath = _geometry.CheckLongPath(player, *start, cur);
        hasLongPath = true;

        if (!longPath.blocked && longPath.pathExists)
        {
            resetWindow();
            return std::nullopt;
        }

        if (ctx.noClipWindowStartMs == 0 || (cur.serverMs - ctx.noClipWindowStartMs) > cfg.noClipCumulativeWindowMs)
        {
            ctx.noClipWindowStartMs = cur.serverMs;
            ctx.noClipBlockedHits = 0;
        }
    }

    if (ctx.noClipWindowStartMs == 0 || (cur.serverMs - ctx.noClipWindowStartMs) > cfg.noClipCumulativeWindowMs)
    {
        ctx.noClipWindowStartMs = cur.serverMs;
        ctx.noClipBlockedHits = 0;
    }

    ++ctx.noClipBlockedHits;
    uint32 requiredHits = cumulativePath ?
        cfg.noClipCumulativeStrongHits :
        std::max<uint32>(2, cfg.noClipCumulativeStrongHits);
    if (ctx.noClipBlockedHits < requiredHits)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::NoClip;
    evidence.level = (!geometry.reachable || geometry.reason == "segment-unreachable" || cumulativePath) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = cumulativePath ? "BlockedMicroPath" : "BlockedSegment";
    evidence.detail = hasLongPath ? geometry.reason + "|" + longPath.reason : geometry.reason;
    evidence.riskDelta = ClampRiskDelta(evidence.level == AegisEvidenceLevel::Strong ? 18.0f : 12.0f);
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong;
    evidence.geometryConfirmed = true;
    evidence.geometryReason = hasLongPath ? longPath.reason : geometry.reason;
    evidence.metricA = geometry.directDistance;
    evidence.metricB = hasLongPath ? std::max(geometry.pathLength, longPath.pathLength) : geometry.pathLength;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectFly(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.flyEnabled || !player || ctx.samples.Size() < 2)
        return std::nullopt;

    if (ShouldSkipAerialMovementDetectors(movementCtx))
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.serverMs <= prev.serverMs)
        return std::nullopt;

    float groundZ = 0.0f;
    if (!_geometry.GetGroundHeight(player, cur.x, cur.y, cur.z, groundZ))
        return std::nullopt;

    float height = std::max(0.0f, cur.z - groundZ);
    float dist2d = Dist2D(cur, prev);
    float deltaZ = std::fabs(cur.z - prev.z);
    float riseZ = cur.z - prev.z;
    bool notFalling = (cur.moveFlags & (MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR)) == 0;
    bool illegalFlags = HasIllegalFlyFlags(cur.moveFlags);
    bool recentJump = movementCtx.recentExtendedJump;
    bool recentFall = movementCtx.recentFall;
    bool likelyDescending = riseZ <= 0.0f;

    if (recentFall && likelyDescending && !illegalFlags && !recentJump)
        return std::nullopt;

    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
    bool platformBetweenPlayerAndGround = false;
    if (height >= cfg.flyMinHeightAboveGround)
    {
        platformBetweenPlayerAndGround = _geometry.RaycastStaticAndDynamic(player,
            cur.x, cur.y, cur.z + 0.2f,
            cur.x, cur.y, groundZ + 0.2f,
            hitX, hitY, hitZ);

        if (!platformBetweenPlayerAndGround)
        {
            platformBetweenPlayerAndGround = HasNearbyVerticalSupport(player,
                _geometry, cur.x, cur.y, cur.z, groundZ,
                cfg.flyMinHeightAboveGround);
        }
    }

    bool sustainedCandidate =
        !illegalFlags &&
        !platformBetweenPlayerAndGround &&
        !recentJump &&
        !(recentFall && likelyDescending) &&
        notFalling &&
        height >= (cfg.flyMinHeightAboveGround + 2.0f) &&
        dist2d >= cfg.flySustainMinHorizontalDistance &&
        deltaZ <= 1.25f;

    if (sustainedCandidate)
    {
        if (ctx.flyWindowStartMs == 0 || (cur.serverMs - ctx.flyWindowStartMs) > cfg.flyAirStallWindowMs)
        {
            ctx.flyWindowStartMs = cur.serverMs;
            ctx.flySuspicionHits = 0;
        }

        ++ctx.flySuspicionHits;
    }
    else if (!illegalFlags)
    {
        ctx.flyWindowStartMs = 0;
        ctx.flySuspicionHits = 0;
    }

    bool suspended = sustainedCandidate && ctx.flySuspicionHits >= 3;

    bool airStallCandidate =
        !illegalFlags &&
        !platformBetweenPlayerAndGround &&
        !recentJump &&
        notFalling &&
        height >= cfg.flyMinHeightAboveGround &&
        deltaZ <= cfg.flyAirStallMaxDeltaZ &&
        dist2d <= cfg.flyAirStallMaxHorizontalDistance;

    if (airStallCandidate)
    {
        if (ctx.airStallWindowStartMs == 0 || (cur.serverMs - ctx.airStallWindowStartMs) > cfg.flyAirStallWindowMs)
        {
            ctx.airStallWindowStartMs = cur.serverMs;
            ctx.airStallHits = 0;
        }

        ++ctx.airStallHits;
    }
    else
    {
        ctx.airStallWindowStartMs = 0;
        ctx.airStallHits = 0;
    }

    bool airStall = airStallCandidate && ctx.airStallHits >= cfg.flyAirStallStrongHits;

    bool lowGravityJump =
        !illegalFlags &&
        !platformBetweenPlayerAndGround &&
        !(recentFall && likelyDescending) &&
        height >= std::max(cfg.superJumpMinHeight * 1.5f, cfg.flyMinHeightAboveGround * 2.0f) &&
        riseZ >= (cfg.superJumpMinDeltaZ * 0.75f) &&
        (recentJump || !notFalling);

    if (illegalFlags && !suspended && !airStall && !lowGravityJump &&
        height < cfg.flyIllegalFlagMinHeightAboveGround &&
        dist2d < cfg.flyIllegalFlagMinHorizontalDistance)
    {
        return std::nullopt;
    }

    if (!illegalFlags && !suspended && !airStall)
    {
        if (!lowGravityJump)
            return std::nullopt;
    }

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Fly;
    evidence.level = (lowGravityJump || airStall || (height >= cfg.flyMinHeightAboveGround && (illegalFlags || suspended))) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    if (lowGravityJump)
        evidence.tag = "LowGravityJump";
    else if (airStall)
        evidence.tag = "AirStall";
    else
        evidence.tag = suspended ? "AirSuspension" : "IllegalFlyFlag";
    evidence.detail = "height=" + std::to_string(height);
    evidence.riskDelta = ClampRiskDelta(12.0f + std::min(12.0f, height) + (illegalFlags ? 4.0f : 0.0f) + (lowGravityJump ? 4.0f : 0.0f) + (airStall ? 5.0f : 0.0f));
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong;
    evidence.metricA = height;
    evidence.metricB = dist2d;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectWaterWalk(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.flyEnabled || !player || ctx.samples.Size() < 2)
        return std::nullopt;

    if (movementCtx.isTaxiFlight || movementCtx.hasTransport || movementCtx.hasVehicle || !movementCtx.hasMap)
        return std::nullopt;

    if (movementCtx.hasWaterWalkAura || movementCtx.hasGhostAura || movementCtx.hasHoverAura ||
        movementCtx.hasAuthorizedAerialState)
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.mapId != prev.mapId || cur.serverMs <= prev.serverMs)
        return std::nullopt;

    if ((cur.moveFlags & (MOVEMENTFLAG_ONTRANSPORT | MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR)) != 0)
        return std::nullopt;

    uint32 dtMs = cur.serverMs - prev.serverMs;
    if (dtMs < 80 || dtMs > cfg.speedMaxDtMs)
        return std::nullopt;

    float dist2d = Dist2D(cur, prev);
    float deltaZ = std::fabs(cur.z - prev.z);
    if (dist2d < cfg.flyWaterWalkMinHorizontalDistance || deltaZ > cfg.flyWaterWalkSurfaceTolerance)
    {
        ctx.waterWalkWindowStartMs = 0;
        ctx.waterWalkHits = 0;
        return std::nullopt;
    }

    float curGroundZ = 0.0f;
    float prevGroundZ = 0.0f;
    if (!_geometry.GetGroundHeight(player, cur.x, cur.y, cur.z, curGroundZ) ||
        !_geometry.GetGroundHeight(player, prev.x, prev.y, prev.z, prevGroundZ))
    {
        ctx.waterWalkWindowStartMs = 0;
        ctx.waterWalkHits = 0;
        return std::nullopt;
    }

    float curWaterLevel = player->GetMap()->GetWaterLevel(cur.x, cur.y);
    float prevWaterLevel = player->GetMap()->GetWaterLevel(prev.x, prev.y);
    if (curWaterLevel <= INVALID_HEIGHT || prevWaterLevel <= INVALID_HEIGHT)
    {
        ctx.waterWalkWindowStartMs = 0;
        ctx.waterWalkHits = 0;
        return std::nullopt;
    }

    float curWaterDepth = curWaterLevel - curGroundZ;
    float prevWaterDepth = prevWaterLevel - prevGroundZ;
    float curSurfaceOffset = std::fabs(cur.z - curWaterLevel);
    float prevSurfaceOffset = std::fabs(prev.z - prevWaterLevel);
    bool swimming = (cur.moveFlags & MOVEMENTFLAG_SWIMMING) != 0;
    bool recentJump = ctx.lastJumpOpcodeMs && (cur.serverMs - ctx.lastJumpOpcodeMs) <= cfg.superJumpWindowMs;

    bool surfaceWaterWalk =
        !swimming &&
        !player->IsUnderWater() &&
        !recentJump &&
        curWaterDepth >= cfg.flyWaterWalkMinWaterDepth &&
        prevWaterDepth >= cfg.flyWaterWalkMinWaterDepth &&
        curSurfaceOffset <= cfg.flyWaterWalkSurfaceTolerance &&
        prevSurfaceOffset <= cfg.flyWaterWalkSurfaceTolerance;

    bool underwaterWalk =
        !swimming &&
        player->IsUnderWater() &&
        !recentJump &&
        (curWaterLevel - cur.z) >= (cfg.flyWaterWalkMinWaterDepth * 0.5f) &&
        (prevWaterLevel - prev.z) >= (cfg.flyWaterWalkMinWaterDepth * 0.5f);

    if (!surfaceWaterWalk && !underwaterWalk)
    {
        ctx.waterWalkWindowStartMs = 0;
        ctx.waterWalkHits = 0;
        return std::nullopt;
    }

    if (ctx.waterWalkWindowStartMs == 0 || (cur.serverMs - ctx.waterWalkWindowStartMs) > cfg.flyWaterWalkWindowMs)
    {
        ctx.waterWalkWindowStartMs = cur.serverMs;
        ctx.waterWalkHits = 0;
    }

    ++ctx.waterWalkHits;
    if (ctx.waterWalkHits < 2)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Fly;
    evidence.level = ctx.waterWalkHits >= cfg.flyWaterWalkStrongHits ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = surfaceWaterWalk ? "SurfaceWaterWalk" : "UnderwaterWalk";
    evidence.detail = "waterDepth=" + std::to_string(curWaterDepth) + ",surfaceOffset=" + std::to_string(curSurfaceOffset);
    evidence.riskDelta = ClampRiskDelta(10.0f + std::min(8.0f, curWaterDepth) + (surfaceWaterWalk ? 3.0f : 1.0f));
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong;
    evidence.metricA = curWaterDepth;
    evidence.metricB = dist2d;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectMount(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.mountEnabled || !player || !movementCtx.isMounted)
    {
        ctx.mountWindowStartMs = 0;
        ctx.mountHits = 0;
        return std::nullopt;
    }

    if (movementCtx.isTaxiFlight || movementCtx.hasVehicle || movementCtx.hasTransport)
    {
        ctx.mountWindowStartMs = 0;
        ctx.mountHits = 0;
        return std::nullopt;
    }

    if (movementCtx.recentMountGrace)
        return std::nullopt;

    Map* map = player->GetMap();
    if (!map)
        return std::nullopt;

    bool allowMount = true;
    if (map->IsDungeon() && !map->IsBattlegroundOrArena())
    {
        allowMount = false;
        if (InstanceTemplate const* instanceTemplate = sObjectMgr->GetInstanceTemplate(map->GetId()))
            allowMount = instanceTemplate->AllowMount;
    }

    bool indoors = !player->IsOutdoors();
    if (!indoors && allowMount)
    {
        ctx.mountWindowStartMs = 0;
        ctx.mountHits = 0;
        return std::nullopt;
    }

    float moved2d = 0.0f;
    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.mapId == prev.mapId && cur.serverMs > prev.serverMs)
        moved2d = Dist2D(cur, prev);

    if (indoors && allowMount && moved2d < cfg.mountIndoorMinMoveDistance)
        return std::nullopt;

    if (ctx.mountWindowStartMs == 0 || (_elapsedMs - ctx.mountWindowStartMs) > cfg.mountIndoorWindowMs)
    {
        ctx.mountWindowStartMs = _elapsedMs;
        ctx.mountHits = 0;
    }

    ++ctx.mountHits;
    if (allowMount && indoors && ctx.mountHits < cfg.mountIndoorStrongHits)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Mount;
    evidence.level = (!allowMount || ctx.mountHits >= cfg.mountIndoorStrongHits) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = allowMount ? "IndoorMount" : "DisallowedMapMount";
    evidence.detail = allowMount ? "indoors-mounted" : "map-disallows-mount";
    evidence.riskDelta = ClampRiskDelta(!allowMount ? 14.0f : 10.0f);
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = false;
    evidence.metricA = moved2d;
    evidence.metricB = static_cast<float>(ctx.mountHits);
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectClimb(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!cfg.climbEnabled || !player || ctx.samples.Size() < 2)
        return std::nullopt;

    if (ShouldSkipAerialMovementDetectors(movementCtx))
        return std::nullopt;

    AegisMoveSample const& cur = ctx.samples.Newest();
    AegisMoveSample const& prev = ctx.samples.Previous();
    if (cur.serverMs <= prev.serverMs)
        return std::nullopt;

    if ((cur.moveFlags & MOVEMENTFLAG_SWIMMING) != 0)
        return std::nullopt;

    uint32 dtMs = cur.serverMs - prev.serverMs;
    if (dtMs < 80 || dtMs > 900)
        return std::nullopt;

    float dist2d = Dist2D(cur, prev);
    float deltaZ = cur.z - prev.z;
    if (dist2d < cfg.climbMinHorizontalDistance && deltaZ < cfg.superJumpMinDeltaZ)
        return std::nullopt;

    bool recentJump = movementCtx.recentJump;
    if (recentJump)
    {
        float groundZ = 0.0f;
        if (_geometry.GetGroundHeight(player, cur.x, cur.y, cur.z, groundZ))
        {
            float heightOverGround = std::max(0.0f, cur.z - groundZ);
            float hitX = 0.0f;
            float hitY = 0.0f;
            float hitZ = 0.0f;
            bool platformBetweenPlayerAndGround =
                heightOverGround >= cfg.superJumpMinHeight &&
                _geometry.RaycastStaticAndDynamic(player,
                    cur.x, cur.y, cur.z + 0.2f,
                    cur.x, cur.y, groundZ + 0.2f,
                    hitX, hitY, hitZ);
            if (!platformBetweenPlayerAndGround &&
                heightOverGround >= cfg.superJumpMinHeight &&
                deltaZ >= cfg.superJumpMinDeltaZ)
            {
                if (ctx.superJumpWindowStartMs == 0 || (cur.serverMs - ctx.superJumpWindowStartMs) > cfg.superJumpWindowMs)
                {
                    ctx.superJumpWindowStartMs = cur.serverMs;
                    ctx.superJumpHitsInWindow = 0;
                }

                ++ctx.superJumpHitsInWindow;

                AegisEvidenceEvent evidence;
                evidence.cheatType = AegisCheatType::Climb;
                evidence.level = ctx.superJumpHitsInWindow >= 2 ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
                evidence.tag = "SuperJumpApex";
                evidence.detail = "height=" + std::to_string(heightOverGround);
                evidence.riskDelta = ClampRiskDelta(14.0f + std::min(10.0f, heightOverGround));
                evidence.serverMs = cur.serverMs;
                evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong || heightOverGround >= (cfg.superJumpMinHeight * 1.5f);
                evidence.metricA = heightOverGround;
                evidence.metricB = deltaZ;
                return evidence;
            }
        }
    }

    if ((cur.moveFlags & (MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR)) != 0)
        return std::nullopt;

    if (deltaZ < cfg.climbMinRise || dist2d < cfg.climbMinHorizontalDistance)
        return std::nullopt;

    bool activeFlightFlags = (cur.moveFlags & (MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY | MOVEMENTFLAG_DISABLE_GRAVITY)) != 0;
    if (activeFlightFlags)
        return std::nullopt;

    float slopeRatio = deltaZ / std::max(0.1f, dist2d);
    if (slopeRatio < cfg.climbMinSlopeRatio)
        return std::nullopt;

    AegisGeometryResult geometry = _geometry.CheckShortSegment(player, prev, cur, cfg.allowHotPathReachability);
    if (!geometry.blocked)
    {
        float curGroundZ = 0.0f;
        float prevGroundZ = 0.0f;
        bool haveCurGround = _geometry.GetGroundHeight(player, cur.x, cur.y, cur.z, curGroundZ);
        bool havePrevGround = _geometry.GetGroundHeight(player, prev.x, prev.y, prev.z, prevGroundZ);
        if ((haveCurGround && (cur.z - curGroundZ) > cfg.flyMinHeightAboveGround) ||
            (havePrevGround && (prev.z - prevGroundZ) > cfg.flyMinHeightAboveGround))
            return std::nullopt;
    }

    if (ctx.climbWindowStartMs == 0 || (cur.serverMs - ctx.climbWindowStartMs) > cfg.climbWindowMs)
    {
        ctx.climbWindowStartMs = cur.serverMs;
        ctx.climbHitsInWindow = 0;
    }

    ++ctx.climbHitsInWindow;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Climb;
    evidence.level = (ctx.climbHitsInWindow >= cfg.climbStrongHits || geometry.blocked) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = geometry.blocked ? "WallClimbBlocked" : "SteepClimb";
    evidence.detail = geometry.reason.empty() ? "slope-only" : geometry.reason;
    evidence.riskDelta = ClampRiskDelta(10.0f + slopeRatio * 4.0f + (geometry.blocked ? 4.0f : 0.0f));
    evidence.serverMs = cur.serverMs;
    evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong || geometry.blocked;
    evidence.geometryConfirmed = geometry.blocked;
    evidence.geometryReason = geometry.reason;
    evidence.metricA = slopeRatio;
    evidence.metricB = deltaZ;
    return evidence;
}

std::optional<AegisEvidenceEvent> AcAegisMgr::DetectAfk(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    uint32 nowMs = movementCtx.nowMs;
    if (!cfg.afkEnabled || !player || ctx.gather.windowStartMs == 0)
        return std::nullopt;

    if ((nowMs - ctx.gather.windowStartMs) < cfg.afkWindowMs)
        return std::nullopt;
    if (ctx.gather.actionCount < cfg.afkMinActions)
        return std::nullopt;
    if (ctx.gather.lootCount < cfg.afkMinLootCount && ctx.gather.gatherCount < cfg.afkMinGatherCount)
        return std::nullopt;
    if (ctx.gather.lastEvidenceMs && (nowMs - ctx.gather.lastEvidenceMs) < cfg.afkEvidenceCooldownMs)
        return std::nullopt;
    if (ctx.gather.lastWhitelistActionMs && (nowMs - ctx.gather.lastWhitelistActionMs) < cfg.afkIgnoreActionGraceMs)
        return std::nullopt;

    if (movementCtx.hasAfkIgnoreAura)
        return std::nullopt;

    if (!movementCtx.isAlive || movementCtx.isInCombat || movementCtx.isTaxiFlight || movementCtx.hasTransport || movementCtx.hasVehicle)
        return std::nullopt;

    float moved = Dist3DToPoint(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), ctx.gather.startX, ctx.gather.startY, ctx.gather.startZ);
    if (moved > cfg.afkMaxMoveDistance)
        return std::nullopt;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Afk;
    evidence.level = (ctx.gather.actionCount >= cfg.afkStrongActions && moved <= cfg.afkStrongMoveDistance) ? AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "GatherLoop";
    evidence.detail = "actions=" + std::to_string(ctx.gather.actionCount) + ",gather=" + std::to_string(ctx.gather.gatherCount) + ",loot=" + std::to_string(ctx.gather.lootCount) + ",moved=" + std::to_string(moved);
    evidence.riskDelta = ClampRiskDelta(6.0f + std::min(18.0f, static_cast<float>(ctx.gather.actionCount) * 0.8f));
    evidence.serverMs = nowMs;
    evidence.metricA = static_cast<float>(ctx.gather.actionCount);
    evidence.metricB = moved;
    return evidence;
}

void AcAegisMgr::NotifyGms(Player* player, AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx) const
{
    if (!player)
        return;

    std::string text = BuildGmMessage(player, evidence, ctx);
    WorldPacket data(SMSG_NOTIFICATION, text.size() + 1);
    data << text;
    sWorldSessionMgr->SendGlobalGMMessage(&data);

    std::ostringstream audit;
    audit << "\"type\":\"" << CheatTypeText(evidence.cheatType) << "\""
          << ",\"level\":\"" << EvidenceLevelText(evidence.level) << "\""
          << ",\"tag\":\"" << EscapeJson(evidence.tag) << "\""
          << ",\"message\":\"" << EscapeJson(text) << "\"";
    WriteAuditLog("gm_notify", player, &ctx, audit.str());
}

void AcAegisMgr::Rollback(Player* player, AegisPlayerContext& ctx) const
{
    if (!player)
        return;

    if (!CanSafelyTeleportForPunish(player))
    {
        WriteAuditLog("rollback_skipped", player, &ctx,
            "\"reason\":\"unsafe-teleport-state\"");
        return;
    }

    if (!ctx.safePosition.valid)
    {
        WriteAuditLog("rollback_skipped", player, &ctx,
            "\"reason\":\"no-safe-position\"");
        return;
    }

    if (ctx.safePosition.mapId == player->GetMapId())
        player->NearTeleportTo(ctx.safePosition.x, ctx.safePosition.y, ctx.safePosition.z, ctx.safePosition.o);
    else
        player->TeleportTo(ctx.safePosition.mapId, ctx.safePosition.x, ctx.safePosition.y, ctx.safePosition.z, ctx.safePosition.o);

    ctx.lastTeleportMs = _elapsedMs;

    std::ostringstream audit;
    audit << "\"targetMap\":" << ctx.safePosition.mapId
          << ",\"targetX\":" << ctx.safePosition.x
          << ",\"targetY\":" << ctx.safePosition.y
          << ",\"targetZ\":" << ctx.safePosition.z
          << ",\"targetO\":" << ctx.safePosition.o;
    WriteAuditLog("rollback", player, &ctx, audit.str());
}

void AcAegisMgr::ApplyDebuff(Player* player) const
{
    if (!player)
        return;

    for (uint32 spellId : sAcAegisConfig->Get().debuffSpellIds)
        player->CastSpell(player, spellId, true);
}

void AcAegisMgr::ClearDebuffs(Player* player) const
{
    if (!player)
        return;

    for (uint32 spellId : sAcAegisConfig->Get().debuffSpellIds)
        player->RemoveAurasDueToSpell(spellId);
}

void AcAegisMgr::SetHomebind(Player* player, uint32 mapId, float x, float y, float z) const
{
    if (!player)
        return;

    WorldLocation location(mapId, x, y, z, 0.0f);
    uint32 areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, mapId, x, y, z);
    player->SetHomebind(location, areaId);
}

void AcAegisMgr::Jail(Player* player, AegisPlayerContext& ctx) const
{
    if (!player)
        return;

    AegisConfig const& cfg = sAcAegisConfig->Get();
    SetHomebind(player, cfg.jailMapId, cfg.jailX, cfg.jailY, cfg.jailZ);

    if (!CanSafelyTeleportForPunish(player))
    {
        WriteAuditLog("jail_deferred", player, &ctx,
            "\"reason\":\"unsafe-teleport-state\"");
        return;
    }

    player->TeleportTo(cfg.jailMapId, cfg.jailX, cfg.jailY, cfg.jailZ, cfg.jailO);
    if (cfg.jailKeepDebuff)
        ApplyDebuff(player);
    ctx.lastTeleportMs = _elapsedMs;
    ctx.lastJailReturnCheckMs = _elapsedMs;
    ResetGatherWindow(player, ctx, "jailed");

    std::ostringstream payload;
    payload << "{\"guid\":" << player->GetGUID().GetCounter()
            << ",\"map\":" << cfg.jailMapId
            << ",\"zone\":" << player->GetZoneId()
            << ",\"x\":" << cfg.jailX
            << ",\"y\":" << cfg.jailY
            << ",\"z\":" << cfg.jailZ
            << ",\"ip\":\"" << EscapeJson(player->GetSession() ? player->GetSession()->GetRemoteAddress() : "")
            << "\"}";
    WritePanelCharacterLog("aegis_jail", "success", payload.str());

    std::ostringstream audit;
    audit << "\"targetMap\":" << cfg.jailMapId
          << ",\"targetX\":" << cfg.jailX
          << ",\"targetY\":" << cfg.jailY
          << ",\"targetZ\":" << cfg.jailZ
          << ",\"targetO\":" << cfg.jailO
          << ",\"keepDebuff\":" << (cfg.jailKeepDebuff ? "true" : "false");
    WriteAuditLog("jail_apply", player, &ctx, audit.str());
}

void AcAegisMgr::Release(Player* player, AegisPlayerContext& ctx) const
{
    if (!player)
        return;

    if (!CanSafelyTeleportForPunish(player))
    {
        WriteAuditLog("release_deferred", player, &ctx,
            "\"reason\":\"unsafe-teleport-state\"");
        return;
    }

    AegisConfig const& cfg = sAcAegisConfig->Get();
    ClearDebuffs(player);
    SetHomebind(player, cfg.releaseMapId, cfg.releaseX, cfg.releaseY, cfg.releaseZ);
    if (!IsNearPosition(player, cfg.releaseMapId, cfg.releaseX, cfg.releaseY, cfg.releaseZ, 5.0f))
        player->TeleportTo(cfg.releaseMapId, cfg.releaseX, cfg.releaseY, cfg.releaseZ, cfg.releaseO);
    ctx.lastTeleportMs = _elapsedMs;
    float riskBefore = ctx.riskScore;
    ctx.riskScore *= cfg.releaseRiskFactor;
    ctx.gather.suspiciousWindows = 0;

    std::ostringstream payload;
    payload << "{\"guid\":" << player->GetGUID().GetCounter()
            << ",\"map\":" << cfg.releaseMapId
            << ",\"zone\":" << player->GetZoneId()
            << ",\"x\":" << cfg.releaseX
            << ",\"y\":" << cfg.releaseY
            << ",\"z\":" << cfg.releaseZ
            << ",\"ip\":\"" << EscapeJson(player->GetSession() ? player->GetSession()->GetRemoteAddress() : "")
            << "\"}";
    WritePanelCharacterLog("aegis_release", "success", payload.str());

    std::ostringstream audit;
    audit << "\"targetMap\":" << cfg.releaseMapId
          << ",\"targetX\":" << cfg.releaseX
          << ",\"targetY\":" << cfg.releaseY
          << ",\"targetZ\":" << cfg.releaseZ
          << ",\"targetO\":" << cfg.releaseO
          << ",\"riskBefore\":" << static_cast<uint32>(riskBefore)
          << ",\"riskAfter\":" << static_cast<uint32>(ctx.riskScore);
    WriteAuditLog("release_apply", player, &ctx, audit.str());
}

std::string AcAegisMgr::BuildBanReason(AegisEvidenceEvent const& evidence,
    uint32 offenseCount, uint8 offenseTier, float risk) const
{
    std::string reason = sAcAegisConfig->Get().banReasonTemplate;
    ReplaceAll(reason, "{type}", CheatTypeText(evidence.cheatType));
    ReplaceAll(reason, "{tier}", std::to_string(offenseTier));
    ReplaceAll(reason, "{offense}", std::to_string(offenseCount));
    ReplaceAll(reason, "{risk}", std::to_string(static_cast<uint32>(risk)));
    return reason;
}

std::string AcAegisMgr::ExecuteBan(Player* player, AegisActionDecision const& decision) const
{
    if (!player)
        return "BAN_NOTFOUND";

    AegisConfig const& cfg = sAcAegisConfig->Get();
    std::string duration = decision.banSeconds == 0 ? cfg.banPermanentDurationToken : std::to_string(decision.banSeconds);

    BanReturn result = BAN_NOTFOUND;
    if (decision.banMode == "character")
    {
        result = sBan->BanCharacter(player->GetName(), duration, decision.reason, cfg.banAuthor);
    }
    else if (decision.banMode == "account")
    {
        std::string accountName;
        if (player->GetSession() && AccountMgr::GetName(player->GetSession()->GetAccountId(), accountName))
            result = sBan->BanAccount(accountName, duration, decision.reason, cfg.banAuthor);
        else
            result = BAN_NOTFOUND;
    }
    else
    {
        result = sBan->BanAccountByPlayerName(player->GetName(), duration, decision.reason, cfg.banAuthor);
    }

    return BanReturnText(result);
}

bool AcAegisMgr::ClearCoreBanState(uint32 guidLow, uint32 accountId, std::string const& banMode, Player* player) const
{
    std::string characterName;
    if (player)
        characterName = player->GetName();
    else
        sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(guidLow), characterName);

    if (accountId == 0)
    {
        if (player && player->GetSession())
            accountId = player->GetSession()->GetAccountId();
        else
            accountId = sCharacterCache->GetCharacterAccountIdByGuid(ObjectGuid::Create<HighGuid::Player>(guidLow));
    }

    std::string accountName;
    if (accountId != 0)
        AccountMgr::GetName(accountId, accountName);

    bool cleared = false;
    if (banMode == "character")
    {
        if (!characterName.empty())
            cleared = sBan->RemoveBanCharacter(characterName);
    }
    else if (banMode == "account")
    {
        if (!accountName.empty())
            cleared = sBan->RemoveBanAccount(accountName);
        else if (!characterName.empty())
            cleared = sBan->RemoveBanAccountByPlayerName(characterName);
    }
    else if (banMode == "account-by-character")
    {
        if (!characterName.empty())
            cleared = sBan->RemoveBanAccountByPlayerName(characterName);
    }
    else
    {
        if (!characterName.empty())
            cleared = sBan->RemoveBanCharacter(characterName) || cleared;
        if (!accountName.empty())
            cleared = sBan->RemoveBanAccount(accountName) || cleared;
        else if (!characterName.empty())
            cleared = sBan->RemoveBanAccountByPlayerName(characterName) || cleared;
    }

    return cleared;
}

void AcAegisMgr::SavePunishState(Player* player, AegisPlayerContext const& ctx) const
{
    SavePunishState(player ? player->GetGUID().GetCounter() : 0,
        (player && player->GetSession()) ? player->GetSession()->GetAccountId() : 0,
        ctx);
}

void AcAegisMgr::SavePunishState(uint32 guidLow, uint32 accountId,
    AegisPlayerContext const& ctx) const
{
    AegisOffenseRecord record;
    record.guid = guidLow;
    record.accountId = accountId;
    record.offenseCount = ctx.punish.offenseCount;
    record.offenseTier = ctx.punish.offenseTier;
    record.punishStage = static_cast<uint8>(ctx.punish.punishStage);
    record.lastCheatType = ctx.punish.lastCheatType;
    record.lastOffenseEpoch = ctx.punish.lastOffenseEpoch;
    record.debuffUntilEpoch = ctx.punish.debuffUntilEpoch;
    record.jailUntilEpoch = ctx.punish.jailUntilEpoch;
    record.banUntilEpoch = ctx.punish.banUntilEpoch;
    record.permanentBan = ctx.punish.permanentBan;
    record.lastReason = ctx.punish.lastReason;
    record.lastBanMode = ctx.punish.lastBanMode;
    record.lastBanResult = ctx.punish.lastBanResult;
    _persistence.SaveOffense(record);
}

AegisActionDecision AcAegisMgr::DetermineAction(Player* /*player*/, AegisPlayerContext const& ctx, AegisEvidenceEvent const& evidence) const
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    AegisActionDecision decision;
    uint32 nowMs = ctx.samples.Empty() ? _elapsedMs : ctx.samples.Newest().serverMs;

    AegisPunishStage baseStage = BasePunishStage(evidence);

    decision.shouldNotify =
        (ctx.riskScore >= cfg.notifyThreshold ||
            (evidence.level == AegisEvidenceLevel::Strong &&
                StageRank(baseStage) >= StageRank(AegisPunishStage::Jail))) &&
        (ctx.lastNotifyMs == 0 || nowMs < ctx.lastNotifyMs || (nowMs - ctx.lastNotifyMs) >= cfg.gmNotifyCooldownMs || ctx.lastEvidenceTag != evidence.tag);

    if (evidence.cheatType == AegisCheatType::Afk &&
        (ctx.gather.suspiciousWindows + 1) < cfg.afkMinSuspiciousWindows &&
        StageRank(baseStage) > StageRank(AegisPunishStage::Debuff))
    {
        baseStage = AegisPunishStage::Debuff;
    }

    AegisPunishStage stageByEvidence = cfg.offenseEnabled ?
        RaisePunishStage(baseStage, ctx.punish.offenseTier, cfg) :
        baseStage;
    AegisPunishStage stageByRisk = RiskThresholdStage(ctx.riskScore, cfg);
    AegisPunishStage finalStage = ApplyRiskGate(stageByEvidence, stageByRisk);

    uint32 pendingPunishOffenseCount =
        ctx.punish.offenseCount +
        (ShouldCountOffenseForPunishStage(finalStage, cfg) ? 1u : 0u);
    uint8 pendingPunishTier = ShouldCountOffenseForPunishStage(finalStage, cfg) ?
        static_cast<uint8>(std::min<uint32>(cfg.offenseMaxTier,
            ctx.punish.offenseTier + 1)) :
        ctx.punish.offenseTier;

    if (StageRank(finalStage) >= StageRank(AegisPunishStage::TempBan))
    {
        bool allowBan =
            (!cfg.banStrongEvidenceRequired ||
                evidence.level == AegisEvidenceLevel::Strong) &&
            (!cfg.offenseEnabled ||
                pendingPunishOffenseCount >= cfg.banMinOffenseCount);

        if (!allowBan)
            finalStage = AegisPunishStage::Kick;
        else if (stageByEvidence == AegisPunishStage::PermBan &&
            cfg.stage5Permanent &&
            pendingPunishTier >= cfg.banPermanentTier)
            finalStage = AegisPunishStage::PermBan;
        else
            finalStage = AegisPunishStage::TempBan;
    }

    bool positionCheatNeedsRollback =
        IsPositionMovementCheat(evidence.cheatType) &&
        evidence.cheatType != AegisCheatType::Speed &&
        evidence.level >= AegisEvidenceLevel::Medium;

    bool speedNeedsRollback =
        evidence.cheatType == AegisCheatType::Speed &&
        evidence.level == AegisEvidenceLevel::Strong &&
        finalStage != AegisPunishStage::Observe;

    decision.shouldRollback = cfg.rollbackEnabled &&
        (positionCheatNeedsRollback || speedNeedsRollback ||
            (evidence.shouldRollback &&
                (evidence.level == AegisEvidenceLevel::Strong ||
                    ctx.riskScore >= cfg.rollbackThreshold)));

    decision.persistOffense = ShouldPersistOffense(finalStage,
        decision.shouldRollback, cfg);
    decision.pendingOffenseCount = ctx.punish.offenseCount +
        (decision.persistOffense ? 1u : 0u);
    decision.pendingOffenseTier = decision.persistOffense ?
        static_cast<uint8>(std::min<uint32>(cfg.offenseMaxTier,
            std::max<uint32>(1, ctx.punish.offenseTier + 1))) :
        ctx.punish.offenseTier;

    if (finalStage == AegisPunishStage::Observe &&
        !decision.shouldRollback)
        return decision;

    decision.reason = BuildBanReason(evidence,
        decision.pendingOffenseCount,
        decision.pendingOffenseTier,
        ctx.riskScore);

    switch (finalStage)
    {
    case AegisPunishStage::Debuff:
        decision.primaryAction = AegisActionType::Debuff;
        decision.debuffSeconds = cfg.stage1DebuffSeconds;
        break;
    case AegisPunishStage::Jail:
        decision.primaryAction = AegisActionType::Jail;
        decision.jailSeconds = cfg.stage2JailSeconds;
        break;
    case AegisPunishStage::Kick:
        decision.primaryAction = AegisActionType::Kick;
        break;
    case AegisPunishStage::TempBan:
        if (cfg.banMode == "character")
            decision.primaryAction = AegisActionType::BanCharacter;
        else if (cfg.banMode == "account")
            decision.primaryAction = AegisActionType::BanAccount;
        else
            decision.primaryAction = AegisActionType::BanAccountByCharacter;
        decision.banMode = cfg.banMode;
        decision.banSeconds =
            (decision.pendingOffenseTier >= 4 ?
                cfg.banStage4Days : cfg.banStage3Days) * DAY;
        break;
    case AegisPunishStage::PermBan:
        if (cfg.banMode == "character")
            decision.primaryAction = AegisActionType::BanCharacter;
        else if (cfg.banMode == "account")
            decision.primaryAction = AegisActionType::BanAccount;
        else
            decision.primaryAction = AegisActionType::BanAccountByCharacter;
        decision.banMode = cfg.banMode;
        decision.banSeconds = 0;
        break;
    default:
        break;
    }

    return decision;
}

bool AcAegisMgr::ExecuteAction(Player* player, AegisPlayerContext& ctx, AegisEvidenceEvent const& evidence, AegisActionDecision const& decision)
{
    if (!player)
        return false;

    AegisConfig const& cfg = sAcAegisConfig->Get();
    std::string playerName = player->GetName();
    uint32 playerGuid = player->GetGUID().GetCounter();
    uint32 accountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;
    std::string remoteAddress = player->GetSession() ? player->GetSession()->GetRemoteAddress() : "";
    uint32 mapId = player->GetMapId();
    uint32 zoneId = player->GetZoneId();
    uint32 areaId = player->GetAreaId();
    float posX = player->GetPositionX();
    float posY = player->GetPositionY();
    float posZ = player->GetPositionZ();
    float orientation = player->GetOrientation();
    uint32 nowMs = ctx.samples.Empty() ? _elapsedMs : ctx.samples.Newest().serverMs;
    int64 nowEpoch = static_cast<int64>(std::time(nullptr));
    AegisPunishStage stageBefore = ctx.punish.punishStage;
    AegisPunishState punishBefore = ctx.punish;
    uint32 offenseBefore = ctx.punish.offenseCount;
    uint8 tierBefore = ctx.punish.offenseTier;
    std::string executionResult = "observe";
    bool actionTaken = false;
    bool rollbackExecuted = false;
    AegisActionType announcedAction = AegisActionType::None;
    bool canTeleportForPunish = CanSafelyTeleportForPunish(player);

    if (decision.shouldNotify)
    {
        NotifyGms(player, evidence, ctx);
        ctx.lastNotifyMs = nowMs;
    }

    if (evidence.cheatType == AegisCheatType::Mount && player->IsMounted())
    {
        player->Dismount();
        actionTaken = true;
        if (executionResult == "observe")
            executionResult = "mount-removed";
    }

    if (decision.shouldRollback)
    {
        if (canTeleportForPunish)
        {
            Rollback(player, ctx);
            rollbackExecuted = true;
            announcedAction = AegisActionType::Rollback;
        }
        else if (executionResult == "observe")
            executionResult = "rollback-deferred-unsafe-state";
    }

    if (decision.persistOffense && cfg.offenseEnabled)
    {
        ctx.punish.loaded = true;
        ctx.punish.offenseCount = decision.pendingOffenseCount;
        ctx.punish.offenseTier = decision.pendingOffenseTier;
        ctx.punish.lastOffenseEpoch = nowEpoch;
        ctx.punish.lastCheatType = static_cast<uint8>(evidence.cheatType);
        ctx.punish.lastReason = decision.reason;
        ctx.punish.lastBanMode.clear();
        ctx.punish.lastBanResult.clear();
    }

    switch (decision.primaryAction)
    {
    case AegisActionType::Debuff:
        if (cfg.debuffEnabled)
        {
            ApplyDebuff(player);
            ctx.punish.punishStage = AegisPunishStage::Debuff;
            ctx.punish.debuffUntilEpoch = nowEpoch + decision.debuffSeconds;
            executionResult = "debuff-applied";
            actionTaken = true;
            announcedAction = AegisActionType::Debuff;
            if (player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(BuildPunishPlayerMessage(cfg.debuffApplyMessage, FormatDurationShort(decision.debuffSeconds)));
        }
        else
            executionResult = "debuff-disabled";
        break;
    case AegisActionType::Jail:
        if (cfg.jailEnabled)
        {
            ctx.punish.punishStage = AegisPunishStage::Jail;
            ctx.punish.jailUntilEpoch = nowEpoch + decision.jailSeconds;
            SetHomebind(player, cfg.jailMapId, cfg.jailX, cfg.jailY, cfg.jailZ);
            if (canTeleportForPunish)
            {
                Jail(player, ctx);
                executionResult = "jail-applied";
                actionTaken = true;
                announcedAction = AegisActionType::Jail;
                if (player->GetSession())
                    ChatHandler(player->GetSession()).SendSysMessage(BuildPunishPlayerMessage(cfg.jailApplyMessage, FormatDurationShort(decision.jailSeconds)));
            }
            else
                executionResult = "jail-deferred-unsafe-state";
        }
        else
            executionResult = "jail-disabled";
        break;
    case AegisActionType::Kick:
        if (cfg.kickEnabled && player->GetSession())
        {
            ctx.punish.punishStage = AegisPunishStage::Kick;
            player->GetSession()->KickPlayer("AcAegis kick");
            executionResult = "kick-issued";
            actionTaken = true;
            announcedAction = AegisActionType::Kick;
        }
        else
            executionResult = "kick-skipped";
        break;
    case AegisActionType::BanCharacter:
    case AegisActionType::BanAccountByCharacter:
    case AegisActionType::BanAccount:
        if (cfg.banEnabled)
        {
            ctx.punish.punishStage = decision.banSeconds == 0 ? AegisPunishStage::PermBan : AegisPunishStage::TempBan;
            ctx.punish.permanentBan = decision.banSeconds == 0;
            ctx.punish.banUntilEpoch = decision.banSeconds == 0 ? 0 : (nowEpoch + decision.banSeconds);
            ctx.punish.lastBanMode = decision.banMode;
            ctx.punish.lastBanResult = ExecuteBan(player, decision);
            WritePanelAccountBanLog(playerGuid, accountId, remoteAddress, evidence, decision);
            if (player->GetSession())
                player->GetSession()->KickPlayer("AcAegis ban");
            executionResult = ctx.punish.lastBanResult;
            actionTaken = true;
            announcedAction = decision.primaryAction;
        }
        else
            executionResult = "ban-disabled";
        break;
    default:
        executionResult = decision.shouldNotify || decision.shouldRollback ?
            "observe-with-side-effects" : "observe";
        break;
    }

    if (PersistentPunishStateChanged(punishBefore, ctx.punish))
        SavePunishState(playerGuid, accountId, ctx);

    if (cfg.punishBroadcastEnabled && (actionTaken || rollbackExecuted) &&
        announcedAction != AegisActionType::None)
    {
        std::string cheatLabel = std::string(CheatTypeTextZh(evidence.cheatType));
        if (!evidence.tag.empty())
            cheatLabel += "[" + evidence.tag + "]";

        ChatHandler(nullptr).SendWorldText(
            "玩家 |cffff0000{}|r 因 |cffff0000{}作弊|r，被 |cffff0000{}|r，请各位英雄引以为戒，规范游戏。",
            playerName,
            cheatLabel,
            ActionTypeTextZh(announcedAction));
    }

    std::ostringstream audit;
    audit << "\"type\":\"" << CheatTypeText(evidence.cheatType) << "\""
          << ",\"tag\":\"" << EscapeJson(evidence.tag) << "\""
          << ",\"action\":\"" << EscapeJson(ActionText(decision)) << "\""
          << ",\"result\":\"" << EscapeJson(executionResult) << "\""
          << ",\"notify\":" << (decision.shouldNotify ? "true" : "false")
          << ",\"rollback\":" << (decision.shouldRollback ? "true" : "false")
          << ",\"persistOffense\":" << (decision.persistOffense ? "true" : "false")
          << ",\"stageBefore\":\"" << StageText(stageBefore) << "\""
          << ",\"stageAfter\":\"" << StageText(ctx.punish.punishStage) << "\""
          << ",\"offenseBefore\":" << offenseBefore
          << ",\"offenseAfter\":" << ctx.punish.offenseCount
          << ",\"tierBefore\":" << static_cast<uint32>(tierBefore)
          << ",\"tierAfter\":" << static_cast<uint32>(ctx.punish.offenseTier)
          << ",\"banMode\":\"" << EscapeJson(decision.banMode) << "\""
          << ",\"banSeconds\":" << decision.banSeconds
          << ",\"reason\":\"" << EscapeJson(decision.reason) << "\""
          << ",\"banResult\":\"" << EscapeJson(ctx.punish.lastBanResult) << "\"";
    std::ostringstream actionPayload;
    actionPayload << "{\"event\":\"action\""
                  << ",\"player\":\"" << EscapeJson(playerName) << "\""
                  << ",\"guid\":" << playerGuid
                  << ",\"account\":" << accountId
                  << ",\"ip\":\"" << EscapeJson(remoteAddress) << "\""
                  << ",\"map\":" << mapId
                  << ",\"zone\":" << zoneId
                  << ",\"area\":" << areaId
                  << ",\"x\":" << posX
                  << ",\"y\":" << posY
                  << ",\"z\":" << posZ
                  << ",\"o\":" << orientation;

    std::string ctxFields = BuildAuditCommonFields(nullptr, &ctx);
    if (!ctxFields.empty())
        actionPayload << ',' << ctxFields;
    actionPayload << ',' << audit.str() << '}';
    WriteAegisLog("audit", actionPayload.str());
    return actionTaken;
}

bool AcAegisMgr::HandleEvidence(Player* player, AegisPlayerContext& ctx, AegisEvidenceEvent const& evidence)
{
    if (!player)
        return false;

    DecayRisk(ctx, evidence.serverMs);
    ctx.riskScore += ClampRiskDelta(evidence.riskDelta);
    ctx.lastEvidenceMs = evidence.serverMs;
    ctx.lastEvidenceTag = evidence.tag;
    if (evidence.cheatType == AegisCheatType::Afk)
    {
        ctx.gather.lastEvidenceMs = evidence.serverMs;
        ++ctx.gather.suspiciousWindows;
    }

    AegisEventRecord eventRecord;
    eventRecord.guid = player->GetGUID().GetCounter();
    eventRecord.accountId = player->GetSession() ?
        player->GetSession()->GetAccountId() : 0;
    eventRecord.mapId = player->GetMapId();
    eventRecord.zoneId = player->GetZoneId();
    eventRecord.areaId = player->GetAreaId();
    eventRecord.cheatType = static_cast<uint8>(evidence.cheatType);
    eventRecord.evidenceLevel = static_cast<uint8>(evidence.level);
    eventRecord.riskDelta = evidence.riskDelta;
    eventRecord.totalRiskAfter = ctx.riskScore;
    eventRecord.evidenceTag = evidence.tag;
    eventRecord.detailText = evidence.detail;
    eventRecord.posX = player->GetPositionX();
    eventRecord.posY = player->GetPositionY();
    eventRecord.posZ = player->GetPositionZ();

    _persistence.QueueEvent(eventRecord);
    WritePanelDetectionLog(player, evidence, ctx);
    AegisActionDecision decision = DetermineAction(player, ctx, evidence);

    std::ostringstream audit;
    audit << "\"type\":\"" << CheatTypeText(evidence.cheatType) << "\""
          << ",\"level\":\"" << EvidenceLevelText(evidence.level) << "\""
          << ",\"tag\":\"" << EscapeJson(evidence.tag) << "\""
          << ",\"detail\":\"" << EscapeJson(evidence.detail) << "\""
          << ",\"riskDelta\":" << evidence.riskDelta
          << ",\"metricA\":" << evidence.metricA
          << ",\"metricB\":" << evidence.metricB
          << ",\"rollbackHint\":" << (evidence.shouldRollback ? "true" : "false")
          << ",\"geometryConfirmed\":"
          << (evidence.geometryConfirmed ? "true" : "false")
          << ",\"geometryReason\":\"" << EscapeJson(evidence.geometryReason) << "\""
          << ",\"decisionAction\":\"" << EscapeJson(ActionText(decision)) << "\""
          << ",\"decisionNotify\":" << (decision.shouldNotify ? "true" : "false")
          << ",\"decisionRollback\":" << (decision.shouldRollback ? "true" : "false")
          << ",\"decisionPersistOffense\":"
          << (decision.persistOffense ? "true" : "false");
    WriteAuditLog("evidence", player, &ctx, audit.str());

    return ExecuteAction(player, ctx, evidence, decision);
}

void AcAegisMgr::ReloadConfig()
{
    sAcAegisConfig->Reload();
    EnsureFileLogReady();
    WriteAegisLog("info", std::string("config reloaded, file_log=") + ResolveLogPath(sAcAegisConfig->Get().fileLogPath));
}

bool AcAegisMgr::GetPlayerDebugSnapshot(uint32 guidLow, AegisPlayerDebugSnapshot& outSnapshot) const
{
    outSnapshot = AegisPlayerDebugSnapshot{};

    bool found = false;
    auto it = _players.find(guidLow);
    if (it != _players.end())
    {
        AegisPlayerContext const& ctx = it->second;
        outSnapshot.online = ctx.online;
        outSnapshot.riskScore = ctx.riskScore;
        outSnapshot.sampleCount = static_cast<uint32>(ctx.samples.Size());
        outSnapshot.offenseCount = ctx.punish.offenseCount;
        outSnapshot.offenseTier = ctx.punish.offenseTier;
        outSnapshot.punishStage = ctx.punish.punishStage;
        outSnapshot.lastCheatType = ctx.punish.lastCheatType;
        outSnapshot.debuffUntilEpoch = ctx.punish.debuffUntilEpoch;
        outSnapshot.jailUntilEpoch = ctx.punish.jailUntilEpoch;
        outSnapshot.banUntilEpoch = ctx.punish.banUntilEpoch;
        outSnapshot.lastOffenseEpoch = ctx.punish.lastOffenseEpoch;
        outSnapshot.permanentBan = ctx.punish.permanentBan;
        outSnapshot.lastEvidenceTag = ctx.lastEvidenceTag;
        outSnapshot.lastBanMode = ctx.punish.lastBanMode;
        outSnapshot.lastBanResult = ctx.punish.lastBanResult;
        outSnapshot.lastBanReason = ctx.punish.lastReason;
        found = true;
    }

    AegisOffenseRecord record;
    if (_persistence.LoadOffenseByGuid(guidLow, record))
    {
        outSnapshot.offenseCount = record.offenseCount;
        outSnapshot.offenseTier = record.offenseTier;
        outSnapshot.punishStage = static_cast<AegisPunishStage>(record.punishStage);
        outSnapshot.lastCheatType = record.lastCheatType;
        outSnapshot.debuffUntilEpoch = record.debuffUntilEpoch;
        outSnapshot.jailUntilEpoch = record.jailUntilEpoch;
        outSnapshot.banUntilEpoch = record.banUntilEpoch;
        outSnapshot.lastOffenseEpoch = record.lastOffenseEpoch;
        outSnapshot.permanentBan = record.permanentBan;
        outSnapshot.lastBanMode = record.lastBanMode;
        outSnapshot.lastBanResult = record.lastBanResult;
        outSnapshot.lastBanReason = record.lastReason;
        found = true;
    }

    return found;
}

void AcAegisMgr::ClearPlayerOffense(Player* player)
{
    if (!player)
        return;

    uint32 guidLow = player->GetGUID().GetCounter();
    AegisPlayerContext& ctx = GetOrCreate(player);
    AegisPunishState previousPunish = ctx.punish;

    int64 nowEpoch = static_cast<int64>(std::time(nullptr));
    bool wasJailed = ctx.punish.jailUntilEpoch > nowEpoch;
    bool hadBanState = ctx.punish.permanentBan || ctx.punish.banUntilEpoch > 0;

    ResetMovementLifecycleState(ctx);
    ctx.riskScore = 0.0f;
    ctx.lastDecayMs = _elapsedMs;
    ctx.punish = AegisPunishState{};

    ClearDebuffs(player);
    if (wasJailed)
    {
        SetHomebind(player, sAcAegisConfig->Get().releaseMapId, sAcAegisConfig->Get().releaseX,
            sAcAegisConfig->Get().releaseY, sAcAegisConfig->Get().releaseZ);
        if (!IsNearPosition(player, sAcAegisConfig->Get().releaseMapId, sAcAegisConfig->Get().releaseX,
            sAcAegisConfig->Get().releaseY, sAcAegisConfig->Get().releaseZ, 5.0f))
        {
            player->TeleportTo(sAcAegisConfig->Get().releaseMapId, sAcAegisConfig->Get().releaseX,
                sAcAegisConfig->Get().releaseY, sAcAegisConfig->Get().releaseZ, sAcAegisConfig->Get().releaseO);
            ctx.lastTeleportMs = _elapsedMs;
        }
    }

    if (hadBanState)
        ClearCoreBanState(guidLow, player->GetSession() ? player->GetSession()->GetAccountId() : 0, previousPunish.lastBanMode, player);

    _persistence.DeleteOffense(guidLow);
    WriteAuditLog("gm_clear", player, &ctx, "\"reason\":\"manual-clear\"");
}

void AcAegisMgr::ClearPlayerOffense(uint32 guidLow)
{
    if (Player* player = ObjectAccessor::FindPlayerByLowGUID(guidLow))
    {
        ClearPlayerOffense(player);
        return;
    }

    AegisOffenseRecord record;
    if (_persistence.LoadOffenseByGuid(guidLow, record) && (record.permanentBan || record.banUntilEpoch > 0))
        ClearCoreBanState(guidLow, record.accountId, record.lastBanMode, nullptr);

    _players.erase(guidLow);
    _persistence.DeleteOffense(guidLow);
}

void AcAegisMgr::DeletePlayerData(uint32 guidLow)
{
    if (Player* player = ObjectAccessor::FindPlayerByLowGUID(guidLow))
        ClearPlayerOffense(player);
    else
    {
        AegisOffenseRecord record;
        if (_persistence.LoadOffenseByGuid(guidLow, record) && (record.permanentBan || record.banUntilEpoch > 0))
            ClearCoreBanState(guidLow, record.accountId, record.lastBanMode, nullptr);
    }

    _players.erase(guidLow);
    _persistence.DeletePlayerData(guidLow);
}

void AcAegisMgr::PurgeAllData()
{
    AegisConfig const& cfg = sAcAegisConfig->Get();
    int64 nowEpoch = static_cast<int64>(std::time(nullptr));

    for (auto& entry : _players)
    {
        if (!entry.second.online)
            continue;

        if (Player* player = ObjectAccessor::FindPlayerByLowGUID(entry.first))
        {
            ClearDebuffs(player);
            if (entry.second.punish.jailUntilEpoch > nowEpoch)
            {
                SetHomebind(player, cfg.releaseMapId, cfg.releaseX, cfg.releaseY, cfg.releaseZ);
                if (!IsNearPosition(player, cfg.releaseMapId, cfg.releaseX, cfg.releaseY, cfg.releaseZ, 5.0f))
                    player->TeleportTo(cfg.releaseMapId, cfg.releaseX, cfg.releaseY, cfg.releaseZ, cfg.releaseO);
            }
        }
    }

    for (AegisOffenseRecord const& record : _persistence.LoadAllOffenses())
    {
        if (record.permanentBan || record.banUntilEpoch > 0)
            ClearCoreBanState(record.guid, record.accountId, record.lastBanMode, nullptr);
    }

    _players.clear();
    _persistence.PurgeAllData();
}

void AcAegisMgr::OnLogin(Player* player)
{
    if (!player)
        return;

    Touch(player);
    AegisPlayerContext& ctx = GetOrCreate(player);
    ctx.online = true;
    ResetMovementLifecycleState(ctx);
    ctx.punish = AegisPunishState{};

    AegisOffenseRecord record;
    if (_persistence.LoadOffense(player, record))
    {
        ctx.punish.loaded = true;
        ctx.punish.offenseCount = record.offenseCount;
        ctx.punish.offenseTier = record.offenseTier;
        ctx.punish.punishStage = static_cast<AegisPunishStage>(record.punishStage);
        ctx.punish.lastCheatType = record.lastCheatType;
        ctx.punish.lastOffenseEpoch = record.lastOffenseEpoch;
        ctx.punish.debuffUntilEpoch = record.debuffUntilEpoch;
        ctx.punish.jailUntilEpoch = record.jailUntilEpoch;
        ctx.punish.banUntilEpoch = record.banUntilEpoch;
        ctx.punish.permanentBan = record.permanentBan;
        ctx.punish.lastReason = record.lastReason;
        ctx.punish.lastBanMode = record.lastBanMode;
        ctx.punish.lastBanResult = record.lastBanResult;
    }

    int64 nowEpoch = static_cast<int64>(std::time(nullptr));
    bool changed = false;
    if (ctx.punish.debuffUntilEpoch > 0 && nowEpoch >= ctx.punish.debuffUntilEpoch)
    {
        ctx.punish.debuffUntilEpoch = 0;
        changed = true;
    }
    if (ctx.punish.jailUntilEpoch > 0 && nowEpoch >= ctx.punish.jailUntilEpoch)
    {
        ctx.punish.jailUntilEpoch = 0;
        changed = true;
    }
    if (!ctx.punish.permanentBan && ctx.punish.banUntilEpoch > 0 && nowEpoch >= ctx.punish.banUntilEpoch)
    {
        ClearCoreBanState(player->GetGUID().GetCounter(), player->GetSession() ? player->GetSession()->GetAccountId() : 0, ctx.punish.lastBanMode, player);
        ctx.punish.banUntilEpoch = 0;
        changed = true;
    }

    if (ctx.punish.debuffUntilEpoch > nowEpoch)
        ApplyDebuff(player);
    else
        ClearDebuffs(player);

    if (ctx.punish.jailUntilEpoch > nowEpoch)
    {
        SetHomebind(player, sAcAegisConfig->Get().jailMapId, sAcAegisConfig->Get().jailX, sAcAegisConfig->Get().jailY, sAcAegisConfig->Get().jailZ);
        if (!IsNearPosition(player, sAcAegisConfig->Get().jailMapId, sAcAegisConfig->Get().jailX, sAcAegisConfig->Get().jailY, sAcAegisConfig->Get().jailZ, 15.0f))
            Jail(player, ctx);
        else
            EnforceJail(player, ctx, true);
    }

    if (ctx.punish.lastOffenseEpoch > 0 && sAcAegisConfig->Get().offenseDecayDays > 0)
    {
        int64 decayWindow = static_cast<int64>(sAcAegisConfig->Get().offenseDecayDays) * DAY;
        if (!ctx.punish.permanentBan && nowEpoch > (ctx.punish.lastOffenseEpoch + decayWindow))
        {
            if (sAcAegisConfig->Get().offenseDecayMode == "hard-reset")
            {
                ctx.punish.offenseCount = 0;
                ctx.punish.offenseTier = 0;
                ctx.punish.punishStage = AegisPunishStage::Observe;
            }
            else
            {
                if (ctx.punish.offenseCount > 0)
                    --ctx.punish.offenseCount;
                if (ctx.punish.offenseTier > 0)
                    --ctx.punish.offenseTier;
                ctx.punish.punishStage = StageFromTier(ctx.punish.offenseTier, sAcAegisConfig->Get());
            }
            changed = true;
        }
    }

    if (changed)
    {
        if (!ctx.punish.permanentBan && ctx.punish.debuffUntilEpoch == 0 && ctx.punish.jailUntilEpoch == 0 && ctx.punish.banUntilEpoch == 0)
            ctx.punish.punishStage = StageFromTier(ctx.punish.offenseTier, sAcAegisConfig->Get());
        SavePunishState(player, ctx);
    }

    std::ostringstream audit;
    audit << "\"loadedFromDb\":" << (ctx.punish.loaded ? "true" : "false")
          << ",\"stateChanged\":" << (changed ? "true" : "false")
          << ",\"lastCheatType\":"
          << static_cast<uint32>(ctx.punish.lastCheatType)
          << ",\"lastReason\":\"" << EscapeJson(ctx.punish.lastReason) << "\""
          << ",\"lastBanMode\":\"" << EscapeJson(ctx.punish.lastBanMode) << "\""
          << ",\"lastBanResult\":\"" << EscapeJson(ctx.punish.lastBanResult) << "\"";
    WriteAuditLog("login_restore", player, &ctx, audit.str());
}

void AcAegisMgr::OnLogout(Player* player)
{
    if (!player)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    ctx.online = false;
    ctx.lastSeenMs = _elapsedMs;
    ResetMovementLifecycleState(ctx);

    WriteAuditLog("logout", player, &ctx, "\"reason\":\"session-end\"");
}

void AcAegisMgr::MaintainPlayerPunishments()
{
    int64 nowEpoch = static_cast<int64>(std::time(nullptr));
    for (auto& entry : _players)
    {
        if (!entry.second.online)
            continue;

        Player* player = ObjectAccessor::FindPlayerByLowGUID(entry.first);
        if (!player)
            continue;

        bool changed = false;
        if (entry.second.punish.debuffUntilEpoch > 0 && nowEpoch >= entry.second.punish.debuffUntilEpoch)
        {
            entry.second.punish.debuffUntilEpoch = 0;
            ClearDebuffs(player);
            if (entry.second.punish.jailUntilEpoch == 0 && entry.second.punish.banUntilEpoch == 0 && !entry.second.punish.permanentBan)
                entry.second.punish.punishStage = AegisPunishStage::Observe;
            if (player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage(sAcAegisConfig->Get().debuffExpireMessage);
            WriteAuditLog("debuff_expired", player, &entry.second,
                "\"reason\":\"timer-expired\"");
            changed = true;
        }

        if (entry.second.punish.jailUntilEpoch > 0 && nowEpoch >= entry.second.punish.jailUntilEpoch)
        {
            if (CanSafelyTeleportForPunish(player))
            {
                entry.second.punish.jailUntilEpoch = 0;
                WriteAuditLog("jail_expired", player, &entry.second,
                    "\"reason\":\"timer-expired\"");
                Release(player, entry.second);
                entry.second.punish.punishStage = AegisPunishStage::Observe;
                if (player->GetSession())
                    ChatHandler(player->GetSession()).SendSysMessage(sAcAegisConfig->Get().jailExpireMessage);
                changed = true;
            }
            else
                WriteAuditLog("jail_release_deferred", player, &entry.second,
                    "\"reason\":\"unsafe-teleport-state\"");
        }
        else if (entry.second.punish.jailUntilEpoch > nowEpoch)
        {
            if (entry.second.lastJailReturnCheckMs == 0 || (_elapsedMs - entry.second.lastJailReturnCheckMs) >= sAcAegisConfig->Get().jailReturnCheckMs)
            {
                entry.second.lastJailReturnCheckMs = _elapsedMs;
                EnforceJail(player, entry.second, false);
            }
        }

        if ((entry.second.punish.debuffUntilEpoch > nowEpoch || entry.second.punish.jailUntilEpoch > nowEpoch) &&
            player->GetSession() &&
            (entry.second.lastPunishNotifyMs == 0 || (_elapsedMs - entry.second.lastPunishNotifyMs) >= sAcAegisConfig->Get().punishNotifyIntervalMs))
        {
            entry.second.lastPunishNotifyMs = _elapsedMs;
            if (entry.second.punish.jailUntilEpoch > nowEpoch)
                ChatHandler(player->GetSession()).SendSysMessage(BuildPunishPlayerMessage(sAcAegisConfig->Get().jailRemainMessage, FormatDurationShort(entry.second.punish.jailUntilEpoch - nowEpoch)));
            else if (entry.second.punish.debuffUntilEpoch > nowEpoch)
                ChatHandler(player->GetSession()).SendSysMessage(BuildPunishPlayerMessage(sAcAegisConfig->Get().debuffRemainMessage, FormatDurationShort(entry.second.punish.debuffUntilEpoch - nowEpoch)));
        }

        if (!entry.second.punish.permanentBan && entry.second.punish.banUntilEpoch > 0 && nowEpoch >= entry.second.punish.banUntilEpoch)
        {
            ClearCoreBanState(entry.first, player->GetSession() ? player->GetSession()->GetAccountId() : 0, entry.second.punish.lastBanMode, player);
            entry.second.punish.banUntilEpoch = 0;
            WriteAuditLog("ban_expired", player, &entry.second,
                "\"reason\":\"timer-expired\"");
            changed = true;
        }

        if (changed)
        {
            entry.second.punish.punishStage = StageFromTier(entry.second.punish.offenseTier, sAcAegisConfig->Get());
            SavePunishState(player, entry.second);
        }
    }
}

void AcAegisMgr::MaintainOfflinePunishments()
{
    int64 nowEpoch = static_cast<int64>(std::time(nullptr));
    std::vector<AegisOffenseRecord> records = _persistence.LoadExpiredTempBans(nowEpoch);
    if (records.empty())
        return;

    for (AegisOffenseRecord& record : records)
    {
        auto it = _players.find(record.guid);
        if (it != _players.end() && it->second.online)
            continue;

        ClearCoreBanState(record.guid, record.accountId, record.lastBanMode, nullptr);
        record.banUntilEpoch = 0;
        if (!record.permanentBan && record.debuffUntilEpoch == 0 && record.jailUntilEpoch == 0)
            record.punishStage = static_cast<uint8>(StageFromTier(record.offenseTier, sAcAegisConfig->Get()));
        record.lastBanResult = "BAN_EXPIRED";
        _persistence.SaveOffense(record);
    }
}

void AcAegisMgr::CleanupOfflineContexts()
{
    uint32 ttlMs = 300000;
    for (auto it = _players.begin(); it != _players.end();)
    {
        AegisPlayerContext& ctx = it->second;
        if (!ctx.online)
            DecayRisk(ctx, _elapsedMs);

        bool activePunish = ctx.punish.permanentBan || ctx.punish.debuffUntilEpoch > 0 || ctx.punish.jailUntilEpoch > 0 || ctx.punish.banUntilEpoch > 0;
        bool stale = !ctx.online && _elapsedMs >= ctx.lastSeenMs && (_elapsedMs - ctx.lastSeenMs) > ttlMs;
        if (stale && ctx.riskScore <= 0.0f && !activePunish)
            it = _players.erase(it);
        else
            ++it;
    }
}

void AcAegisMgr::EmitSummary() const
{
    uint32 onlineCount = 0;
    uint32 riskyCount = 0;
    for (auto const& entry : _players)
    {
        if (entry.second.online)
            ++onlineCount;
        if (entry.second.riskScore > 0.0f)
            ++riskyCount;
    }

    LOG_INFO("module", "[{}] ACA: onlineContexts={}, riskyContexts={}", ToIsoTimestamp(), onlineCount, riskyCount);
    WriteAegisLog("summary", "onlineContexts=" + std::to_string(onlineCount) + ", riskyContexts=" + std::to_string(riskyCount));
}

void AcAegisMgr::OnWorldUpdate(uint32 diff)
{
    _elapsedMs += diff;
    MaintainPlayerPunishments();

    _offlinePunishSweepElapsedMs += diff;
    if (_offlinePunishSweepElapsedMs >= 5000)
    {
        _offlinePunishSweepElapsedMs = 0;
        MaintainOfflinePunishments();
    }

    CleanupOfflineContexts();

    if (!sAcAegisConfig->Get().logEnabled || !sAcAegisConfig->Get().summaryLogEnabled)
    {
        _summaryElapsedMs = 0;
        return;
    }

    _summaryElapsedMs += diff;
    if (_summaryElapsedMs >= sAcAegisConfig->Get().summaryLogIntervalMs)
    {
        _summaryElapsedMs = 0;
        EmitSummary();
    }
}

void AcAegisMgr::OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/)
{
    if (!player || !spell)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    SpellInfo const* spellInfo = spell->GetSpellInfo();
    if (!spellInfo)
        return;

    if (ContainsId(sAcAegisConfig->Get().spellWhitelist, spellInfo->Id))
        ctx.lastSpellGraceMs = _elapsedMs;

    if (HasExternalMobilityAura(player))
        ctx.lastSpellGraceMs = _elapsedMs;

    if (HasControlledTeleportEffect(spellInfo))
    {
        ctx.lastControlledTeleportMs = _elapsedMs;
        if (spell->m_targets.HasDst())
        {
            WorldLocation const* dst = spell->m_targets.GetDstPos();
            float dist2d = Dist3DToPoint(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                dst->GetPositionX(), dst->GetPositionY(), player->GetPositionZ());
            ArmControlledMoveExpectation(ctx, AegisControlledMoveKind::Teleport,
                _elapsedMs, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                std::clamp(dist2d * 0.2f, 1.5f, 8.0f), 0.0f);
        }
    }

    if (HasControlledChargeEffect(spellInfo))
    {
        ctx.lastControlledChargeMs = _elapsedMs;
        float minDistance2D = 1.5f;
        if (Unit* target = spell->m_targets.GetUnitTarget())
            minDistance2D = std::clamp(player->GetDistance2d(target), 1.5f, 8.0f) * 0.25f;
        else if (spell->m_targets.HasDst())
        {
            WorldLocation const* dst = spell->m_targets.GetDstPos();
            minDistance2D = std::clamp(Dist3DToPoint(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                dst->GetPositionX(), dst->GetPositionY(), player->GetPositionZ()), 1.5f, 8.0f) * 0.25f;
        }

        ArmControlledMoveExpectation(ctx, AegisControlledMoveKind::Charge,
            _elapsedMs, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            std::max(1.0f, minDistance2D), 0.0f);
    }

    if (HasControlledJumpEffect(spellInfo))
    {
        ctx.lastControlledJumpMs = _elapsedMs;
        float minDistance2D = 0.75f;
        float minDeltaZ = 0.35f;
        if (spell->m_targets.HasDst())
        {
            WorldLocation const* dst = spell->m_targets.GetDstPos();
            minDistance2D = std::clamp(Dist3DToPoint(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
                dst->GetPositionX(), dst->GetPositionY(), player->GetPositionZ()), 0.75f, 6.0f) * 0.2f;
            minDeltaZ = std::clamp(std::fabs(dst->GetPositionZ() - player->GetPositionZ()) * 0.25f, 0.25f, 4.0f);
        }

        ArmControlledMoveExpectation(ctx, AegisControlledMoveKind::Jump,
            _elapsedMs, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
            std::max(0.75f, minDistance2D), minDeltaZ);
    }

    if (HasControlledPullEffect(spellInfo))
    {
        ctx.lastControlledPullMs = _elapsedMs;
        ctx.lastSpellGraceMs = _elapsedMs;
    }

    if (std::find(sAcAegisConfig->Get().afkIgnoreSpellIds.begin(), sAcAegisConfig->Get().afkIgnoreSpellIds.end(), spellInfo->Id) != sAcAegisConfig->Get().afkIgnoreSpellIds.end())
        ctx.gather.lastWhitelistActionMs = _elapsedMs;
}

bool AcAegisMgr::OnBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z)
{
    if (!player)
        return true;

    AegisPlayerContext& ctx = GetOrCreate(player);
    int64 nowEpoch = static_cast<int64>(std::time(nullptr));
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (cfg.jailEnabled && ctx.punish.jailUntilEpoch > nowEpoch)
    {
        bool allowed = mapId == cfg.jailMapId && Dist3DToPoint(x, y, z, cfg.jailX, cfg.jailY, cfg.jailZ) <= 25.0f;
        if (!allowed)
        {
            std::ostringstream audit;
            audit << "\"reason\":\"jailed-teleport-block\""
                  << ",\"targetMap\":" << mapId
                  << ",\"targetX\":" << x
                  << ",\"targetY\":" << y
                  << ",\"targetZ\":" << z;
            WriteAuditLog("teleport_blocked", player, &ctx, audit.str());

            if (player->GetSession())
                ChatHandler(player->GetSession()).SendSysMessage("AcAegis: teleport blocked while jailed.");
            ctx.lastJailReturnCheckMs = 0;
            return false;
        }
    }

    ctx.lastTeleportMs = _elapsedMs;
    ArmPendingTeleportExpectation(ctx, _elapsedMs, mapId, x, y, z, cfg);
    ResetTeleportDetectionWindows(ctx);
    ClearControlledMoveExpectation(ctx);
    return true;
}

void AcAegisMgr::OnMapChanged(Player* player)
{
    if (!player)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    ResetMovementLifecycleState(ctx);
    ctx.lastMapChangeMs = _elapsedMs;
    ResetGatherWindow(player, ctx, "map-changed");
}

void AcAegisMgr::OnLoot(Player* player)
{
    if (!player)
        return;

    TouchGatherWindow(player, GetOrCreate(player), "loot");
}

void AcAegisMgr::OnGatherAction(Player* player)
{
    if (!player)
        return;

    TouchGatherWindow(player, GetOrCreate(player), "gather");
}

void AcAegisMgr::OnCanFlyByServer(Player* player, bool apply)
{
    if (!player)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    bool changed = ctx.serverCanFly != apply;
    ctx.serverCanFly = apply;
    ctx.lastCanFlyServerMs = _elapsedMs;
    ctx.lastAuthorizedAerialMs = _elapsedMs;

    if (changed)
        ResetMovementDetectionState(ctx);
}

void AcAegisMgr::OnUnderAckMount(Player* player)
{
    if (!player)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    ctx.lastAckMountMs = _elapsedMs;

    if (player->IsMounted() || player->HasIncreaseMountedSpeedAura() ||
        player->HasIncreaseMountedFlightSpeedAura() ||
        player->HasWaterWalkAura() || player->HasHoverAura() ||
        IsServerAuthorizedAerialState(player) || ctx.serverCanFly)
        ctx.lastAuthorizedAerialMs = _elapsedMs;

    if (player->HasWaterWalkAura() || player->HasHoverAura() ||
        IsServerAuthorizedAerialState(player))
        ctx.lastSpellGraceMs = _elapsedMs;

    if (player->GetTransport())
        ctx.lastTransportMs = _elapsedMs;
    if (player->GetVehicle())
        ctx.lastVehicleMs = _elapsedMs;

    ResetMovementDetectionState(ctx);
}

void AcAegisMgr::OnVehicleTransition(Player* player)
{
    if (!player)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    ctx.lastVehicleMs = _elapsedMs;
    ResetMovementDetectionState(ctx);
    ResetGatherWindow(player, ctx, "vehicle-transition");
}

void AcAegisMgr::OnRootAckUpd(Player* player)
{
    if (!player)
        return;

    GetOrCreate(player).lastRootAckMs = _elapsedMs;
}

void AcAegisMgr::OnJumpOpcode(Player* player, bool jump)
{
    if (!player || !jump)
        return;

    GetOrCreate(player).lastJumpOpcodeMs = _elapsedMs;
}

void AcAegisMgr::OnMovementInfoUpdate(Player* player, MovementInfo const& movementInfo)
{
    if (!player)
        return;

    AegisPlayerContext& ctx = GetOrCreate(player);
    SyncMovementBoundaryState(player, movementInfo, ctx, _elapsedMs);
}

void AcAegisMgr::OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode)
{
    if (!IsEnabledFor(player))
        return;

    Touch(player);
    AegisPlayerContext& ctx = GetOrCreate(player);
    AegisConfig const& cfg = sAcAegisConfig->Get();
    if (!ctx.samples.Empty())
    {
        AegisMoveSample const& newest = ctx.samples.Newest();
        if (_elapsedMs > newest.serverMs &&
            (_elapsedMs - newest.serverMs) > MovementSampleContinuityResetMs(cfg))
        {
            ResetMovementDetectionState(ctx);
        }
    }

    SyncMovementBoundaryState(player, movementInfo, ctx, _elapsedMs);

    CaptureSample(player, movementInfo, opcode, ctx);
    ConsumePendingTeleportArrival(ctx, ctx.samples.Newest(), _elapsedMs,
        cfg);

    if (ctx.gather.windowStartMs != 0)
    {
        float moved = Dist3DToPoint(movementInfo.pos.GetPositionX(), movementInfo.pos.GetPositionY(), movementInfo.pos.GetPositionZ(), ctx.gather.startX, ctx.gather.startY, ctx.gather.startZ);
        if (moved > (sAcAegisConfig->Get().afkMaxMoveDistance * 2.0f))
            ResetGatherWindow(player, ctx, "movement-reset");
    }

    if (ctx.samples.Size() < 2)
        return;

    DecayRisk(ctx, _elapsedMs);

    AegisMovementContext movementCtx = BuildMovementContext(player, ctx, _elapsedMs);

    if (std::optional<AegisEvidenceEvent> evidence = DetectForceMove(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectControlledMove(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectTransportRelativeSpeed(player, ctx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    AegisMoveSample const& cur = ctx.samples.Newest();
    if (cfg.forceMoveEnabled && cur.opcode == CMSG_MOVE_KNOCK_BACK_ACK &&
        (cur.jumpXySpeed >= cfg.forceMoveMinAckSpeedXY || std::fabs(cur.jumpZSpeed) >= cfg.forceMoveMinAckSpeedZ))
    {
        ctx.lastKnockBackAckMs = _elapsedMs;
        movementCtx = BuildMovementContext(player, ctx, _elapsedMs);
    }

    if (ShouldSkipAllMovementDetectors(movementCtx))
        return;

    if (std::optional<AegisEvidenceEvent> evidence = DetectTeleport(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectNoClip(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectWaterWalk(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectMount(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectFly(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectClimb(player, ctx, movementCtx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectTime(player, ctx))
    {
        HandleEvidence(player, ctx, *evidence);
        return;
    }

    if (std::optional<AegisEvidenceEvent> evidence = DetectSpeed(player, ctx, movementCtx))
        HandleEvidence(player, ctx, *evidence);
}

bool AcAegisMgr::CheckMovement(Player* player, MovementInfo const& movementInfo, Unit* mover, bool /*jump*/)
{
    if (!player || !mover || player != mover || !IsEnabledFor(player))
        return true;

    Touch(player);
    AegisPlayerContext& ctx = GetOrCreate(player);
    DecayRisk(ctx, _elapsedMs);
    AegisMovementContext movementCtx = BuildMovementContext(player, ctx, _elapsedMs);

    if (std::optional<AegisEvidenceEvent> evidence = DetectRootBreak(player, movementInfo, ctx, movementCtx))
        HandleEvidence(player, ctx, *evidence);

    return true;
}

bool AcAegisMgr::HandleDoubleJump(Player* player, Unit* mover)
{
    if (!player || !mover || player != mover || !IsEnabledFor(player))
        return true;

    AegisPlayerContext& ctx = GetOrCreate(player);
    AegisConfig const& cfg = sAcAegisConfig->Get();
    uint32 nowMs = _elapsedMs;
    AegisMovementContext movementCtx = BuildMovementContext(player, ctx, nowMs);
    auto resetDoubleJumpWindow = [&ctx]()
    {
        ctx.doubleJumpWindowStartMs = 0;
        ctx.doubleJumpHits = 0;
    };

    if (ShouldSkipAerialMovementDetectors(movementCtx))
    {
        resetDoubleJumpWindow();
        ctx.lastJumpOpcodeMs = nowMs;
        return true;
    }

    if (movementCtx.hasAuthorizedAerialState || player->IsInWater() || player->IsUnderWater())
    {
        resetDoubleJumpWindow();
        ctx.lastJumpOpcodeMs = nowMs;
        return true;
    }

    float groundZ = 0.0f;
    if (!_geometry.GetGroundHeight(player, player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(), groundZ))
    {
        resetDoubleJumpWindow();
        ctx.lastJumpOpcodeMs = nowMs;
        return true;
    }

    float heightOverGround = std::max(0.0f, player->GetPositionZ() - groundZ);
    float hitX = 0.0f;
    float hitY = 0.0f;
    float hitZ = 0.0f;
    bool platformBetweenPlayerAndGround =
        heightOverGround >= cfg.doubleJumpMinHeight &&
        _geometry.RaycastStaticAndDynamic(player,
            player->GetPositionX(), player->GetPositionY(),
            player->GetPositionZ() + 0.2f,
            player->GetPositionX(), player->GetPositionY(),
            groundZ + 0.2f,
            hitX, hitY, hitZ);
    uint32 prevJumpMs = ctx.lastJumpOpcodeMs;
    uint32 rapidRepeatWindow = std::min(cfg.doubleJumpMaxRepeatMs,
        cfg.doubleJumpWindowMs);
    bool rapidRepeat = prevJumpMs && nowMs > prevJumpMs && (nowMs - prevJumpMs) <= rapidRepeatWindow;
    float moved2d = 0.0f;
    float riseZ = 0.0f;
    if (ctx.samples.Size() >= 2)
    {
        AegisMoveSample const& cur = ctx.samples.Newest();
        AegisMoveSample const& prev = ctx.samples.Previous();
        if (cur.mapId == prev.mapId)
        {
            moved2d = Dist2D(cur, prev);
            riseZ = cur.z - prev.z;
        }
    }
    bool airborne = player->IsFalling() &&
        heightOverGround >= std::max(cfg.doubleJumpMinHeight,
            cfg.climbMinRise * 1.5f);

    ctx.lastJumpOpcodeMs = nowMs;
    if (!rapidRepeat || !airborne || platformBetweenPlayerAndGround ||
        moved2d > 1.5f || riseZ <= 0.1f)
    {
        if (!rapidRepeat || platformBetweenPlayerAndGround || moved2d > 1.5f)
            resetDoubleJumpWindow();
        return true;
    }

    if (ctx.doubleJumpWindowStartMs == 0 ||
        (nowMs - ctx.doubleJumpWindowStartMs) > cfg.doubleJumpWindowMs)
    {
        ctx.doubleJumpWindowStartMs = nowMs;
        ctx.doubleJumpHits = 0;
    }

    ++ctx.doubleJumpHits;
    if (ctx.doubleJumpHits < std::max<uint32>(3, cfg.doubleJumpRepeatHits))
        return true;

    AegisEvidenceEvent evidence;
    evidence.cheatType = AegisCheatType::Climb;
    evidence.level = (ctx.doubleJumpHits > cfg.doubleJumpRepeatHits ||
        heightOverGround >= cfg.superJumpMinHeight) ?
        AegisEvidenceLevel::Strong : AegisEvidenceLevel::Medium;
    evidence.tag = "DoubleJump";
    evidence.detail = "height=" + std::to_string(heightOverGround) +
        ",hits=" + std::to_string(ctx.doubleJumpHits);
    evidence.riskDelta = ClampRiskDelta(16.0f + std::min(10.0f, heightOverGround));
    evidence.serverMs = nowMs;
    evidence.shouldRollback = evidence.level == AegisEvidenceLevel::Strong;
    evidence.metricA = heightOverGround;
    evidence.metricB = static_cast<float>(nowMs - prevJumpMs);
    HandleEvidence(player, ctx, evidence);
    resetDoubleJumpWindow();
    return true;
}