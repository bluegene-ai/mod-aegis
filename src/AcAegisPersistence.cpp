#include "AcAegisPersistence.h"

#include "DatabaseEnv.h"
#include "Player.h"
#include "WorldSession.h"

namespace
{
    std::string EscapeForCharacterDb(std::string value)
    {
        CharacterDatabase.EscapeString(value);
        return value;
    }

    void LoadOffenseRecordFromFields(Field* field, AegisOffenseRecord& outRecord)
    {
        outRecord.guid = field[0].Get<uint32>();
        outRecord.accountId = field[1].Get<uint32>();
        outRecord.offenseCount = field[2].Get<uint32>();
        outRecord.offenseTier = field[3].Get<uint8>();
        outRecord.punishStage = field[4].Get<uint8>();
        outRecord.lastCheatType = field[5].Get<uint8>();
        outRecord.lastOffenseEpoch = field[6].Get<int64>();
        outRecord.debuffUntilEpoch = field[7].Get<int64>();
        outRecord.jailUntilEpoch = field[8].Get<int64>();
        outRecord.banUntilEpoch = field[9].Get<int64>();
        outRecord.permanentBan = field[10].Get<bool>();
        outRecord.lastReason = field[11].Get<std::string>();
        outRecord.lastBanMode = field[12].Get<std::string>();
        outRecord.lastBanResult = field[13].Get<std::string>();
    }
}

void AcAegisPersistence::EnsureSchema() const
{
    CharacterDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS ac_aegis_offense ("
        "guid INT UNSIGNED NOT NULL, "
        "account_id INT UNSIGNED NOT NULL DEFAULT 0, "
        "offense_count INT UNSIGNED NOT NULL DEFAULT 0, "
        "offense_tier TINYINT UNSIGNED NOT NULL DEFAULT 0, "
        "punish_stage TINYINT UNSIGNED NOT NULL DEFAULT 0, "
        "last_cheat_type TINYINT UNSIGNED NOT NULL DEFAULT 0, "
        "last_offense_at BIGINT NOT NULL DEFAULT 0, "
        "debuff_until BIGINT NOT NULL DEFAULT 0, "
        "jail_until BIGINT NOT NULL DEFAULT 0, "
        "ban_until BIGINT NOT NULL DEFAULT 0, "
        "permanent_ban TINYINT(1) NOT NULL DEFAULT 0, "
        "last_reason VARCHAR(255) NOT NULL DEFAULT '', "
        "last_ban_mode VARCHAR(32) NOT NULL DEFAULT '', "
        "last_ban_result VARCHAR(64) NOT NULL DEFAULT '', "
        "PRIMARY KEY (guid), "
        "KEY idx_ac_aegis_offense_account (account_id), "
        "KEY idx_ac_aegis_offense_stage (punish_stage), "
        "KEY idx_ac_aegis_offense_last_offense (last_offense_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

    CharacterDatabase.Execute(
        "CREATE TABLE IF NOT EXISTS ac_aegis_event ("
        "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
        "guid INT UNSIGNED NOT NULL, "
        "account_id INT UNSIGNED NOT NULL DEFAULT 0, "
        "map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
        "zone_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
        "area_id SMALLINT UNSIGNED NOT NULL DEFAULT 0, "
        "cheat_type TINYINT UNSIGNED NOT NULL DEFAULT 0, "
        "evidence_level TINYINT UNSIGNED NOT NULL DEFAULT 0, "
        "risk_delta FLOAT NOT NULL DEFAULT 0, "
        "total_risk_after FLOAT NOT NULL DEFAULT 0, "
        "evidence_tag VARCHAR(64) NOT NULL DEFAULT '', "
        "detail_text VARCHAR(255) NOT NULL DEFAULT '', "
        "pos_x FLOAT NOT NULL DEFAULT 0, "
        "pos_y FLOAT NOT NULL DEFAULT 0, "
        "pos_z FLOAT NOT NULL DEFAULT 0, "
        "created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
        "PRIMARY KEY (id), "
        "KEY idx_ac_aegis_event_guid (guid), "
        "KEY idx_ac_aegis_event_account (account_id), "
        "KEY idx_ac_aegis_event_cheat (cheat_type), "
        "KEY idx_ac_aegis_event_created (created_at)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
}

bool AcAegisPersistence::LoadOffense(Player* player, AegisOffenseRecord& outRecord) const
{
    if (!player)
        return false;

    return LoadOffenseByGuid(player->GetGUID().GetCounter(), outRecord);
}

bool AcAegisPersistence::LoadOffenseByGuid(uint32 guidLow, AegisOffenseRecord& outRecord) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, account_id, offense_count, offense_tier, punish_stage, "
        "last_cheat_type, last_offense_at, debuff_until, jail_until, ban_until, "
        "permanent_ban, last_reason, last_ban_mode, last_ban_result "
        "FROM ac_aegis_offense WHERE guid = {}",
        guidLow);

    if (!result)
        return false;

    LoadOffenseRecordFromFields(result->Fetch(), outRecord);
    return true;
}

std::vector<AegisOffenseRecord> AcAegisPersistence::LoadAllOffenses() const
{
    std::vector<AegisOffenseRecord> records;
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, account_id, offense_count, offense_tier, punish_stage, "
        "last_cheat_type, last_offense_at, debuff_until, jail_until, ban_until, "
        "permanent_ban, last_reason, last_ban_mode, last_ban_result "
        "FROM ac_aegis_offense");

    if (!result)
        return records;

    do
    {
        AegisOffenseRecord record;
        LoadOffenseRecordFromFields(result->Fetch(), record);
        records.push_back(std::move(record));
    }
    while (result->NextRow());

    return records;
}

std::vector<AegisOffenseRecord> AcAegisPersistence::LoadExpiredTempBans(int64 nowEpoch) const
{
    std::vector<AegisOffenseRecord> records;
    QueryResult result = CharacterDatabase.Query(
        "SELECT guid, account_id, offense_count, offense_tier, punish_stage, "
        "last_cheat_type, last_offense_at, debuff_until, jail_until, ban_until, "
        "permanent_ban, last_reason, last_ban_mode, last_ban_result "
        "FROM ac_aegis_offense WHERE permanent_ban = 0 AND ban_until > 0 AND ban_until <= {}",
        nowEpoch);

    if (!result)
        return records;

    do
    {
        AegisOffenseRecord record;
        LoadOffenseRecordFromFields(result->Fetch(), record);
        records.push_back(std::move(record));
    }
    while (result->NextRow());

    return records;
}

void AcAegisPersistence::SaveOffense(AegisOffenseRecord const& record) const
{
    std::string reason = EscapeForCharacterDb(record.lastReason);
    std::string banMode = EscapeForCharacterDb(record.lastBanMode);
    std::string banResult = EscapeForCharacterDb(record.lastBanResult);

    CharacterDatabase.Execute(
        "INSERT INTO ac_aegis_offense (guid, account_id, offense_count, offense_tier, "
        "punish_stage, last_cheat_type, last_offense_at, debuff_until, jail_until, "
        "ban_until, permanent_ban, last_reason, last_ban_mode, last_ban_result) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}', '{}', '{}') "
        "ON DUPLICATE KEY UPDATE "
        "account_id = VALUES(account_id), "
        "offense_count = VALUES(offense_count), "
        "offense_tier = VALUES(offense_tier), "
        "punish_stage = VALUES(punish_stage), "
        "last_cheat_type = VALUES(last_cheat_type), "
        "last_offense_at = VALUES(last_offense_at), "
        "debuff_until = VALUES(debuff_until), "
        "jail_until = VALUES(jail_until), "
        "ban_until = VALUES(ban_until), "
        "permanent_ban = VALUES(permanent_ban), "
        "last_reason = VALUES(last_reason), "
        "last_ban_mode = VALUES(last_ban_mode), "
        "last_ban_result = VALUES(last_ban_result)",
        record.guid,
        record.accountId,
        record.offenseCount,
        record.offenseTier,
        record.punishStage,
        record.lastCheatType,
        record.lastOffenseEpoch,
        record.debuffUntilEpoch,
        record.jailUntilEpoch,
        record.banUntilEpoch,
        record.permanentBan ? 1 : 0,
        reason,
        banMode,
        banResult);
}

void AcAegisPersistence::InsertEvent(Player* player, AegisEvidenceEvent const& eventData, float totalRisk) const
{
    if (!player)
        return;

    std::string tag = EscapeForCharacterDb(eventData.tag);
    std::string detail = EscapeForCharacterDb(eventData.detail);
    uint32 accountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;

    CharacterDatabase.Execute(
        "INSERT INTO ac_aegis_event (guid, account_id, map_id, zone_id, area_id, "
        "cheat_type, evidence_level, risk_delta, total_risk_after, evidence_tag, "
        "detail_text, pos_x, pos_y, pos_z) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, {}, '{}', '{}', {}, {}, {})",
        player->GetGUID().GetCounter(),
        accountId,
        player->GetMapId(),
        player->GetZoneId(),
        player->GetAreaId(),
        static_cast<uint32>(eventData.cheatType),
        static_cast<uint32>(eventData.level),
        eventData.riskDelta,
        totalRisk,
        tag,
        detail,
        player->GetPositionX(),
        player->GetPositionY(),
        player->GetPositionZ());
}

void AcAegisPersistence::DeleteOffense(uint32 guidLow) const
{
    CharacterDatabase.Execute(
        "DELETE FROM ac_aegis_offense WHERE guid = {}",
        guidLow);
}

void AcAegisPersistence::DeletePlayerData(uint32 guidLow) const
{
    CharacterDatabase.Execute(
        "DELETE FROM ac_aegis_event WHERE guid = {}",
        guidLow);
    DeleteOffense(guidLow);
}

void AcAegisPersistence::PurgeAllData() const
{
    CharacterDatabase.Execute("TRUNCATE TABLE ac_aegis_event");
    CharacterDatabase.Execute("TRUNCATE TABLE ac_aegis_offense");
}