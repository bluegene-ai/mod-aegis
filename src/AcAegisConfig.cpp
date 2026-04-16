#include "AcAegisConfig.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include "Config.h"

namespace
{
    std::vector<uint32> ParseIdList(std::string const& raw)
    {
        std::vector<uint32> out;
        std::string token;
        std::stringstream ss(raw);

        while (std::getline(ss, token, ','))
        {
            token.erase(std::remove_if(token.begin(), token.end(), [](char c)
            {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n';
            }), token.end());

            if (token.empty())
                continue;

            try
            {
                uint32 id = static_cast<uint32>(std::stoul(token));
                if (id > 0)
                    out.push_back(id);
            }
            catch (...)
            {
            }
        }

        return out;
    }

    std::string SanitizeLower(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch)
        {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }
}

AcAegisConfig* AcAegisConfig::instance()
{
    static AcAegisConfig instance;
    return &instance;
}

void AcAegisConfig::Reload()
{
    _config.enabled = sConfigMgr->GetOption<bool>("AcAegis.Enabled", true);
    _config.enabledOnGmAccounts = sConfigMgr->GetOption<bool>("AcAegis.EnabledOnGmAccounts", false);
    _config.autoEnsureSchema = false;

    _config.logEnabled = sConfigMgr->GetOption<bool>("AcAegis.Log.Enabled", true);
    _config.verboseLog = sConfigMgr->GetOption<bool>("AcAegis.Log.Verbose", false);
    _config.summaryLogEnabled = sConfigMgr->GetOption<bool>("AcAegis.Log.Summary.Enabled", true);
    _config.summaryLogIntervalMs = sConfigMgr->GetOption<uint32>("AcAegis.Log.Summary.IntervalMs", 60000);
    _config.fileLogPath = sConfigMgr->GetOption<std::string>("AcAegis.Log.File.Path", "./logs/aegis.log");
    _config.gmNotifyFormat = SanitizeLower(sConfigMgr->GetOption<std::string>("AcAegis.Log.GmNotify.Format", "compact"));
    _config.gmNotifyCooldownMs = sConfigMgr->GetOption<uint32>("AcAegis.Log.GmNotify.CooldownMs", 10000);
    _config.eventBatchSize = sConfigMgr->GetOption<uint32>("AcAegis.Log.Event.BatchSize", 32);
    _config.eventFlushIntervalMs = sConfigMgr->GetOption<uint32>("AcAegis.Log.Event.FlushIntervalMs", 1000);
    _config.eventQueueLimit = sConfigMgr->GetOption<uint32>("AcAegis.Log.Event.QueueLimit", 4096);

    _config.panelOutputEnabled = sConfigMgr->GetOption<bool>("AcAegis.Panel.Enabled", false);
    _config.panelWriteDetections = sConfigMgr->GetOption<bool>("AcAegis.Panel.WriteDetections", true);
    _config.panelAdmin = sConfigMgr->GetOption<std::string>("AcAegis.Panel.Admin", "AcAegis");
    _config.panelServerId = sConfigMgr->GetOption<uint32>("AcAegis.Panel.ServerId", 1);
    _config.panelAccountLogPath = sConfigMgr->GetOption<std::string>("AcAegis.Panel.AccountLogPath", "");
    _config.panelCharacterLogPath = sConfigMgr->GetOption<std::string>("AcAegis.Panel.CharacterLogPath", "");
    _config.panelMovementReason = sConfigMgr->GetOption<std::string>("AcAegis.Panel.MovementReason", "外挂");
    _config.panelAfkReason = sConfigMgr->GetOption<std::string>("AcAegis.Panel.AfkReason", "挂机");

    _config.samplingBufferSize = sConfigMgr->GetOption<uint32>("AcAegis.Sampling.BufferSize", 32);
    _config.riskHalfLifeSeconds = sConfigMgr->GetOption<float>("AcAegis.Risk.HalfLifeSeconds", 75.0f);
    _config.riskMaxDeltaPerMove = sConfigMgr->GetOption<float>("AcAegis.Risk.MaxDeltaPerMove", 30.0f);

    _config.teleportGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.TeleportGraceMs", 2000);
    _config.teleportArrivalWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.TeleportArrivalWindowMs", 15000);
    _config.teleportArrivalRadius = sConfigMgr->GetOption<float>("AcAegis.Accuracy.TeleportArrivalRadius", 40.0f);
    _config.mapChangeGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.MapChangeGraceMs", 3000);
    _config.mobilitySpellGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.MobilitySpellGraceMs", 1500);
    _config.vehicleGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.VehicleGraceMs", 1500);
    _config.transportGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.TransportGraceMs", 2000);
    _config.fallGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Accuracy.FallGraceMs", 800);
    _config.spellWhitelist = ParseIdList(sConfigMgr->GetOption<std::string>("AcAegis.Accuracy.SpellWhitelist", ""));
    _config.auraWhitelist = ParseIdList(sConfigMgr->GetOption<std::string>("AcAegis.Accuracy.AuraWhitelist", ""));

    _config.speedEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Speed.Enabled", true);
    _config.speedTolerancePct = sConfigMgr->GetOption<float>("AcAegis.Detector.Speed.TolerancePct", 10.0f);
    _config.speedFlatMargin = sConfigMgr->GetOption<float>("AcAegis.Detector.Speed.FlatMargin", 0.75f);
    _config.speedMinDtMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Speed.MinDtMs", 100);
    _config.speedMaxDtMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Speed.MaxDtMs", 1500);
    _config.speedWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Speed.WindowMs", 1800);
    _config.speedMediumHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Speed.MediumHits", 2);
    _config.speedStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Speed.StrongHits", 3);
    _config.speedStrongRatio = sConfigMgr->GetOption<float>("AcAegis.Detector.Speed.StrongRatio", 1.6f);

    _config.timeEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Time.Enabled", true);
    _config.timeMinClientLeadMs = sConfigMgr->GetOption<float>("AcAegis.Detector.Time.MinClientLeadMs", 180.0f);
    _config.timeMinRatio = sConfigMgr->GetOption<float>("AcAegis.Detector.Time.MinRatio", 1.35f);
    _config.timeMinDistance2D = sConfigMgr->GetOption<float>("AcAegis.Detector.Time.MinDistance2D", 1.25f);
    _config.timeWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Time.WindowMs", 2500);
    _config.timeStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Time.StrongHits", 3);

    _config.teleportEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Teleport.Enabled", true);
    _config.teleportMinDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.MinDistance", 18.0f);
    _config.teleportSpeedMultiplier = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.SpeedMultiplier", 3.0f);
    _config.teleportCoordinateMinDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.CoordinateMinDistance", 45.0f);
    _config.teleportCoordinateSpeedMultiplier = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.CoordinateSpeedMultiplier", 1.75f);
    _config.teleportAxisStrongDelta = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.AxisStrongDelta", 25.0f);
    _config.teleportStationaryMinDistance2D = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.StationaryMinDistance2D", 4.5f);
    _config.teleportStationaryMinDeltaZ = sConfigMgr->GetOption<float>("AcAegis.Detector.Teleport.StationaryMinDeltaZ", 3.0f);
    _config.teleportStationaryWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Teleport.StationaryWindowMs", 2500);
    _config.teleportStationaryStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Teleport.StationaryStrongHits", 3);
    _config.teleportBurstWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Teleport.BurstWindowMs", 1800);
    _config.teleportBurstStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Teleport.BurstStrongHits", 2);

    _config.noClipEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.NoClip.Enabled", true);
    _config.noClipCheckIntervalMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.NoClip.CheckIntervalMs", 300);
    _config.noClipMinSegmentDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.NoClip.MinSegmentDistance", 2.5f);
    _config.noClipMaxDirectDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.NoClip.MaxDirectDistance", 35.0f);
    _config.noClipRayZOffset = sConfigMgr->GetOption<float>("AcAegis.Detector.NoClip.RayZOffset", 1.0f);
    _config.noClipMinRemainingDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.NoClip.MinRemainingDistance", 0.8f);
    _config.noClipMaxSpeedMultiplier = sConfigMgr->GetOption<float>("AcAegis.Detector.NoClip.MaxSpeedMultiplier", 1.5f);
    _config.noClipCumulativeWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.NoClip.CumulativeWindowMs", 1800);
    _config.noClipCumulativeMinDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.NoClip.CumulativeMinDistance", 2.8f);
    _config.noClipCumulativeStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.NoClip.CumulativeStrongHits", 3);

    _config.flyEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Fly.Enabled", true);
    _config.flyMinHeightAboveGround = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.MinHeightAboveGround", 6.0f);
    _config.flySustainMinHorizontalDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.SustainMinHorizontalDistance", 3.5f);
    _config.flyIllegalFlagMinHeightAboveGround = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.IllegalFlagMinHeightAboveGround", 1.0f);
    _config.flyIllegalFlagMinHorizontalDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.IllegalFlagMinHorizontalDistance", 1.0f);
    _config.flyCanFlyGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Fly.CanFlyGraceMs", 2500);
    _config.flyAirStallMaxHorizontalDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.AirStallMaxHorizontalDistance", 0.75f);
    _config.flyAirStallMaxDeltaZ = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.AirStallMaxDeltaZ", 0.15f);
    _config.flyAirStallWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Fly.AirStallWindowMs", 2500);
    _config.flyAirStallStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Fly.AirStallStrongHits", 3);
    _config.flyWaterWalkMinHorizontalDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.WaterWalkMinHorizontalDistance", 1.5f);
    _config.flyWaterWalkSurfaceTolerance = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.WaterWalkSurfaceTolerance", 0.35f);
    _config.flyWaterWalkMinWaterDepth = sConfigMgr->GetOption<float>("AcAegis.Detector.Fly.WaterWalkMinWaterDepth", 1.75f);
    _config.flyWaterWalkWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Fly.WaterWalkWindowMs", 2500);
    _config.flyWaterWalkStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Fly.WaterWalkStrongHits", 3);

    _config.mountEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Mount.Enabled", true);
    _config.mountGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Mount.GraceMs", 2500);
    _config.mountIndoorWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Mount.IndoorWindowMs", 2500);
    _config.mountIndoorStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Mount.IndoorStrongHits", 2);
    _config.mountIndoorMinMoveDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Mount.IndoorMinMoveDistance", 0.5f);

    _config.forceMoveEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.ForceMove.Enabled", true);
    _config.forceMoveGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.ForceMove.GraceMs", 1200);
    _config.forceMoveMinAckSpeedXY = sConfigMgr->GetOption<float>("AcAegis.Detector.ForceMove.MinAckSpeedXY", 4.0f);
    _config.forceMoveMinAckSpeedZ = sConfigMgr->GetOption<float>("AcAegis.Detector.ForceMove.MinAckSpeedZ", 2.5f);
    _config.forceMoveExpectedFactor = sConfigMgr->GetOption<float>("AcAegis.Detector.ForceMove.ExpectedFactor", 0.25f);
    _config.forceMoveMinDistance2D = sConfigMgr->GetOption<float>("AcAegis.Detector.ForceMove.MinDistance2D", 0.45f);
    _config.forceMoveMinDeltaZ = sConfigMgr->GetOption<float>("AcAegis.Detector.ForceMove.MinDeltaZ", 0.2f);
    _config.forceMoveWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.ForceMove.WindowMs", 4000);
    _config.forceMoveStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.ForceMove.StrongHits", 2);

    _config.climbEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Climb.Enabled", true);
    _config.climbMinRise = sConfigMgr->GetOption<float>("AcAegis.Detector.Climb.MinRise", 1.87f);
    _config.climbMinHorizontalDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Climb.MinHorizontalDistance", 0.8f);
    _config.climbMinSlopeRatio = sConfigMgr->GetOption<float>("AcAegis.Detector.Climb.MinSlopeRatio", 1.73f);
    _config.climbWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Climb.WindowMs", 4000);
    _config.climbStrongHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Climb.StrongHits", 3);
    _config.superJumpMinHeight = sConfigMgr->GetOption<float>("AcAegis.Detector.Climb.SuperJumpMinHeight", 8.0f);
    _config.superJumpMinDeltaZ = sConfigMgr->GetOption<float>("AcAegis.Detector.Climb.SuperJumpMinDeltaZ", 4.0f);
    _config.superJumpWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Climb.SuperJumpWindowMs", 1200);
    _config.doubleJumpMinHeight = sConfigMgr->GetOption<float>("AcAegis.Detector.Climb.DoubleJumpMinHeight", 2.5f);
    _config.doubleJumpMaxRepeatMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Climb.DoubleJumpMaxRepeatMs", 350);
    _config.doubleJumpWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Climb.DoubleJumpWindowMs", 1200);
    _config.doubleJumpRepeatHits = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Climb.DoubleJumpRepeatHits", 2);

    _config.afkEnabled = sConfigMgr->GetOption<bool>("AcAegis.Detector.Afk.Enabled", true);
    _config.afkWindowMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.WindowMs", 600000);
    _config.afkMinActions = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.MinActions", 12);
    _config.afkStrongActions = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.StrongActions", 20);
    _config.afkMinActionGapMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.MinActionGapMs", 1500);
    _config.afkMaxMoveDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Afk.MaxMoveDistance", 6.0f);
    _config.afkStrongMoveDistance = sConfigMgr->GetOption<float>("AcAegis.Detector.Afk.StrongMoveDistance", 3.0f);
    _config.afkMinLootCount = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.MinLootCount", 4);
    _config.afkMinGatherCount = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.MinGatherCount", 6);
    _config.afkMinSuspiciousWindows = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.MinSuspiciousWindows", 2);
    _config.afkEvidenceCooldownMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.EvidenceCooldownMs", 300000);
    _config.afkIgnoreActionGraceMs = sConfigMgr->GetOption<uint32>("AcAegis.Detector.Afk.IgnoreActionGraceMs", 120000);
    _config.afkIgnoreSpellIds = ParseIdList(sConfigMgr->GetOption<std::string>("AcAegis.Detector.Afk.IgnoreSpellIds", ""));
    _config.afkIgnoreAuras = ParseIdList(sConfigMgr->GetOption<std::string>("AcAegis.Detector.Afk.IgnoreAuras", ""));

    _config.geometryEnabled = sConfigMgr->GetOption<bool>("AcAegis.Geometry.Enabled", true);
    _config.useVmaps = sConfigMgr->GetOption<bool>("AcAegis.Geometry.UseVmaps", true);
    _config.useMmaps = sConfigMgr->GetOption<bool>("AcAegis.Geometry.UseMmaps", true);
    _config.allowHotPathReachability = sConfigMgr->GetOption<bool>("AcAegis.Geometry.AllowHotPathReachability", false);
    _config.longPathTriggerDistance = sConfigMgr->GetOption<float>("AcAegis.Geometry.LongPathTriggerDistance", 40.0f);

    _config.notifyThreshold = sConfigMgr->GetOption<float>("AcAegis.Risk.NotifyThreshold", 60.0f);
    _config.rollbackThreshold = sConfigMgr->GetOption<float>("AcAegis.Risk.RollbackThreshold", 110.0f);
    _config.debuffThreshold = sConfigMgr->GetOption<float>("AcAegis.Risk.DebuffThreshold", 150.0f);
    _config.jailThreshold = sConfigMgr->GetOption<float>("AcAegis.Risk.JailThreshold", 210.0f);
    _config.kickThreshold = sConfigMgr->GetOption<float>("AcAegis.Risk.KickThreshold", 260.0f);
    _config.banThreshold = sConfigMgr->GetOption<float>("AcAegis.Risk.BanThreshold", 320.0f);

    _config.rollbackEnabled = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Rollback.Enabled", true);
    _config.debuffEnabled = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Debuff.Enabled", true);
    _config.debuffSpellIds = ParseIdList(sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Debuff.SpellIds", "9454,22856"));
    _config.punishNotifyIntervalMs = sConfigMgr->GetOption<uint32>("AcAegis.AutoAction.Notify.IntervalMs", 60000);
    _config.debuffApplyMessage = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Notify.DebuffApplyMessage", "AcAegis：你已进入减益处罚，剩余时间 {time}。");
    _config.debuffRemainMessage = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Notify.DebuffRemainMessage", "AcAegis：减益处罚剩余 {time}。");
    _config.debuffExpireMessage = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Notify.DebuffExpireMessage", "AcAegis：减益处罚已结束。");

    _config.jailEnabled = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Jail.Enabled", true);
    _config.jailMapId = sConfigMgr->GetOption<uint32>("AcAegis.AutoAction.Jail.MapId", 1);
    _config.jailX = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Jail.X", 16226.5f);
    _config.jailY = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Jail.Y", 16403.6f);
    _config.jailZ = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Jail.Z", -64.5f);
    _config.jailO = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Jail.O", 3.2f);
    _config.jailLeashRadius = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Jail.LeashRadius", 35.0f);
    _config.jailReturnCheckMs = sConfigMgr->GetOption<uint32>("AcAegis.AutoAction.Jail.ReturnCheckMs", 5000);
    _config.jailKeepDebuff = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Jail.KeepDebuff", true);
    _config.jailApplyMessage = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Notify.JailApplyMessage", "AcAegis：你已被送入监狱，剩余时间 {time}。");
    _config.jailRemainMessage = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Notify.JailRemainMessage", "AcAegis：监禁剩余 {time}。");
    _config.jailExpireMessage = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Notify.JailExpireMessage", "AcAegis：监禁处罚已结束，你已被释放。");

    _config.releaseMapId = sConfigMgr->GetOption<uint32>("AcAegis.AutoAction.Release.MapId", 530);
    _config.releaseX = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Release.X", -1887.62f);
    _config.releaseY = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Release.Y", 5359.09f);
    _config.releaseZ = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Release.Z", -12.43f);
    _config.releaseO = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Release.O", 2.04f);
    _config.releaseRiskFactor = sConfigMgr->GetOption<float>("AcAegis.AutoAction.Release.RiskFactor", 0.35f);

    _config.kickEnabled = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Kick.Enabled", true);
    _config.banEnabled = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Ban.Enabled", true);
    _config.punishBroadcastEnabled = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Broadcast.Enabled", true);
    _config.banMode = SanitizeLower(sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Ban.Mode", "account-by-character"));
    _config.banStrongEvidenceRequired = sConfigMgr->GetOption<bool>("AcAegis.AutoAction.Ban.StrongEvidenceRequired", true);
    _config.banMinOffenseCount = sConfigMgr->GetOption<uint32>("AcAegis.AutoAction.Ban.MinOffenseCount", 2);
    _config.banStage3Days = sConfigMgr->GetOption<uint32>(
        "AcAegis.AutoAction.Ban.Stage3Days", 3);
    _config.banStage4Days = sConfigMgr->GetOption<uint32>(
        "AcAegis.AutoAction.Ban.Stage4Days", 30);
    _config.banPermanentTier = sConfigMgr->GetOption<uint32>("AcAegis.AutoAction.Ban.PermanentTier", 5);
    _config.banPermanentDurationToken = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Ban.PermanentDurationToken", "0");
    _config.banReasonTemplate = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Ban.ReasonTemplate", "AcAegis:{type}:tier={tier}:offense={offense}:risk={risk}");
    _config.banAuthor = sConfigMgr->GetOption<std::string>("AcAegis.AutoAction.Ban.Author", "AcAegis");

    _config.offenseEnabled = sConfigMgr->GetOption<bool>("AcAegis.Offense.Enabled", true);
    _config.offenseCountOnlyOnPunish = sConfigMgr->GetOption<bool>("AcAegis.Offense.CountOnlyOnPunish", true);
    _config.offenseDecayDays = sConfigMgr->GetOption<uint32>("AcAegis.Offense.DecayDays", 90);
    _config.offenseDecayMode = SanitizeLower(sConfigMgr->GetOption<std::string>("AcAegis.Offense.DecayMode", "slow-tier-decay"));
    _config.offenseMaxTier = sConfigMgr->GetOption<uint32>("AcAegis.Offense.MaxTier", 5);
    _config.stage1DebuffSeconds = sConfigMgr->GetOption<uint32>("AcAegis.Offense.Stage1DebuffSeconds", 86400);
    _config.stage2JailSeconds = sConfigMgr->GetOption<uint32>("AcAegis.Offense.Stage2JailSeconds", 7200);
    _config.stage5Permanent = sConfigMgr->GetOption<bool>("AcAegis.Offense.Stage5Permanent", true);

    _config.samplingBufferSize = std::clamp<uint32>(_config.samplingBufferSize, 2, 256);
    _config.summaryLogIntervalMs = std::max<uint32>(1000, _config.summaryLogIntervalMs);
    _config.gmNotifyCooldownMs = std::max<uint32>(1000, _config.gmNotifyCooldownMs);
    _config.eventBatchSize = std::clamp<uint32>(_config.eventBatchSize, 1, 512);
    _config.eventFlushIntervalMs = std::clamp<uint32>(_config.eventFlushIntervalMs, 100, 10000);
    _config.eventQueueLimit = std::max<uint32>(_config.eventBatchSize, _config.eventQueueLimit);
    _config.riskHalfLifeSeconds = std::max(1.0f, _config.riskHalfLifeSeconds);
    _config.riskMaxDeltaPerMove = std::max(1.0f, _config.riskMaxDeltaPerMove);

    _config.speedTolerancePct = std::max(0.0f, _config.speedTolerancePct);
    _config.speedFlatMargin = std::max(0.0f, _config.speedFlatMargin);
    _config.speedMinDtMs = std::max<uint32>(1, _config.speedMinDtMs);
    _config.speedMaxDtMs = std::max(_config.speedMinDtMs, _config.speedMaxDtMs);
    _config.speedWindowMs = std::max<uint32>(200, _config.speedWindowMs);
    _config.speedMediumHits = std::max<uint32>(1, _config.speedMediumHits);
    _config.speedStrongHits = std::max(_config.speedMediumHits, _config.speedStrongHits);
    _config.speedStrongRatio = std::max(1.05f, _config.speedStrongRatio);

    _config.timeMinClientLeadMs = std::max(10.0f, _config.timeMinClientLeadMs);
    _config.timeMinRatio = std::max(1.05f, _config.timeMinRatio);
    _config.timeMinDistance2D = std::max(0.1f, _config.timeMinDistance2D);
    _config.timeWindowMs = std::max<uint32>(200, _config.timeWindowMs);
    _config.timeStrongHits = std::max<uint32>(1, _config.timeStrongHits);

    _config.teleportMinDistance = std::max(0.0f, _config.teleportMinDistance);
    _config.teleportArrivalWindowMs = std::max<uint32>(_config.teleportGraceMs, _config.teleportArrivalWindowMs);
    _config.teleportArrivalRadius = std::max(1.0f, _config.teleportArrivalRadius);
    _config.teleportSpeedMultiplier = std::max(1.0f, _config.teleportSpeedMultiplier);
    _config.teleportCoordinateMinDistance = std::max(_config.teleportMinDistance, _config.teleportCoordinateMinDistance);
    _config.teleportCoordinateSpeedMultiplier = std::max(1.0f, _config.teleportCoordinateSpeedMultiplier);
    _config.teleportAxisStrongDelta = std::max(1.0f, _config.teleportAxisStrongDelta);
    _config.teleportStationaryMinDistance2D = std::max(0.1f, _config.teleportStationaryMinDistance2D);
    _config.teleportStationaryMinDeltaZ = std::max(0.1f, _config.teleportStationaryMinDeltaZ);
    _config.teleportStationaryWindowMs = std::max<uint32>(200, _config.teleportStationaryWindowMs);
    _config.teleportStationaryStrongHits = std::max<uint32>(1, _config.teleportStationaryStrongHits);
    _config.teleportBurstWindowMs = std::max<uint32>(200, _config.teleportBurstWindowMs);
    _config.teleportBurstStrongHits = std::max<uint32>(1, _config.teleportBurstStrongHits);

    _config.noClipCheckIntervalMs = std::max<uint32>(50, _config.noClipCheckIntervalMs);
    _config.noClipMinSegmentDistance = std::max(0.5f, _config.noClipMinSegmentDistance);
    _config.noClipMaxDirectDistance = std::max(_config.noClipMinSegmentDistance, _config.noClipMaxDirectDistance);
    _config.noClipRayZOffset = std::max(0.0f, _config.noClipRayZOffset);
    _config.noClipMinRemainingDistance = std::max(0.0f, _config.noClipMinRemainingDistance);
    _config.noClipMaxSpeedMultiplier = std::max(1.0f, _config.noClipMaxSpeedMultiplier);
    _config.noClipCumulativeWindowMs = std::max<uint32>(200, _config.noClipCumulativeWindowMs);
    _config.noClipCumulativeMinDistance = std::max(_config.noClipMinSegmentDistance, _config.noClipCumulativeMinDistance);
    _config.noClipCumulativeStrongHits = std::max<uint32>(1, _config.noClipCumulativeStrongHits);

    _config.flyMinHeightAboveGround = std::max(1.0f, _config.flyMinHeightAboveGround);
    _config.flySustainMinHorizontalDistance = std::max(0.5f, _config.flySustainMinHorizontalDistance);
    _config.flyIllegalFlagMinHeightAboveGround = std::max(0.0f, _config.flyIllegalFlagMinHeightAboveGround);
    _config.flyIllegalFlagMinHorizontalDistance = std::max(0.0f, _config.flyIllegalFlagMinHorizontalDistance);
    _config.flyCanFlyGraceMs = std::max<uint32>(100, _config.flyCanFlyGraceMs);
    _config.flyAirStallMaxHorizontalDistance = std::max(0.05f, _config.flyAirStallMaxHorizontalDistance);
    _config.flyAirStallMaxDeltaZ = std::max(0.01f, _config.flyAirStallMaxDeltaZ);
    _config.flyAirStallWindowMs = std::max<uint32>(200, _config.flyAirStallWindowMs);
    _config.flyAirStallStrongHits = std::max<uint32>(1, _config.flyAirStallStrongHits);
    _config.flyWaterWalkMinHorizontalDistance = std::max(0.1f, _config.flyWaterWalkMinHorizontalDistance);
    _config.flyWaterWalkSurfaceTolerance = std::max(0.05f, _config.flyWaterWalkSurfaceTolerance);
    _config.flyWaterWalkMinWaterDepth = std::max(0.2f, _config.flyWaterWalkMinWaterDepth);
    _config.flyWaterWalkWindowMs = std::max<uint32>(200, _config.flyWaterWalkWindowMs);
    _config.flyWaterWalkStrongHits = std::max<uint32>(1, _config.flyWaterWalkStrongHits);

    _config.mountGraceMs = std::max<uint32>(0, _config.mountGraceMs);
    _config.mountIndoorWindowMs = std::max<uint32>(200, _config.mountIndoorWindowMs);
    _config.mountIndoorStrongHits = std::max<uint32>(1, _config.mountIndoorStrongHits);
    _config.mountIndoorMinMoveDistance = std::max(0.0f, _config.mountIndoorMinMoveDistance);

    _config.forceMoveGraceMs = std::max<uint32>(100, _config.forceMoveGraceMs);
    _config.forceMoveMinAckSpeedXY = std::max(0.1f, _config.forceMoveMinAckSpeedXY);
    _config.forceMoveMinAckSpeedZ = std::max(0.1f, _config.forceMoveMinAckSpeedZ);
    _config.forceMoveExpectedFactor = std::clamp(_config.forceMoveExpectedFactor, 0.05f, 1.0f);
    _config.forceMoveMinDistance2D = std::max(0.05f, _config.forceMoveMinDistance2D);
    _config.forceMoveMinDeltaZ = std::max(0.01f, _config.forceMoveMinDeltaZ);
    _config.forceMoveWindowMs = std::max<uint32>(200, _config.forceMoveWindowMs);
    _config.forceMoveStrongHits = std::max<uint32>(1, _config.forceMoveStrongHits);

    _config.climbMinRise = std::max(0.5f, _config.climbMinRise);
    _config.climbMinHorizontalDistance = std::max(0.1f, _config.climbMinHorizontalDistance);
    _config.climbMinSlopeRatio = std::max(0.5f, _config.climbMinSlopeRatio);
    _config.climbWindowMs = std::max<uint32>(500, _config.climbWindowMs);
    _config.climbStrongHits = std::max<uint32>(1, _config.climbStrongHits);
    _config.superJumpMinHeight = std::max(1.0f, _config.superJumpMinHeight);
    _config.superJumpMinDeltaZ = std::max(0.5f, _config.superJumpMinDeltaZ);
    _config.superJumpWindowMs = std::max<uint32>(100, _config.superJumpWindowMs);
    _config.doubleJumpMinHeight = std::max(0.5f, _config.doubleJumpMinHeight);
    _config.doubleJumpMaxRepeatMs = std::max<uint32>(50, _config.doubleJumpMaxRepeatMs);
    _config.doubleJumpWindowMs = std::max<uint32>(_config.doubleJumpMaxRepeatMs, _config.doubleJumpWindowMs);
    _config.doubleJumpRepeatHits = std::max<uint32>(1, _config.doubleJumpRepeatHits);

    _config.afkWindowMs = std::max<uint32>(60000, _config.afkWindowMs);
    _config.afkMinActions = std::max<uint32>(2, _config.afkMinActions);
    _config.afkStrongActions = std::max(_config.afkMinActions, _config.afkStrongActions);
    _config.afkMinActionGapMs = std::max<uint32>(100, _config.afkMinActionGapMs);
    _config.afkMaxMoveDistance = std::max(0.5f, _config.afkMaxMoveDistance);
    _config.afkStrongMoveDistance = std::max(0.1f, std::min(_config.afkMaxMoveDistance, _config.afkStrongMoveDistance));
    _config.afkMinLootCount = std::max<uint32>(0, _config.afkMinLootCount);
    _config.afkMinGatherCount = std::max<uint32>(0, _config.afkMinGatherCount);
    _config.afkMinSuspiciousWindows = std::max<uint32>(1, _config.afkMinSuspiciousWindows);
    _config.afkEvidenceCooldownMs = std::max<uint32>(1000, _config.afkEvidenceCooldownMs);
    _config.afkIgnoreActionGraceMs = std::max<uint32>(0, _config.afkIgnoreActionGraceMs);

    _config.longPathTriggerDistance = std::max(1.0f, _config.longPathTriggerDistance);

    _config.notifyThreshold = std::max(0.0f, _config.notifyThreshold);
    _config.rollbackThreshold = std::max(_config.notifyThreshold, _config.rollbackThreshold);
    _config.debuffThreshold = std::max(_config.rollbackThreshold, _config.debuffThreshold);
    _config.jailThreshold = std::max(_config.debuffThreshold, _config.jailThreshold);
    _config.kickThreshold = std::max(_config.jailThreshold, _config.kickThreshold);
    _config.banThreshold = std::max(_config.kickThreshold, _config.banThreshold);

    _config.banMinOffenseCount = std::max<uint32>(1, _config.banMinOffenseCount);
    _config.banStage3Days = std::max<uint32>(1, _config.banStage3Days);
    _config.banStage4Days = std::max(_config.banStage3Days, _config.banStage4Days);
    _config.banPermanentTier = std::max<uint32>(1, _config.banPermanentTier);
    _config.jailLeashRadius = std::max(3.0f, _config.jailLeashRadius);
    _config.jailReturnCheckMs = std::max<uint32>(1000, _config.jailReturnCheckMs);
    _config.punishNotifyIntervalMs = std::max<uint32>(1000, _config.punishNotifyIntervalMs);
    _config.releaseRiskFactor = std::clamp(_config.releaseRiskFactor, 0.0f, 1.0f);

    _config.offenseMaxTier = std::clamp<uint32>(_config.offenseMaxTier, 1, 10);
    _config.stage1DebuffSeconds = std::max<uint32>(1, _config.stage1DebuffSeconds);
    _config.stage2JailSeconds = std::max<uint32>(1, _config.stage2JailSeconds);
    _eventBatchSize.store(_config.eventBatchSize, std::memory_order_relaxed);
    _eventFlushIntervalMs.store(_config.eventFlushIntervalMs,
        std::memory_order_relaxed);
    _eventQueueLimit.store(_config.eventQueueLimit,
        std::memory_order_relaxed);
}

AegisConfig const& AcAegisConfig::Get() const
{
    return _config;
}

uint32 AcAegisConfig::GetEventBatchSize() const
{
    return _eventBatchSize.load(std::memory_order_relaxed);
}

uint32 AcAegisConfig::GetEventFlushIntervalMs() const
{
    return _eventFlushIntervalMs.load(std::memory_order_relaxed);
}

uint32 AcAegisConfig::GetEventQueueLimit() const
{
    return _eventQueueLimit.load(std::memory_order_relaxed);
}