#ifndef MOD_AC_AEGIS_MGR_H
#define MOD_AC_AEGIS_MGR_H

#include <optional>
#include <unordered_map>

#include "Object.h"

#include "AcAegisConfig.h"
#include "AcAegisGeometry.h"
#include "AcAegisPersistence.h"
#include "AcAegisTypes.h"

class Player;
class Spell;
class Unit;

struct AegisMovementContext
{
    uint32 nowMs = 0;
    bool hasMap = false;
    bool isAlive = false;
    bool isInCombat = false;
    bool isMounted = false;
    bool isBeingTeleported = false;
    bool isTaxiFlight = false;
    bool hasTransport = false;
    bool hasVehicle = false;
    bool hasAuthorizedAerialState = false;
    bool hasExternalAerialAura = false;
    bool hasConfiguredAuraWhitelist = false;
    bool hasWaterWalkAura = false;
    bool hasGhostAura = false;
    bool hasHoverAura = false;
    bool hasAfkIgnoreAura = false;
    bool recentSpellGrace = false;
    bool recentTeleportGrace = false;
    bool recentMapChangeGrace = false;
    bool recentVehicleGrace = false;
    bool recentTransportGrace = false;
    bool recentTaxiFlightGrace = false;
    bool recentRootAckGrace = false;
    bool recentControlledTeleportGrace = false;
    bool recentControlledChargeGrace = false;
    bool recentControlledJumpGrace = false;
    bool recentControlledPullGrace = false;
    bool recentFallGrace = false;
    bool recentKnockBackGrace = false;
    bool recentServerCanFly = false;
    bool recentMountAck = false;
    bool recentMountGrace = false;
    bool recentJump = false;
    bool recentExtendedJump = false;
    bool recentFall = false;
    bool shouldSkipAllDetectors = false;
    bool shouldSkipAerialDetectors = false;
};

class AcAegisMgr
{
public:
    static AcAegisMgr* instance();

    void ReloadConfig();
    bool GetPlayerDebugSnapshot(uint32 guidLow, AegisPlayerDebugSnapshot& outSnapshot) const;
    void ClearPlayerOffense(Player* player);
    void ClearPlayerOffense(uint32 guidLow);
    void DeletePlayerData(uint32 guidLow);
    void PurgeAllData();

    void OnLogin(Player* player);
    void OnLogout(Player* player);
    void OnWorldUpdate(uint32 diff);

    void OnSpellCast(Player* player, Spell* spell, bool skipCheck);
    bool OnBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z);
    void OnMapChanged(Player* player);
    void OnLoot(Player* player);
    void OnGatherAction(Player* player);
    void OnCanFlyByServer(Player* player, bool apply);
    void OnUnderAckMount(Player* player);
    void OnVehicleTransition(Player* player);
    void OnTransportTransition(Player* player);
    void OnRootAckUpd(Player* player);
    void OnJumpOpcode(Player* player, bool jump);
    void OnMovementInfoUpdate(Player* player, MovementInfo const& movementInfo);
    void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode);

    bool CheckMovement(Player* player, MovementInfo const& movementInfo, Unit* mover, bool jump);
    bool HandleDoubleJump(Player* player, Unit* mover);

private:
    using ContextMap = std::unordered_map<uint32, AegisPlayerContext>;

    std::string EscapeJson(std::string const& value) const;
    std::string ToIsoTimestamp() const;
    std::string EvidenceLevelText(AegisEvidenceLevel level) const;
    std::string ActionText(AegisActionDecision const& decision) const;
    std::string StageText(AegisPunishStage stage) const;
    std::string BuildAuditCommonFields(Player* player, AegisPlayerContext const* ctx) const;
    std::string BuildGmMessage(Player* player, AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx) const;
    std::string BuildPanelReason(AegisEvidenceEvent const& evidence) const;
    std::string BuildPunishPlayerMessage(std::string const& templ, std::string const& timeText) const;
    std::string ResolveLogPath(std::string const& configuredPath) const;
    void EnsureFileLogReady() const;
    void WriteAegisLog(std::string const& level, std::string const& message) const;
    void WriteAuditLog(std::string const& event, Player* player, AegisPlayerContext const* ctx, std::string const& fields = "") const;
    void AppendTextLine(std::string const& path, std::string const& line) const;
    void WritePanelCharacterLog(std::string const& action, std::string const& stage, std::string const& payload) const;
    void WritePanelAccountBanLog(Player* player, AegisEvidenceEvent const& evidence, AegisActionDecision const& decision) const;
    void WritePanelAccountBanLog(uint32 guidLow, uint32 accountId, std::string const& ip, AegisEvidenceEvent const& evidence, AegisActionDecision const& decision) const;
    void WritePanelDetectionLog(Player* player, AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx) const;
    void EnforceJail(Player* player, AegisPlayerContext& ctx, bool silent) const;

    AegisPlayerContext& GetOrCreate(Player* player);
    void Touch(Player* player);
    bool IsEnabledFor(Player* player) const;
    bool CanSafelyTeleportForPunish(Player* player) const;
    AegisMovementContext BuildMovementContext(Player* player, AegisPlayerContext const& ctx, uint32 nowMs) const;
    bool HasWhitelistedAura(AegisMovementContext const& movementCtx) const;
    bool ShouldSkipAllMovementDetectors(AegisMovementContext const& movementCtx) const;
    bool ShouldSkipAerialMovementDetectors(AegisMovementContext const& movementCtx) const;
    float ComputeAllowedSpeed(Player* player, MovementInfo const& movementInfo) const;
    void CaptureSample(Player* player, MovementInfo const& movementInfo, uint32 opcode, AegisPlayerContext& ctx);
    void MaybeUpdateSafePosition(Player* player, AegisPlayerContext& ctx, AegisMoveSample const& sample);
    void DecayRisk(AegisPlayerContext& ctx, uint32 nowMs) const;
    void ResetGatherWindow(Player* player, AegisPlayerContext& ctx, char const* reason) const;
    void TouchGatherWindow(Player* player, AegisPlayerContext& ctx, char const* source);

    std::optional<AegisEvidenceEvent> DetectSpeed(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectTime(Player* player, AegisPlayerContext& ctx) const;
    std::optional<AegisEvidenceEvent> DetectForceMove(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectRootBreak(Player* player, MovementInfo const& movementInfo, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectControlledMove(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectTransportRelativeSpeed(Player* player, AegisPlayerContext& ctx) const;
    std::optional<AegisEvidenceEvent> DetectTeleport(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectNoClip(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectWaterWalk(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectMount(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectFly(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectClimb(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;
    std::optional<AegisEvidenceEvent> DetectAfk(Player* player, AegisPlayerContext& ctx, AegisMovementContext const& movementCtx) const;

    bool HandleEvidence(Player* player, AegisPlayerContext& ctx, AegisEvidenceEvent const& evidence);
    AegisActionDecision DetermineAction(Player* player, AegisPlayerContext const& ctx, AegisEvidenceEvent const& evidence) const;
    bool ExecuteAction(Player* player, AegisPlayerContext& ctx, AegisEvidenceEvent const& evidence, AegisActionDecision const& decision);
    void NotifyGms(Player* player, AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx) const;
    void Rollback(Player* player, AegisPlayerContext& ctx) const;
    void ApplyDebuff(Player* player) const;
    void ClearDebuffs(Player* player) const;
    void SetHomebind(Player* player, uint32 mapId, float x, float y, float z) const;
    void Jail(Player* player, AegisPlayerContext& ctx) const;
    void Release(Player* player, AegisPlayerContext& ctx) const;
    std::string BuildBanReason(AegisEvidenceEvent const& evidence, AegisPlayerContext const& ctx, float risk) const;
    std::string ExecuteBan(Player* player, AegisActionDecision const& decision) const;
    bool ClearCoreBanState(uint32 guidLow, uint32 accountId, std::string const& banMode, Player* player) const;
    void SavePunishState(Player* player, AegisPlayerContext const& ctx) const;
    void SavePunishState(uint32 guidLow, uint32 accountId, AegisPlayerContext const& ctx) const;
    void MaintainPlayerPunishments();
    void MaintainOfflinePunishments();
    void CleanupOfflineContexts();
    void EmitSummary() const;

private:
    uint32 _elapsedMs = 0;
    uint32 _summaryElapsedMs = 0;
    uint32 _offlinePunishSweepElapsedMs = 0;
    ContextMap _players;
    AcAegisGeometry _geometry;
    AcAegisPersistence _persistence;
};

#define sAcAegisMgr AcAegisMgr::instance()

#endif