#include "AcAegisScripts.h"

#include "AcAegisMgr.h"

#include <ctime>

#include "CharacterCache.h"
#include "Chat.h"
#include "CommandScript.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "LootMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ServerScript.h"

using namespace Acore::ChatCommands;

namespace
{
    Optional<PlayerIdentifier> TryResolvePlayer(ChatHandler* handler,
        Optional<PlayerIdentifier> player, bool requireConnected)
    {
        if (!player)
            player = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!player)
        {
            handler->SendSysMessage(LANG_PLAYER_NOT_FOUND);
            handler->SetSentErrorMessage(true);
            return std::nullopt;
        }

        if (player->IsConnected())
        {
            if (handler->HasLowerSecurity(player->GetConnectedPlayer()))
                return std::nullopt;
        }
        else
        {
            if (handler->HasLowerSecurity(nullptr, player->GetGUID()))
                return std::nullopt;

            if (requireConnected)
            {
                handler->SendSysMessage(LANG_PLAYER_NOT_FOUND);
                handler->SetSentErrorMessage(true);
                return std::nullopt;
            }
        }

        return player;
    }

    char const* CheatTypeTextZh(uint8 cheatType)
    {
        switch (static_cast<AegisCheatType>(cheatType))
        {
        case AegisCheatType::Speed:
            return "速度";
        case AegisCheatType::Teleport:
            return "瞬移";
        case AegisCheatType::NoClip:
            return "穿墙";
        case AegisCheatType::Fly:
            return "飞行";
        case AegisCheatType::Climb:
            return "爬坡";
        case AegisCheatType::Afk:
            return "挂机";
        case AegisCheatType::Mount:
            return "坐骑";
        case AegisCheatType::Time:
            return "时间倍率";
        case AegisCheatType::Control:
            return "受控位移";
        default:
            return "未知";
        }
    }

    char const* PunishStageTextZh(AegisPunishStage punishStage)
    {
        switch (punishStage)
        {
        case AegisPunishStage::Observe:
            return "观察";
        case AegisPunishStage::Debuff:
            return "减益";
        case AegisPunishStage::Jail:
            return "监禁";
        case AegisPunishStage::Kick:
            return "踢下线";
        case AegisPunishStage::TempBan:
            return "临时封禁";
        case AegisPunishStage::PermBan:
            return "永久封禁";
        default:
            return "未知";
        }
    }

    std::string FormatEpoch(int64 epoch)
    {
        if (epoch <= 0)
            return "无";

        std::time_t stamp = static_cast<std::time_t>(epoch);
        std::tm tmNow{};
#ifdef _WIN32
        localtime_s(&tmNow, &stamp);
#else
        localtime_r(&stamp, &tmNow);
#endif

        char buffer[32] = { 0 };
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tmNow);
        return buffer;
    }
}

class aegis_commandscript : public CommandScript
{
public:
    aegis_commandscript() : CommandScript("aegis_commandscript") { }

    static bool HandleHelpCommand(ChatHandler* handler)
    {
        handler->PSendSysMessage("AcAegis GM 命令帮助:");
        handler->PSendSysMessage(".aegis help | 参数: 无 | 作用: 显示本帮助、列出全部 AcAegis GM 命令。");
        handler->PSendSysMessage(".aegis global | 参数: 无 | 作用: 查看累计处罚 Top 10。");
        handler->PSendSysMessage(".aegis player [$playername] | 参数: 可选玩家名，缺省为当前目标或自己 | 作用: 查看玩家风险值、处罚阶段、累计处罚次数、封禁/监禁状态、最近证据。");
        handler->PSendSysMessage(".aegis clear [$playername] | 参数: 可选玩家名，缺省为当前目标或自己 | 作用: 清空累计处罚、移除 debuff、重置炉石并传送到释放点。");
        handler->PSendSysMessage(".aegis delete <$playername> | 参数: 玩家名 | 作用: 清理该玩家的在线上下文与数据库记录。");
        handler->PSendSysMessage(".aegis purge | 参数: 无 | 作用: 清空全部 offense/event 数据和内存状态。");
        handler->PSendSysMessage(".aegis reload | 参数: 无 | 作用: 重载 AcAegis 配置。");
        return true;
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable aegisCommandTable =
        {
            { "help", HandleHelpCommand, SEC_GAMEMASTER, Console::Yes },
            { "global", HandleGlobalCommand, SEC_GAMEMASTER, Console::Yes },
            { "player", HandlePlayerCommand, SEC_GAMEMASTER, Console::Yes },
            { "clear", HandleClearCommand, SEC_GAMEMASTER, Console::Yes },
            { "delete", HandleDeleteCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "purge", HandlePurgeCommand, SEC_ADMINISTRATOR, Console::Yes },
            { "reload", HandleReloadCommand, SEC_ADMINISTRATOR, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "aegis", aegisCommandTable },
        };

        return commandTable;
    }

    static bool HandleGlobalCommand(ChatHandler* handler)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT guid, offense_count, offense_tier, punish_stage, last_cheat_type, last_offense_at "
            "FROM ac_aegis_offense WHERE offense_count > 0 "
            "ORDER BY offense_count DESC, offense_tier DESC, last_offense_at DESC LIMIT 10");

        if (!result)
        {
            handler->PSendSysMessage("AcAegis 当前没有处罚记录。");
            return true;
        }

        handler->PSendSysMessage("AcAegis 全局高风险玩家 Top 10:");
        do
        {
            Field* fields = result->Fetch();
            uint32 guidLow = fields[0].Get<uint32>();
            std::string playerName = "<unknown>";
            sCharacterCache->GetCharacterNameByGuid(ObjectGuid::Create<HighGuid::Player>(guidLow), playerName);

            handler->PSendSysMessage(
                "玩家: {} | 累计处罚次数={} | 处罚等级={} | 当前处罚阶段={} | 最近作弊类型={} | 最近处罚时间={}",
                playerName,
                fields[1].Get<uint32>(),
                fields[2].Get<uint8>(),
                PunishStageTextZh(static_cast<AegisPunishStage>(fields[3].Get<uint8>())),
                CheatTypeTextZh(fields[4].Get<uint8>()),
                FormatEpoch(fields[5].Get<int64>()));
        } while (result->NextRow());

        return true;
    }

    static bool HandlePlayerCommand(ChatHandler* handler, Optional<PlayerIdentifier> player)
    {
        player = TryResolvePlayer(handler, player, false);
        if (!player)
            return false;

        AegisPlayerDebugSnapshot snapshot;
        if (!sAcAegisMgr->GetPlayerDebugSnapshot(player->GetGUID().GetCounter(), snapshot))
        {
            handler->PSendSysMessage("AcAegis 未找到玩家 {} 的数据。", player->GetName());
            return true;
        }

        handler->PSendSysMessage("AcAegis 玩家信息: {}", player->GetName());
        handler->PSendSysMessage("在线: {} | 风险值: {:.2f} | 样本数: {}",
            snapshot.online ? "是" : "否", snapshot.riskScore, snapshot.sampleCount);
        handler->PSendSysMessage("当前处罚阶段: {} | 累计处罚次数: {} | 处罚等级: {} | 最近作弊类型: {}",
            PunishStageTextZh(snapshot.punishStage), snapshot.offenseCount, snapshot.offenseTier,
            CheatTypeTextZh(snapshot.lastCheatType));
        handler->PSendSysMessage("最近证据: {} | 最近处罚时间: {}",
            snapshot.lastEvidenceTag.empty() ? "无" : snapshot.lastEvidenceTag,
            FormatEpoch(snapshot.lastOffenseEpoch));
        handler->PSendSysMessage("减益到期: {} | 监禁到期: {} | 封禁到期: {} | 永封: {}",
            FormatEpoch(snapshot.debuffUntilEpoch),
            FormatEpoch(snapshot.jailUntilEpoch),
            snapshot.permanentBan ? "永久" : FormatEpoch(snapshot.banUntilEpoch),
            snapshot.permanentBan ? "是" : "否");
        handler->PSendSysMessage("封禁模式: {} | 封禁结果: {}",
            snapshot.lastBanMode.empty() ? "无" : snapshot.lastBanMode,
            snapshot.lastBanResult.empty() ? "无" : snapshot.lastBanResult);
        if (!snapshot.lastBanReason.empty())
            handler->PSendSysMessage("封禁原因: {}", snapshot.lastBanReason);

        return true;
    }

    static bool HandleClearCommand(ChatHandler* handler, Optional<PlayerIdentifier> player)
    {
        player = TryResolvePlayer(handler, player, false);
        if (!player)
            return false;

        if (Player* target = player->GetConnectedPlayer())
            sAcAegisMgr->ClearPlayerOffense(target);
        else
            sAcAegisMgr->ClearPlayerOffense(player->GetGUID().GetCounter());

        handler->PSendSysMessage("AcAegis 已清空玩家 {} 的 offense，移除减益并重置至释放点。", player->GetName());
        return true;
    }

    static bool HandleDeleteCommand(ChatHandler* handler, Optional<PlayerIdentifier> player)
    {
        player = TryResolvePlayer(handler, player, false);
        if (!player)
            return false;

        sAcAegisMgr->DeletePlayerData(player->GetGUID().GetCounter());
        handler->PSendSysMessage("AcAegis 已清理玩家 {} 的在线状态和数据库记录。", player->GetName());
        return true;
    }

    static bool HandlePurgeCommand(ChatHandler* handler)
    {
        sAcAegisMgr->PurgeAllData();
        handler->PSendSysMessage("AcAegis 已清空全部 offense/event 数据和内存状态。");
        return true;
    }

    static bool HandleReloadCommand(ChatHandler* handler)
    {
        sAcAegisMgr->ReloadConfig();
        handler->PSendSysMessage("AcAegis 配置已重载。");
        return true;
    }
};

void AddSC_acaegis_commandscript()
{
    new aegis_commandscript();
}

class AcAegisPlayerScript : public PlayerScript
{
public:
    AcAegisPlayerScript() : PlayerScript("AcAegisPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_SPELL_CAST,
        PLAYERHOOK_ON_BEFORE_TELEPORT,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
        PLAYERHOOK_ON_LOOT_ITEM,
        PLAYERHOOK_ON_UPDATE_GATHERING_SKILL,
        PLAYERHOOK_ANTICHEAT_SET_CAN_FLY_BY_SERVER,
        PLAYERHOOK_ANTICHEAT_SET_UNDER_ACK_MOUNT,
        PLAYERHOOK_ANTICHEAT_SET_ROOT_ACK_UPD,
        PLAYERHOOK_ANTICHEAT_SET_JUMPING_BY_OPCODE,
        PLAYERHOOK_ANTICHEAT_UPDATE_MOVEMENT_INFO,
        PLAYERHOOK_ANTICHEAT_HANDLE_DOUBLE_JUMP,
        PLAYERHOOK_ANTICHEAT_CHECK_MOVEMENT_INFO
    })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        sAcAegisMgr->OnLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        sAcAegisMgr->OnLogout(player);
    }

    void OnPlayerSpellCast(Player* player, Spell* spell, bool skipCheck) override
    {
        sAcAegisMgr->OnSpellCast(player, spell, skipCheck);
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z, float /*o*/, uint32 /*options*/, Unit* /*target*/) override
    {
        return sAcAegisMgr->OnBeforeTeleport(player, mapId, x, y, z);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        sAcAegisMgr->OnMapChanged(player);
    }

    void OnPlayerBeforeLootMoney(Player* player, Loot* /*loot*/) override
    {
        sAcAegisMgr->OnLoot(player);
    }

    void OnPlayerLootItem(Player* player, Item* /*item*/, uint32 /*count*/, ObjectGuid /*lootGuid*/) override
    {
        sAcAegisMgr->OnLoot(player);
    }

    void OnPlayerUpdateGatheringSkill(Player* player, uint32 /*skillId*/, uint32 /*currentLevel*/, uint32 /*gray*/, uint32 /*green*/, uint32 /*yellow*/, uint32& /*gain*/) override
    {
        sAcAegisMgr->OnGatherAction(player);
    }

    void AnticheatSetCanFlybyServer(Player* player, bool apply) override
    {
        sAcAegisMgr->OnCanFlyByServer(player, apply);
    }

    void AnticheatSetUnderACKmount(Player* player) override
    {
        sAcAegisMgr->OnUnderAckMount(player);
    }

    void AnticheatSetRootACKUpd(Player* player) override
    {
        sAcAegisMgr->OnRootAckUpd(player);
    }

    void AnticheatSetJumpingbyOpcode(Player* player, bool jump) override
    {
        sAcAegisMgr->OnJumpOpcode(player, jump);
    }

    void AnticheatUpdateMovementInfo(Player* player, MovementInfo const& movementInfo) override
    {
        sAcAegisMgr->OnMovementInfoUpdate(player, movementInfo);
    }

    bool AnticheatHandleDoubleJump(Player* player, Unit* mover) override
    {
        return sAcAegisMgr->HandleDoubleJump(player, mover);
    }

    bool AnticheatCheckMovementInfo(Player* player, MovementInfo const& movementInfo, Unit* mover, bool jump) override
    {
        return sAcAegisMgr->CheckMovement(player, movementInfo, mover, jump);
    }
};

class AcAegisMovementHandlerScript : public MovementHandlerScript
{
public:
    AcAegisMovementHandlerScript() : MovementHandlerScript("AcAegisMovementHandlerScript", { MOVEMENTHOOK_ON_PLAYER_MOVE })
    {
    }

    void OnPlayerMove(Player* player, MovementInfo movementInfo, uint32 opcode) override
    {
        sAcAegisMgr->OnPlayerMove(player, movementInfo, opcode);
    }
};

class AcAegisVehicleScript : public VehicleScript
{
public:
    AcAegisVehicleScript() : VehicleScript("AcAegisVehicleScript")
    {
    }

    void OnAddPassenger(Vehicle* /*veh*/, Unit* passenger, int8 /*seatId*/) override
    {
        if (Player* player = passenger ? passenger->ToPlayer() : nullptr)
            sAcAegisMgr->OnVehicleTransition(player);
    }

    void OnRemovePassenger(Vehicle* /*veh*/, Unit* passenger) override
    {
        if (Player* player = passenger ? passenger->ToPlayer() : nullptr)
            sAcAegisMgr->OnVehicleTransition(player);
    }
};

class AcAegisTransportScript : public TransportScript
{
public:
    AcAegisTransportScript() : TransportScript("AcAegisTransportScript")
    {
    }

    void OnAddPassenger(Transport* /*transport*/, Player* player) override
    {
        sAcAegisMgr->OnTransportTransition(player);
    }

    void OnRemovePassenger(Transport* /*transport*/, Player* player) override
    {
        sAcAegisMgr->OnTransportTransition(player);
    }
};

class AcAegisWorldScript : public WorldScript
{
public:
    AcAegisWorldScript() : WorldScript("AcAegisWorldScript", { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnUpdate(uint32 diff) override
    {
        sAcAegisMgr->OnWorldUpdate(diff);
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        sAcAegisMgr->ReloadConfig();
        if (sAcAegisConfig->Get().enabled)
            LOG_INFO("module", "ACA: mod-ac-aegis enabled.");
        else
            LOG_INFO("module", "ACA: mod-ac-aegis disabled.");
    }
};

void startAcAegisScripts()
{
    new AcAegisWorldScript();
    new AcAegisPlayerScript();
    new AcAegisMovementHandlerScript();
    new AcAegisVehicleScript();
    new AcAegisTransportScript();
    AddSC_acaegis_commandscript();
}