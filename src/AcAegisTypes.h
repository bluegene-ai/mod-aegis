#ifndef MOD_AC_AEGIS_TYPES_H
#define MOD_AC_AEGIS_TYPES_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Define.h"

enum class AegisCheatType : uint8
{
    Unknown = 0,
    Speed = 1,
    Teleport = 2,
    NoClip = 3,
    Fly = 4,
    Climb = 5,
    Afk = 6,
    Mount = 7,
    Time = 8,
    Control = 9
};

enum class AegisEvidenceLevel : uint8
{
    Info = 0,
    Weak = 1,
    Medium = 2,
    Strong = 3
};

enum class AegisActionType : uint8
{
    None = 0,
    Notify = 1,
    Rollback = 2,
    Debuff = 3,
    Jail = 4,
    Kick = 5,
    BanCharacter = 6,
    BanAccountByCharacter = 7,
    BanAccount = 8
};

enum class AegisPunishStage : uint8
{
    Observe = 0,
    Debuff = 1,
    Jail = 2,
    Kick = 3,
    TempBan = 4,
    PermBan = 5
};

enum class AegisControlledMoveKind : uint8
{
    None = 0,
    Charge = 1,
    Jump = 2,
    Pull = 3,
    Teleport = 4
};

template <class T>
class AegisRingBuffer
{
public:
    void Init(size_t capacity)
    {
        capacity = std::max<size_t>(2, capacity);
        _data.assign(capacity, T{});
        _head = 0;
        _size = 0;
    }

    void Push(T const& value)
    {
        if (_data.empty())
            Init(32);

        _data[_head] = value;
        _head = (_head + 1) % _data.size();
        if (_size < _data.size())
            ++_size;
    }

    void Clear()
    {
        _head = 0;
        _size = 0;
    }

    bool Empty() const { return _size == 0; }
    size_t Size() const { return _size; }

    T const& Newest() const
    {
        size_t idx = (_head + _data.size() - 1) % _data.size();
        return _data[idx];
    }

    T const& Previous() const
    {
        size_t idx = (_head + _data.size() - 2) % _data.size();
        return _data[idx];
    }

    T const& GetFromNewest(size_t offset) const
    {
        size_t idx = (_head + _data.size() - 1 - (offset % _data.size())) % _data.size();
        return _data[idx];
    }

private:
    std::vector<T> _data;
    size_t _head = 0;
    size_t _size = 0;
};

struct AegisMoveSample
{
    uint32 serverMs = 0;
    uint32 clientMs = 0;
    uint32 opcode = 0;
    uint32 moveFlags = 0;
    uint16 moveFlags2 = 0;
    uint32 mapId = 0;
    uint32 zoneId = 0;
    uint32 areaId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
    float jumpXySpeed = 0.0f;
    float jumpZSpeed = 0.0f;
    float allowedSpeed = 0.0f;
    uint32 latencyMs = 0;
    uint32 transportGuidCounter = 0;
    int8 transportSeat = -1;
    float transportX = 0.0f;
    float transportY = 0.0f;
    float transportZ = 0.0f;
};

struct AegisSafePosition
{
    bool valid = false;
    uint32 mapId = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float o = 0.0f;
};

struct AegisGatherState
{
    uint32 windowStartMs = 0;
    uint32 lastActionMs = 0;
    uint32 actionCount = 0;
    uint32 lootCount = 0;
    uint32 gatherCount = 0;
    uint32 lastEvidenceMs = 0;
    uint32 lastWhitelistActionMs = 0;
    uint32 suspiciousWindows = 0;
    float startX = 0.0f;
    float startY = 0.0f;
    float startZ = 0.0f;
    std::string lastSource;
};

struct AegisPunishState
{
    bool loaded = false;
    uint32 offenseCount = 0;
    uint8 offenseTier = 0;
    AegisPunishStage punishStage = AegisPunishStage::Observe;
    int64 debuffUntilEpoch = 0;
    int64 jailUntilEpoch = 0;
    int64 banUntilEpoch = 0;
    int64 lastOffenseEpoch = 0;
    bool permanentBan = false;
    uint8 lastCheatType = 0;
    std::string lastReason;
    std::string lastBanMode;
    std::string lastBanResult;
};

struct AegisPlayerContext
{
    bool online = false;
    uint32 lastSeenMs = 0;

    AegisRingBuffer<AegisMoveSample> samples;
    AegisSafePosition safePosition;

    float riskScore = 0.0f;
    uint32 lastDecayMs = 0;
    uint32 lastNotifyMs = 0;
    uint32 lastEvidenceMs = 0;
    std::string lastEvidenceTag;

    uint32 lastSpellGraceMs = 0;
    uint32 lastTeleportMs = 0;
    uint32 lastAuthorizedAerialMs = 0;
    uint32 pendingTeleportExpireMs = 0;
    uint32 pendingTeleportMapId = 0;
    float pendingTeleportX = 0.0f;
    float pendingTeleportY = 0.0f;
    float pendingTeleportZ = 0.0f;
    uint32 lastMapChangeMs = 0;
    uint32 lastVehicleMs = 0;
    uint32 lastTransportMs = 0;
    uint32 lastTaxiFlightMs = 0;
    uint32 lastFallMs = 0;
    uint32 lastCanFlyServerMs = 0;
    uint32 lastAckMountMs = 0;
    uint32 lastControlledTeleportMs = 0;
    uint32 lastControlledChargeMs = 0;
    uint32 lastControlledJumpMs = 0;
    uint32 lastControlledPullMs = 0;
    uint32 lastKnockBackAckMs = 0;
    uint32 lastRootAckMs = 0;
    uint32 lastJumpOpcodeMs = 0;
    uint32 lastGeometryCheckMs = 0;
    uint32 lastJailReturnCheckMs = 0;
    uint32 lastPunishNotifyMs = 0;
    bool serverCanFly = false;
    bool observedAuthorizedAerialState = false;
    bool observedTaxiFlightState = false;
    bool observedTransportState = false;
    bool observedVehicleState = false;

    AegisControlledMoveKind controlledMoveKind = AegisControlledMoveKind::None;
    uint32 controlledMoveIssuedMs = 0;
    float controlledMoveStartX = 0.0f;
    float controlledMoveStartY = 0.0f;
    float controlledMoveStartZ = 0.0f;
    float controlledMoveMinDistance2D = 0.0f;
    float controlledMoveMinDeltaZ = 0.0f;
    uint32 controlledMoveWindowStartMs = 0;
    uint32 controlledMoveHits = 0;

    uint32 flyWindowStartMs = 0;
    uint32 flySuspicionHits = 0;
    uint32 noClipWindowStartMs = 0;
    uint32 noClipBlockedHits = 0;
    uint32 speedWindowStartMs = 0;
    uint32 speedHits = 0;
    uint32 timeWindowStartMs = 0;
    uint32 timeHits = 0;
    uint32 airStallWindowStartMs = 0;
    uint32 airStallHits = 0;
    uint32 waterWalkWindowStartMs = 0;
    uint32 waterWalkHits = 0;
    uint32 mountWindowStartMs = 0;
    uint32 mountHits = 0;
    uint32 forceMoveWindowStartMs = 0;
    uint32 forceMoveHits = 0;
    uint32 climbWindowStartMs = 0;
    uint32 climbHitsInWindow = 0;
    uint32 superJumpWindowStartMs = 0;
    uint32 superJumpHitsInWindow = 0;
    uint32 doubleJumpWindowStartMs = 0;
    uint32 doubleJumpHits = 0;
    uint32 stationaryMoveWindowStartMs = 0;
    uint32 stationaryMoveHits = 0;
    uint32 teleportBurstWindowStartMs = 0;
    uint32 teleportBurstHits = 0;
    uint32 transportSpeedWindowStartMs = 0;
    uint32 transportSpeedHits = 0;

    AegisGatherState gather;
    AegisPunishState punish;
};

struct AegisEvidenceEvent
{
    AegisCheatType cheatType = AegisCheatType::Unknown;
    AegisEvidenceLevel level = AegisEvidenceLevel::Info;
    std::string tag;
    std::string detail;
    float riskDelta = 0.0f;
    uint32 serverMs = 0;
    bool shouldRollback = false;
    bool geometryConfirmed = false;
    std::string geometryReason;
    float metricA = 0.0f;
    float metricB = 0.0f;
};

struct AegisActionDecision
{
    AegisActionType primaryAction = AegisActionType::None;
    bool shouldNotify = false;
    bool shouldRollback = false;
    bool persistOffense = false;
    uint32 pendingOffenseCount = 0;
    uint8 pendingOffenseTier = 0;
    uint32 debuffSeconds = 0;
    uint32 jailSeconds = 0;
    uint32 banSeconds = 0;
    std::string banMode;
    std::string reason;
};

struct AegisPlayerDebugSnapshot
{
    bool online = false;
    float riskScore = 0.0f;
    uint32 sampleCount = 0;
    uint32 offenseCount = 0;
    uint8 offenseTier = 0;
    AegisPunishStage punishStage = AegisPunishStage::Observe;
    uint8 lastCheatType = 0;
    int64 debuffUntilEpoch = 0;
    int64 jailUntilEpoch = 0;
    int64 banUntilEpoch = 0;
    int64 lastOffenseEpoch = 0;
    bool permanentBan = false;
    std::string lastEvidenceTag;
    std::string lastBanMode;
    std::string lastBanResult;
    std::string lastBanReason;
};

#endif