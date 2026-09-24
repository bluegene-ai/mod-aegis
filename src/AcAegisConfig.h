#ifndef MOD_AC_AEGIS_CONFIG_H
#define MOD_AC_AEGIS_CONFIG_H

#include <atomic>
#include <string>
#include <vector>

#include "Define.h"

struct AegisConfig
{
    bool enabled = true;
    bool enabledOnGmAccounts = false;
    bool autoEnsureSchema = false;

    bool logEnabled = true;
    bool verboseLog = false;
    bool summaryLogEnabled = true;
    uint32 summaryLogIntervalMs = 60000;
    std::string fileLogPath = "./logs/aegis.log";
    std::string gmNotifyFormat = "compact";
    uint32 gmNotifyCooldownMs = 10000;
    uint32 eventBatchSize = 32;
    uint32 eventFlushIntervalMs = 1000;
    uint32 eventQueueLimit = 4096;

    bool panelOutputEnabled = false;
    bool panelWriteDetections = true;
    std::string panelAdmin = "AcAegis";
    uint32 panelServerId = 1;
    std::string panelAccountLogPath;
    std::string panelCharacterLogPath;
    std::string panelMovementReason = "外挂";
    std::string panelAfkReason = "挂机";

    uint32 samplingBufferSize = 32;
    float riskHalfLifeSeconds = 75.0f;
    float riskMaxDeltaPerMove = 30.0f;
    // Ground reference cache: a Map::GetFullTerrainStatusForPosition query is
    // VMAP-heavy, so lookups that stay inside this radius and time window reuse
    // the previous result. Set GroundCacheTtlMs to 0 to disable caching.
    //
    // The radius must exceed the distance a player covers inside the TTL or the
    // cache never hits while moving: at MOVE_RUN 7 yd/s a 250 ms window moves
    // 1.75 yards, so the old 1.5 yard default missed on essentially every packet
    // and the hot path kept paying one full terrain query per movement packet.
    // 4.0 yards also covers mounted ground speed (14 yd/s * 250 ms = 3.5 yards).
    // Flight and transport movement skip the cache entirely (see
    // MaybeUpdateSafePosition), so the larger radius cannot hide a fly hack.
    uint32 groundCacheTtlMs = 250;
    float groundCacheRadius = 4.0f;

    uint32 teleportGraceMs = 2000;
    uint32 teleportArrivalWindowMs = 15000;
    float teleportArrivalRadius = 40.0f;
    uint32 mapChangeGraceMs = 3000;
    uint32 mobilitySpellGraceMs = 1500;
    uint32 vehicleGraceMs = 1500;
    uint32 transportGraceMs = 2000;
    uint32 fallGraceMs = 800;
    std::vector<uint32> spellWhitelist;
    std::vector<uint32> auraWhitelist;

    bool speedEnabled = true;
    float speedTolerancePct = 10.0f;
    float speedFlatMargin = 0.75f;
    uint32 speedMinDtMs = 100;
    uint32 speedMaxDtMs = 1500;
    uint32 speedWindowMs = 1800;
    uint32 speedMediumHits = 2;
    uint32 speedStrongHits = 3;
    float speedStrongRatio = 1.6f;

    bool timeEnabled = true;
    float timeMinClientLeadMs = 180.0f;
    float timeMinRatio = 1.35f;
    float timeMinDistance2D = 1.25f;
    uint32 timeWindowMs = 2500;
    uint32 timeStrongHits = 3;

    bool teleportEnabled = true;
    float teleportMinDistance = 18.0f;
    float teleportSpeedMultiplier = 3.0f;
    float teleportCoordinateMinDistance = 45.0f;
    float teleportCoordinateSpeedMultiplier = 1.75f;
    float teleportAxisStrongDelta = 25.0f;
    float teleportStationaryMinDistance2D = 4.5f;
    float teleportStationaryMinDeltaZ = 3.0f;
    uint32 teleportStationaryWindowMs = 2500;
    uint32 teleportStationaryStrongHits = 3;
    uint32 teleportBurstWindowMs = 1800;
    uint32 teleportBurstStrongHits = 2;

    bool noClipEnabled = true;
    uint32 noClipCheckIntervalMs = 300;
    float noClipMinSegmentDistance = 2.5f;
    float noClipMaxDirectDistance = 35.0f;
    float noClipRayZOffset = 1.0f;
    float noClipMinRemainingDistance = 0.8f;
    float noClipMaxSpeedMultiplier = 1.5f;
    uint32 noClipCumulativeWindowMs = 1800;
    float noClipCumulativeMinDistance = 2.8f;
    uint32 noClipCumulativeStrongHits = 3;
    bool noClipDoorCrossEnabled = true;
    float noClipDoorCrossHalfWidthYards = 2.0f;
    // A door opened this recently is never reported. The window has to stay well above the
    // door's own auto-close time: at 3000 ms it was exactly equal to the 3000 ms auto-close
    // of the Scarlet Monastery wing doors, and the state is read when the segment is judged
    // rather than when it was walked, so a player who opened a door and walked through it
    // could still be judged against an already closed door with no margin left.
    uint32 noClipDoorOpenGraceMs = 8000;
    // Whether walking through a *closed* door may be treated as actionable (Strong) evidence.
    // Off by default: the door's state and the door's collision are two views of the same
    // server-side state (GameObject::SetGoState() flips both), so a client that still shows
    // the door open - a loading screen, a missed state update, or another player having
    // toggled it - produces exactly the same evidence as a client whose collision was
    // removed. With this off the crossing is still recorded as Weak evidence for triage.
    bool noClipDoorCrossActionable = false;

    bool flyEnabled = true;
    float flyMinHeightAboveGround = 6.0f;
    float flySustainMinHorizontalDistance = 3.5f;
    float flyIllegalFlagMinHeightAboveGround = 1.0f;
    float flyIllegalFlagMinHorizontalDistance = 1.0f;
    uint32 flyCanFlyGraceMs = 2500;
    float flyAirStallMaxHorizontalDistance = 0.75f;
    float flyAirStallMaxDeltaZ = 0.15f;
    uint32 flyAirStallWindowMs = 2500;
    uint32 flyAirStallStrongHits = 3;
    float flyWaterWalkMinHorizontalDistance = 1.5f;
    float flyWaterWalkSurfaceTolerance = 0.35f;
    float flyWaterWalkMinWaterDepth = 1.75f;
    uint32 flyWaterWalkWindowMs = 2500;
    uint32 flyWaterWalkStrongHits = 3;

    bool mountEnabled = true;
    uint32 mountGraceMs = 2500;
    uint32 mountIndoorWindowMs = 2500;
    uint32 mountIndoorStrongHits = 2;
    float mountIndoorMinMoveDistance = 0.5f;

    bool forceMoveEnabled = true;
    // Server-issued displacement (knockback / pull / charge / root ack) grace. A knockback
    // trajectory commonly lasts longer than a second, and while it plays out the client
    // keeps reporting real 20+ yd/s movement, so a grace shorter than the flight turns a
    // legitimate knockback into speed evidence (the 2026-09-23 production log has two such
    // isolated SpeedEnvelope spikes on a character the server believed was walking).
    uint32 forceMoveGraceMs = 2000;
    float forceMoveMinAckSpeedXY = 4.0f;
    float forceMoveMinAckSpeedZ = 2.5f;
    float forceMoveExpectedFactor = 0.25f;
    float forceMoveMinDistance2D = 0.45f;
    float forceMoveMinDeltaZ = 0.2f;
    uint32 forceMoveWindowMs = 4000;
    uint32 forceMoveStrongHits = 2;

    bool climbEnabled = true;
    float climbMinRise = 1.87f;
    float climbMinHorizontalDistance = 0.8f;
    float climbMinSlopeRatio = 1.73f;
    uint32 climbWindowMs = 4000;
    uint32 climbStrongHits = 3;
    float superJumpMinHeight = 8.0f;
    float superJumpMinDeltaZ = 4.0f;
    uint32 superJumpWindowMs = 1200;
    float doubleJumpMinHeight = 2.5f;
    uint32 doubleJumpMaxRepeatMs = 350;
    uint32 doubleJumpWindowMs = 1200;
    uint32 doubleJumpRepeatHits = 2;

    bool afkEnabled = true;
    uint32 afkWindowMs = 600000;
    uint32 afkMinActions = 12;
    uint32 afkStrongActions = 20;
    uint32 afkMinActionGapMs = 1500;
    float afkMaxMoveDistance = 6.0f;
    float afkStrongMoveDistance = 3.0f;
    uint32 afkMinLootCount = 4;
    uint32 afkMinGatherCount = 6;
    uint32 afkMinSuspiciousWindows = 2;
    uint32 afkEvidenceCooldownMs = 300000;
    uint32 afkIgnoreActionGraceMs = 120000;
    // This realm does not allow camping a spawn point, so a character that never
    // leaves one spot across the whole window is itself the violation. Two window-wide
    // facts keep that from firing on a player who gathers while moving:
    //  - the character must have stayed inside CampStationaryEpsilon of the window
    //    origin at every counted action. The running maximum is used, not the final
    //    position, so "gather, step away, come back" cannot hide behind a small final
    //    distance;
    //  - being in combat at a counted action marks the window as not a pure gathering
    //    loop, so a camp that also fights is not reported.
    // A player who gathers across a zone travels well past MaxMoveDistance, which
    // resets the window entirely.
    float afkCampStationaryEpsilon = 0.25f;
    std::vector<uint32> afkIgnoreSpellIds;
    std::vector<uint32> afkIgnoreAuras;
    // Fishing is the one legitimate activity that requires standing perfectly still and
    // produces nothing but loot actions, so the gather detector cannot tell it apart from a
    // fixed-position farm loop. When this is on, casting Fishing refreshes the same grace as
    // the IgnoreSpellIds list, so an active angler is never reported. Turn it off on a realm
    // that wants fishing bots caught as well.
    bool afkIgnoreFishing = true;

    bool geometryEnabled = true;
    bool useVmaps = true;
    bool useMmaps = true;
    bool allowHotPathReachability = false;
    float pathBudgetSpeedFactor = 1.5f;
    float pathBudgetSlackYards = 1.0f;

    float notifyThreshold = 60.0f;
    float rollbackThreshold = 110.0f;
    float debuffThreshold = 150.0f;
    float jailThreshold = 210.0f;
    float kickThreshold = 260.0f;
    float banThreshold = 320.0f;
    // Persistent offense history raises the punishment floor for the current
    // evidence. Without this the risk gate below cancels every prior-tier
    // promotion, and intermittent cheaters are never escalated.
    bool offenseTierRiskFloor = true;
    // Strong evidence whose own family floor is already jail or above (coordinate
    // teleport, stationary coordinate shift, unreachable micro path, low gravity
    // jump, blocked wall climb) is treated as high risk on its own, so the risk gate
    // cannot cancel it. Without this the gate is purely an event frequency threshold
    // and an intermittent cheater below roughly one event per 24 seconds never
    // reaches a punishment at all. Restricted to the movement/geometry families: the
    // behavioural gather heuristic stays fully gated. Ban still additionally requires
    // Ban.StrongEvidenceRequired and Ban.MinOffenseCount.
    bool strongEvidenceFloor = true;

    bool rollbackEnabled = true;
    bool debuffEnabled = true;
    std::vector<uint32> debuffSpellIds;
    uint32 punishNotifyIntervalMs = 60000;
    std::string debuffApplyMessage = "AcAegis：你已进入减益处罚，剩余时间 {time}。";
    std::string debuffRemainMessage = "AcAegis：减益处罚剩余 {time}。";
    std::string debuffExpireMessage = "AcAegis：减益处罚已结束。";

    bool jailEnabled = true;
    uint32 jailMapId = 1;
    float jailX = 16226.5f;
    float jailY = 16403.6f;
    float jailZ = -64.5f;
    float jailO = 3.2f;
    float jailLeashRadius = 35.0f;
    uint32 jailReturnCheckMs = 5000;
    bool jailKeepDebuff = true;
    std::string jailApplyMessage = "AcAegis：你已被送入监狱，剩余时间 {time}。";
    std::string jailRemainMessage = "AcAegis：监禁剩余 {time}。";
    std::string jailExpireMessage = "AcAegis：监禁处罚已结束，你已被释放。";

    uint32 releaseMapId = 530;
    float releaseX = -1887.62f;
    float releaseY = 5359.09f;
    float releaseZ = -12.43f;
    float releaseO = 2.04f;
    float releaseRiskFactor = 0.35f;

    bool kickEnabled = true;
    bool banEnabled = true;
    bool punishBroadcastEnabled = true;
    std::string punishBroadcastFormat =
        "玩家 |cffff0000{player}|r 因 |cffff0000{type}作弊|r，被 |cffff0000{action}|r，请各位英雄引以为戒，规范游戏。";
    std::string banMode = "account-by-character";
    bool banStrongEvidenceRequired = true;
    uint32 banMinOffenseCount = 2;
    uint32 banStage3Days = 3;
    uint32 banStage4Days = 30;
    uint32 banPermanentTier = 5;
    std::string banPermanentDurationToken = "0";
    std::string banReasonTemplate = "AcAegis:{type}:tier={tier}:offense={offense}:risk={risk}";
    std::string banAuthor = "AcAegis";

    bool offenseEnabled = true;
    bool offenseCountOnlyOnPunish = true;
    uint32 offenseDecayDays = 90;
    std::string offenseDecayMode = "slow-tier-decay";
    uint32 offenseMaxTier = 5;
    uint32 stage1DebuffSeconds = 86400;
    uint32 stage2JailSeconds = 7200;
    bool stage5Permanent = true;
};

class AcAegisConfig
{
public:
    static AcAegisConfig* instance();

    void Reload();
    AegisConfig const& Get() const;
    uint32 GetEventBatchSize() const;
    uint32 GetEventFlushIntervalMs() const;
    uint32 GetEventQueueLimit() const;

private:
    AegisConfig _config;
    std::atomic<uint32> _eventBatchSize{32};
    std::atomic<uint32> _eventFlushIntervalMs{1000};
    std::atomic<uint32> _eventQueueLimit{4096};
};

#define sAcAegisConfig AcAegisConfig::instance()

#endif