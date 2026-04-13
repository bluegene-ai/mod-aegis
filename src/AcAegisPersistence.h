#ifndef MOD_AC_AEGIS_PERSISTENCE_H
#define MOD_AC_AEGIS_PERSISTENCE_H

#include <string>
#include <vector>

#include "AcAegisTypes.h"

class Player;

struct AegisOffenseRecord
{
    uint32 guid = 0;
    uint32 accountId = 0;
    uint32 offenseCount = 0;
    uint8 offenseTier = 0;
    uint8 punishStage = 0;
    uint8 lastCheatType = 0;
    int64 lastOffenseEpoch = 0;
    int64 debuffUntilEpoch = 0;
    int64 jailUntilEpoch = 0;
    int64 banUntilEpoch = 0;
    bool permanentBan = false;
    std::string lastReason;
    std::string lastBanMode;
    std::string lastBanResult;
};

struct AegisEventRecord
{
    uint32 guid = 0;
    uint32 accountId = 0;
    uint32 mapId = 0;
    uint32 zoneId = 0;
    uint32 areaId = 0;
    uint8 cheatType = 0;
    uint8 evidenceLevel = 0;
    float riskDelta = 0.0f;
    float totalRiskAfter = 0.0f;
    std::string evidenceTag;
    std::string detailText;
    float posX = 0.0f;
    float posY = 0.0f;
    float posZ = 0.0f;
};

class AcAegisPersistence
{
public:
    void EnsureSchema() const;
    bool LoadOffense(Player* player, AegisOffenseRecord& outRecord) const;
    bool LoadOffenseByGuid(uint32 guidLow, AegisOffenseRecord& outRecord) const;
    std::vector<AegisOffenseRecord> LoadAllOffenses() const;
    std::vector<AegisOffenseRecord> LoadExpiredTempBans(int64 nowEpoch) const;
    void SaveOffense(AegisOffenseRecord const& record) const;
    void QueueEvent(AegisEventRecord const& eventRecord) const;
    void DeleteOffense(uint32 guidLow) const;
    void DeletePlayerData(uint32 guidLow) const;
    void PurgeAllData() const;
};

#endif