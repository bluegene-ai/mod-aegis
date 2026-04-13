#include "AcAegisPersistence.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>

#include "AcAegisConfig.h"
#include "DatabaseEnv.h"
#include "Log.h"
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

    class AsyncEventWriter
    {
    public:
        AsyncEventWriter() : _worker(&AsyncEventWriter::Run, this) { }

        ~AsyncEventWriter()
        {
            Shutdown();
        }

        void Enqueue(AegisEventRecord const& eventRecord)
        {
            AegisConfig const& cfg = sAcAegisConfig->Get();

            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_queue.size() >= cfg.eventQueueLimit)
                {
                    ++_droppedCount;
                    return;
                }

                _queue.push_back(eventRecord);
            }

            _condition.notify_one();
        }

    private:
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
        }

        void Run()
        {
            for (;;)
            {
                std::vector<AegisEventRecord> batch;

                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    uint32 flushMs = sAcAegisConfig->Get().eventFlushIntervalMs;
                    uint32 batchSize = sAcAegisConfig->Get().eventBatchSize;
                    _condition.wait_for(lock,
                        std::chrono::milliseconds(flushMs),
                        [this, batchSize]()
                        {
                            return _stopping || _queue.size() >= batchSize;
                        });

                    if (!_queue.empty())
                        DrainBatch(batch, batchSize);

                    if (_stopping && batch.empty() && _queue.empty())
                        break;
                }

                if (!batch.empty())
                    FlushBatch(batch);

                MaybeLogDropped();
            }

            std::vector<AegisEventRecord> remaining;
            for (;;)
            {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    DrainBatch(remaining, sAcAegisConfig->Get().eventBatchSize);
                }

                if (remaining.empty())
                    break;

                FlushBatch(remaining);
            }

            MaybeLogDropped(true);
        }

        void DrainBatch(std::vector<AegisEventRecord>& outBatch, size_t limit)
        {
            outBatch.clear();
            limit = std::max<size_t>(1, limit);

            while (!_queue.empty() && outBatch.size() < limit)
            {
                outBatch.push_back(std::move(_queue.front()));
                _queue.pop_front();
            }
        }

        void FlushBatch(std::vector<AegisEventRecord> const& batch)
        {
            if (batch.empty())
                return;

            std::ostringstream sql;
            sql << "INSERT INTO ac_aegis_event (guid, account_id, map_id, zone_id, area_id, "
                   "cheat_type, evidence_level, risk_delta, total_risk_after, evidence_tag, "
                   "detail_text, pos_x, pos_y, pos_z) VALUES ";

            for (size_t index = 0; index < batch.size(); ++index)
            {
                if (index > 0)
                    sql << ',';

                std::string tag = EscapeForCharacterDb(batch[index].evidenceTag);
                std::string detail = EscapeForCharacterDb(batch[index].detailText);
                sql << '(' << batch[index].guid
                    << ',' << batch[index].accountId
                    << ',' << batch[index].mapId
                    << ',' << batch[index].zoneId
                    << ',' << batch[index].areaId
                    << ',' << static_cast<uint32>(batch[index].cheatType)
                    << ',' << static_cast<uint32>(batch[index].evidenceLevel)
                    << ',' << batch[index].riskDelta
                    << ',' << batch[index].totalRiskAfter
                    << ", '" << tag << "', '" << detail << "', "
                    << batch[index].posX
                    << ',' << batch[index].posY
                    << ',' << batch[index].posZ
                    << ')';
            }

            CharacterDatabase.DirectExecute(sql.str());
        }

        void MaybeLogDropped(bool force = false)
        {
            uint32 dropped = 0;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (!force && _droppedCount == 0)
                    return;

                auto now = std::chrono::steady_clock::now();
                if (!force && (now - _lastDropLogAt) < std::chrono::seconds(10))
                    return;

                dropped = _droppedCount;
                _droppedCount = 0;
                _lastDropLogAt = now;
            }

            if (dropped > 0)
                LOG_WARN("module", "[AcAegis] Dropped {} queued event rows due to event queue pressure", dropped);
        }

    private:
        std::mutex _mutex;
        std::condition_variable _condition;
        std::deque<AegisEventRecord> _queue;
        std::thread _worker;
        std::chrono::steady_clock::time_point _lastDropLogAt = std::chrono::steady_clock::now();
        uint32 _droppedCount = 0;
        bool _stopping = false;
    };

    AsyncEventWriter& GetAsyncEventWriter()
    {
        static AsyncEventWriter writer;
        return writer;
    }
}

void AcAegisPersistence::EnsureSchema() const
{
    static bool warned = false;
    if (!warned)
    {
        warned = true;
        LOG_WARN("module", "[AcAegis] Runtime schema auto-creation is disabled; apply SQL migrations from modules/mod-ac-aegis/data/sql/db-characters/base/001_ac_aegis_tables.sql");
    }
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

void AcAegisPersistence::QueueEvent(AegisEventRecord const& eventRecord) const
{
    GetAsyncEventWriter().Enqueue(eventRecord);
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