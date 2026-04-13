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

class AcAegisPersistence
{
public:
    void EnsureSchema() const;
    bool LoadOffense(Player* player, AegisOffenseRecord& outRecord) const;
    bool LoadOffenseByGuid(uint32 guidLow, AegisOffenseRecord& outRecord) const;
    std::vector<AegisOffenseRecord> LoadAllOffenses() const;
    std::vector<AegisOffenseRecord> LoadExpiredTempBans(int64 nowEpoch) const;
    void SaveOffense(AegisOffenseRecord const& record) const;
    void InsertEvent(Player* player, AegisEvidenceEvent const& eventData, float totalRisk) const;
    void DeleteOffense(uint32 guidLow) const;
    void DeletePlayerData(uint32 guidLow) const;
    void PurgeAllData() const;
};

#endif