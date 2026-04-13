#ifndef MOD_AC_AEGIS_CONFIG_H
#define MOD_AC_AEGIS_CONFIG_H

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

    uint32 teleportGraceMs = 2000;
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
    float teleportStationaryMinDistance2D = 1.25f;
    float teleportStationaryMinDeltaZ = 1.87f;
    uint32 teleportStationaryWindowMs = 2500;
    uint32 teleportStationaryStrongHits = 2;
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
    uint32 forceMoveGraceMs = 1200;
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
    std::vector<uint32> afkIgnoreSpellIds;
    std::vector<uint32> afkIgnoreAuras;

    bool geometryEnabled = true;
    bool useVmaps = true;
    bool useMmaps = true;
    bool allowHotPathReachability = false;
    float longPathTriggerDistance = 40.0f;

    float notifyThreshold = 60.0f;
    float rollbackThreshold = 110.0f;
    float debuffThreshold = 150.0f;
    float jailThreshold = 210.0f;
    float kickThreshold = 260.0f;
    float banThreshold = 320.0f;

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
    uint32 stage3BanDays = 3;
    uint32 stage4BanDays = 30;
    bool stage5Permanent = true;
};

class AcAegisConfig
{
public:
    static AcAegisConfig* instance();

    void Reload();
    AegisConfig const& Get() const;

private:
    AegisConfig _config;
};

#define sAcAegisConfig AcAegisConfig::instance()

#endif