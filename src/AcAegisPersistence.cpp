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

    struct QueuedEventRecord
    {
        uint64 sequence = 0;
        AegisEventRecord eventRecord;
    };

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
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_queue.size() >= sAcAegisConfig->GetEventQueueLimit())
                {
                    ++_droppedCount;
                    return;
                }

                QueuedEventRecord queuedEvent;
                queuedEvent.sequence = ++_nextSequence;
                queuedEvent.eventRecord = eventRecord;
                _queue.push_back(std::move(queuedEvent));
            }

            _condition.notify_one();
        }

        void Barrier()
        {
            std::unique_lock<std::mutex> lock(_mutex);
            uint64 targetSequence = _nextSequence;
            if (targetSequence == 0 || _completedSequence >= targetSequence)
                return;

            if (targetSequence > _flushTargetSequence)
                _flushTargetSequence = targetSequence;

            _condition.notify_one();
            _condition.wait(lock, [this, targetSequence]()
            {
                return _completedSequence >= targetSequence;
            });
        }

        void DropQueuedForGuid(uint32 guidLow)
        {
            std::lock_guard<std::mutex> lock(_mutex);
            for (auto it = _queue.begin(); it != _queue.end();)
            {
                if (it->eventRecord.guid == guidLow)
                    it = _queue.erase(it);
                else
                    ++it;
            }
        }

        void DropAllQueued()
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _queue.clear();
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
                std::vector<QueuedEventRecord> batch;

                {
                    std::unique_lock<std::mutex> lock(_mutex);
                    uint32 flushMs = sAcAegisConfig->GetEventFlushIntervalMs();
                    uint32 batchSize = sAcAegisConfig->GetEventBatchSize();
                    _condition.wait_for(lock,
                        std::chrono::milliseconds(flushMs),
                        [this, batchSize]()
                        {
                            return _stopping || _queue.size() >= batchSize ||
                                (_flushTargetSequence != 0 && !_queue.empty());
                        });

                    if (!_queue.empty())
                        DrainBatch(batch, batchSize);

                    if (_stopping && batch.empty() && _queue.empty())
                        break;
                }

                if (!batch.empty())
                {
                    FlushBatch(batch);
                    CompleteBatch(batch);
                }

                MaybeLogDropped();
            }

            std::vector<QueuedEventRecord> remaining;
            for (;;)
            {
                {
                    std::lock_guard<std::mutex> lock(_mutex);
                    DrainBatch(remaining,
                        sAcAegisConfig->GetEventBatchSize());
                }

                if (remaining.empty())
                    break;

                FlushBatch(remaining);
                CompleteBatch(remaining);
            }

            MaybeLogDropped(true);
        }

        void DrainBatch(std::vector<QueuedEventRecord>& outBatch, size_t limit)
        {
            outBatch.clear();
            limit = std::max<size_t>(1, limit);

            while (!_queue.empty() && outBatch.size() < limit)
            {
                outBatch.push_back(std::move(_queue.front()));
                _queue.pop_front();
            }
        }

        void FlushBatch(std::vector<QueuedEventRecord> const& batch)
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

                AegisEventRecord const& eventRecord = batch[index].eventRecord;
                std::string tag = EscapeForCharacterDb(eventRecord.evidenceTag);
                std::string detail = EscapeForCharacterDb(eventRecord.detailText);
                sql << '(' << eventRecord.guid
                    << ',' << eventRecord.accountId
                    << ',' << eventRecord.mapId
                    << ',' << eventRecord.zoneId
                    << ',' << eventRecord.areaId
                    << ',' << static_cast<uint32>(eventRecord.cheatType)
                    << ',' << static_cast<uint32>(eventRecord.evidenceLevel)
                    << ',' << eventRecord.riskDelta
                    << ',' << eventRecord.totalRiskAfter
                    << ", '" << tag << "', '" << detail << "', "
                    << eventRecord.posX
                    << ',' << eventRecord.posY
                    << ',' << eventRecord.posZ
                    << ')';
            }

            CharacterDatabase.DirectExecute(sql.str());
        }

        void CompleteBatch(std::vector<QueuedEventRecord> const& batch)
        {
            if (batch.empty())
                return;

            std::lock_guard<std::mutex> lock(_mutex);
            _completedSequence = std::max(_completedSequence,
                batch.back().sequence);
            if (_flushTargetSequence != 0 &&
                _completedSequence >= _flushTargetSequence)
                _flushTargetSequence = 0;
            _condition.notify_all();
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
        std::deque<QueuedEventRecord> _queue;
        std::thread _worker;
        std::chrono::steady_clock::time_point _lastDropLogAt = std::chrono::steady_clock::now();
        uint32 _droppedCount = 0;
        uint64 _nextSequence = 0;
        uint64 _completedSequence = 0;
        uint64 _flushTargetSequence = 0;
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
    GetAsyncEventWriter().Barrier();
    GetAsyncEventWriter().DropQueuedForGuid(guidLow);
    CharacterDatabase.Execute(
        "DELETE FROM ac_aegis_event WHERE guid = {}",
        guidLow);
    DeleteOffense(guidLow);
}

void AcAegisPersistence::PurgeAllData() const
{
    GetAsyncEventWriter().Barrier();
    GetAsyncEventWriter().DropAllQueued();
    CharacterDatabase.Execute("TRUNCATE TABLE ac_aegis_event");
    CharacterDatabase.Execute("TRUNCATE TABLE ac_aegis_offense");
}