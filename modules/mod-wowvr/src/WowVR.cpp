/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * mod-wowvr: relays WowVR clients' VR poses (head, hands, fingers, lean, pointer, and full body
 * tracker joints) to the players who can see them. The server checks and forwards poses, it never interprets them.
 *
 * Threading: CMSG_WOWVR_* are PROCESS_THREADUNSAFE, so CanPacketReceive runs in World::UpdateSessions
 * while no map updates. OnPlayerUpdate runs in map threads; the players it looks at (the sender and
 * those who see it) are in the sender's map, so in the same thread. sMutex guards the session store,
 * which every thread shares.
 */

#include "Chat.h"
#include "ChatCommand.h"
#include "CommandScript.h"
#include "Config.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "ScriptMgr.h"
#include "ServerScript.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldPacket.h"
#include "WorldScript.h"
#include "WorldSession.h"
#include "WowVRProtocol.h"
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    struct WowVRConfig
    {
        bool Enable = true;
        uint16 SendRate = 20;
        uint16 MaxReceiveRate = 40;
        float PoseRange = 60.0f;
        uint16 PoseTimeoutMs = 2000;
        float MaxOffset = 8.0f;

        // Derived
        uint32 SendIntervalMs = 50;
        uint32 MinReceiveIntervalMs = 25;
        float PoseRangeSq = 3600.0f;
        int32 MaxOffsetMilli = 8000;
    };

    // Written only on config load (world thread, no map update running), read everywhere else.
    WowVRConfig sConfig;

    // Counts events in whole one-second windows; Get() is the count of the last full second.
    class RateCounter
    {
    public:
        void Add(uint32 now)
        {
            Roll(now);
            ++_current;
        }

        uint32 Get(uint32 now)
        {
            Roll(now);
            return _previous;
        }

    private:
        void Roll(uint32 now)
        {
            uint32 const elapsed = getMSTimeDiff(_windowStart, now);
            if (elapsed < 1000)
                return;

            _previous = elapsed < 2000 ? _current : 0;
            _current = 0;
            _windowStart = now;
        }

        uint32 _windowStart = 0;
        uint32 _current = 0;
        uint32 _previous = 0;
    };

    struct VRSession
    {
        ObjectGuid Guid;
        uint16 Version = 0; // 0 until the client says hello
        uint32 Capabilities = 0;
        uint8 State = 0;

        std::vector<uint8> Pose; // newest pose payload as it arrived, empty when none
        uint32 PoseReceivedMs = 0;
        bool PoseUnsent = false;

        uint32 LastSendMs = 0;
        uint32 LastPassMs = 0;
        bool HasSent = false;

        // Players who currently get this one's poses and already have its state.
        std::unordered_set<ObjectGuid> Observers;

        RateCounter Received;
        RateCounter Sent;

        bool Negotiated() const { return Version != 0; }
        bool SendsPoses() const { return Negotiated() && (Capabilities & WowVR::CAP_SENDS_POSES); }
        bool WantsPoses() const { return Negotiated() && (Capabilities & WowVR::CAP_WANTS_POSES); }
        bool InVR() const { return State & WowVR::STATE_IN_VR; }

        bool PoseFresh(uint32 now) const
        {
            return !Pose.empty() && getMSTimeDiff(PoseReceivedMs, now) < sConfig.PoseTimeoutMs;
        }

        void ForgetPose()
        {
            Pose.clear();
            PoseUnsent = false;
        }
    };

    std::mutex sMutex;
    std::unordered_map<uint32 /*accountId*/, VRSession> sSessions;

    // Callers hold sMutex.
    VRSession* FindSession(WorldSession const* session)
    {
        auto itr = sSessions.find(session->GetAccountId());
        return itr != sSessions.end() ? &itr->second : nullptr;
    }

    // Calls worker(observer) for every player who should get sender's poses: sees it, is within
    // WowVR.PoseRange and asked for poses. Callers hold sMutex.
    template<typename Worker>
    void ForEachObserver(Player* sender, Worker&& worker)
    {
        sender->DoForAllVisiblePlayers([&](Player* observer)
        {
            if (observer == sender || !observer->IsInWorld())
                return;

            if (sender->GetExactDistSq(observer) > sConfig.PoseRangeSq)
                return;

            VRSession const* observerSession = FindSession(observer->GetSession());
            if (!observerSession || !observerSession->WantsPoses())
                return;

            worker(observer);
        });
    }

    void BuildStatePacket(WorldPacket& data, ObjectGuid guid, uint8 state)
    {
        data.Initialize(SMSG_WOWVR_STATE, 9 + 1);
        data << guid.WriteAsPacked();
        data << uint8(state);
    }

    void BuildPosePacket(WorldPacket& data, ObjectGuid guid, std::vector<uint8> const& pose)
    {
        data.Initialize(SMSG_WOWVR_POSE, 9 + pose.size());
        data << guid.WriteAsPacked();
        data.append(pose.data(), pose.size());
    }

    // Sends observer what's known of sender: its state, then its pose if newer than the timeout.
    void SendSurroundings(Player* observer, VRSession const& sender, uint32 now)
    {
        WorldPacket data;
        BuildStatePacket(data, sender.Guid, sender.State);
        observer->SendDirectMessage(&data);

        if (!sender.PoseFresh(now))
            return;

        BuildPosePacket(data, sender.Guid, sender.Pose);
        observer->SendDirectMessage(&data);
    }

    void HandleHello(Player* player, WorldPacket const& packet)
    {
        if (packet.size() != WowVR::CMSG_HELLO_SIZE)
        {
            LOG_DEBUG("module.wowvr", "{}: hello of {} bytes dropped", player->GetName(), packet.size());
            return;
        }

        uint8 const* raw = packet.contents();
        uint16 const version = WowVR::ReadUInt16(raw);
        uint32 const capabilities = WowVR::ReadUInt32(raw + 2);
        if (!version)
        {
            LOG_DEBUG("module.wowvr", "{}: hello with version 0 dropped", player->GetName());
            return;
        }

        uint32 const now = getMSTime();

        std::lock_guard<std::mutex> lock(sMutex);

        VRSession& session = sSessions[player->GetSession()->GetAccountId()];
        session.Guid = player->GetGUID();
        session.Version = std::min(version, WowVR::PROTOCOL_VERSION);
        session.Capabilities = capabilities & WowVR::CAP_MASK_V1;
        if (!(session.Capabilities & WowVR::CAP_SENDS_POSES))
        {
            session.State = 0;
            session.ForgetPose();
            session.Observers.clear();
        }

        WorldPacket data(SMSG_WOWVR_HELLO, 2 + 4 + 2 + 2 + 2);
        data << uint16(session.Version);
        data << uint32(session.Capabilities);
        data << uint16(sConfig.SendRate);
        data << uint16(sConfig.MaxReceiveRate);
        data << uint16(sConfig.PoseTimeoutMs);
        player->SendDirectMessage(&data);

        LOG_DEBUG("module.wowvr", "{}: negotiated version {}, capabilities {:#x}", player->GetName(),
            session.Version, session.Capabilities);

        if (!session.WantsPoses())
            return;

        // Section 2.3: what's known of every VR player this one can already see.
        player->DoForAllVisibleWorldObjects([&](WorldObject* object)
        {
            Player* other = object->ToPlayer();
            if (!other || other == player || !other->IsInWorld())
                return;

            if (other->GetExactDistSq(player) > sConfig.PoseRangeSq)
                return;

            VRSession* otherSession = FindSession(other->GetSession());
            if (!otherSession || !otherSession->SendsPoses() || !otherSession->InVR())
                return;

            SendSurroundings(player, *otherSession, now);
            otherSession->Observers.insert(player->GetGUID());
        });
    }

    void HandleState(Player* player, WorldPacket const& packet)
    {
        if (packet.size() != WowVR::CMSG_STATE_SIZE)
            return;

        std::lock_guard<std::mutex> lock(sMutex);

        VRSession* session = FindSession(player->GetSession());
        if (!session || !session->SendsPoses())
            return;

        session->State = packet.contents()[0];
        if (!session->InVR())
            session->ForgetPose();

        WorldPacket data;
        BuildStatePacket(data, session->Guid, session->State);

        std::unordered_set<ObjectGuid> observers;
        ForEachObserver(player, [&](Player* observer)
        {
            observer->SendDirectMessage(&data);
            observers.insert(observer->GetGUID());
        });
        session->Observers = std::move(observers);
    }

    void HandlePose(Player* player, WorldPacket const& packet)
    {
        uint32 const now = getMSTime();

        std::lock_guard<std::mutex> lock(sMutex);

        VRSession* session = FindSession(player->GetSession());
        if (!session || !session->SendsPoses())
            return;

        WowVR::PoseCheck const check = WowVR::CheckPose(packet.contents(), packet.size(), sConfig.MaxOffsetMilli);
        if (check != WowVR::PoseCheck::Ok)
        {
            LOG_DEBUG("module.wowvr", "{}: pose of {} bytes dropped ({})", player->GetName(), packet.size(),
                check == WowVR::PoseCheck::BadLength ? "length" :
                check == WowVR::PoseCheck::UnknownPart ? "unknown part" : "out of bounds");
            return;
        }

        if (!session->Pose.empty() && getMSTimeDiff(session->PoseReceivedMs, now) < sConfig.MinReceiveIntervalMs)
            return;

        session->Pose.assign(packet.contents(), packet.contents() + packet.size());
        session->PoseReceivedMs = now;
        session->PoseUnsent = true;
        session->State = session->Pose[WowVR::POSE_STATE_OFFSET];
        session->Received.Add(now);
    }

    // Sends the sender's newest pose out at WowVR.SendRate (section 3.4), and its state and pose to
    // players who came into view since the last pass. Callers hold sMutex.
    void UpdateSender(Player* player, VRSession& session, uint32 now)
    {
        bool const inVR = session.InVR();
        if (!inVR && !session.PoseUnsent)
            return;

        bool const sendDue = !session.HasSent || getMSTimeDiff(session.LastSendMs, now) >= sConfig.SendIntervalMs;
        bool const sendPose = session.PoseUnsent && sendDue;
        bool const passDue = getMSTimeDiff(session.LastPassMs, now) >= sConfig.SendIntervalMs;
        if (!sendPose && !(inVR && passDue))
            return;

        session.LastPassMs = now;

        WorldPacket poseData;
        if (sendPose)
            BuildPosePacket(poseData, session.Guid, session.Pose);

        std::unordered_set<ObjectGuid> observers;
        ForEachObserver(player, [&](Player* observer)
        {
            ObjectGuid const guid = observer->GetGUID();
            observers.insert(guid);

            if (inVR && !session.Observers.count(guid))
                SendSurroundings(observer, session, now);
            else if (sendPose)
                observer->SendDirectMessage(&poseData);
        });
        session.Observers = std::move(observers);

        if (sendPose)
        {
            session.PoseUnsent = false;
            session.LastSendMs = now;
            session.HasSent = true;
            session.Sent.Add(now);
        }
    }

    std::string CapabilitiesToString(uint32 capabilities)
    {
        std::string out;
        if (capabilities & WowVR::CAP_SENDS_POSES)
            out += "sends";
        if (capabilities & WowVR::CAP_WANTS_POSES)
            out += out.empty() ? "wants" : "+wants";
        return out.empty() ? "none" : out;
    }
}

class mod_wowvr_config : public WorldScript
{
public:
    mod_wowvr_config() : WorldScript("mod_wowvr_config", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        auto getUInt16 = [](char const* name, uint32 def, uint32 min, uint32 max)
        {
            return static_cast<uint16>(std::clamp(sConfigMgr->GetOption<uint32>(name, def), min, max));
        };

        WowVRConfig config;
        config.Enable = sConfigMgr->GetOption<bool>("WowVR.Enable", true);
        config.SendRate = getUInt16("WowVR.SendRate", 20, 1, 1000);
        config.MaxReceiveRate = getUInt16("WowVR.MaxReceiveRate", 40, 1, 1000);
        config.PoseRange = std::max(0.0f, sConfigMgr->GetOption<float>("WowVR.PoseRange", 60.0f));
        config.PoseTimeoutMs = getUInt16("WowVR.PoseTimeoutMs", 2000, 1, 65535);
        // Positions are int16 thousandths of a yard, so nothing beyond 32.767 yards can be sent anyway.
        config.MaxOffset = std::clamp(sConfigMgr->GetOption<float>("WowVR.MaxOffset", 8.0f), 0.0f, 32.767f);

        config.SendIntervalMs = 1000 / config.SendRate;
        config.MinReceiveIntervalMs = 1000 / config.MaxReceiveRate;
        config.PoseRangeSq = config.PoseRange * config.PoseRange;
        config.MaxOffsetMilli = static_cast<int32>(config.MaxOffset * 1000.0f);

        sConfig = config;
    }
};

class mod_wowvr_packets : public ServerScript
{
public:
    mod_wowvr_packets() : ServerScript("mod_wowvr_packets", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

    bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
    {
        uint16 const opcode = packet.GetOpcode();
        if (opcode != CMSG_WOWVR_HELLO && opcode != CMSG_WOWVR_STATE && opcode != CMSG_WOWVR_POSE)
            return true;

        if (!sConfig.Enable)
            return false;

        // The opcodes are STATUS_LOGGEDIN: the hook only sees them for a player in the world.
        Player* player = session->GetPlayer();
        if (!player || !player->IsInWorld())
            return false;

        switch (opcode)
        {
            case CMSG_WOWVR_HELLO:
                HandleHello(player, packet);
                break;
            case CMSG_WOWVR_STATE:
                HandleState(player, packet);
                break;
            case CMSG_WOWVR_POSE:
                HandlePose(player, packet);
                break;
            default:
                break;
        }

        return false;
    }
};

class mod_wowvr_player : public PlayerScript
{
public:
    mod_wowvr_player() : PlayerScript("mod_wowvr_player", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_UPDATE,
        PLAYERHOOK_ON_MAP_CHANGED
    }) { }

    // Section 2.1: an addon whisper from oneself, "WOWVR\t<version>".
    void OnPlayerLogin(Player* player) override
    {
        if (!sConfig.Enable)
            return;

        std::string const text = Acore::StringFormat("{}\t{}", WowVR::ANNOUNCE_PREFIX, WowVR::PROTOCOL_VERSION);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, text);
        player->SendDirectMessage(&data);
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sSessions.erase(player->GetSession()->GetAccountId());
    }

    void OnPlayerUpdate(Player* player, uint32 /*diff*/) override
    {
        if (!sConfig.Enable)
            return;

        uint32 const now = getMSTime();

        std::lock_guard<std::mutex> lock(sMutex);

        VRSession* session = FindSession(player->GetSession());
        if (!session || !session->SendsPoses())
            return;

        UpdateSender(player, *session, now);
    }

    // The negotiation survives; the players in the new map get the state and pose on the next pass.
    void OnPlayerMapChanged(Player* player) override
    {
        std::lock_guard<std::mutex> lock(sMutex);

        if (VRSession* session = FindSession(player->GetSession()))
        {
            session->Guid = player->GetGUID();
            session->Observers.clear();
        }
    }
};

class mod_wowvr_commands : public CommandScript
{
public:
    mod_wowvr_commands() : CommandScript("mod_wowvr_commands") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable wowvrCommandTable =
        {
            { "status", HandleStatusCommand, SEC_GAMEMASTER, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "wowvr", wowvrCommandTable }
        };

        return commandTable;
    }

    static bool HandleStatusCommand(ChatHandler* handler)
    {
        uint32 const now = getMSTime();

        handler->PSendSysMessage(
            "WowVR {}: send rate {}/s, max receive rate {}/s, range {} yd, timeout {} ms, max offset {} yd",
            sConfig.Enable ? "on" : "off", sConfig.SendRate, sConfig.MaxReceiveRate, sConfig.PoseRange,
            sConfig.PoseTimeoutMs, sConfig.MaxOffset);

        std::lock_guard<std::mutex> lock(sMutex);

        uint32 count = 0;
        for (auto& [accountId, session] : sSessions)
        {
            if (!session.Negotiated())
                continue;

            Player* player = ObjectAccessor::FindConnectedPlayer(session.Guid);
            handler->PSendSysMessage(
                "  {} (account {}): v{}, {}, state {:#04x}{}, poses received {}/s, sent {}/s, {} observers",
                player ? player->GetName() : session.Guid.ToString(), accountId, session.Version,
                CapabilitiesToString(session.Capabilities), session.State, session.InVR() ? " (in VR)" : "",
                session.Received.Get(now), session.Sent.Get(now), session.Observers.size());
            ++count;
        }

        handler->PSendSysMessage("{} session(s) negotiated", count);
        return true;
    }
};

void AddWowVRScripts()
{
    new mod_wowvr_config();
    new mod_wowvr_packets();
    new mod_wowvr_player();
    new mod_wowvr_commands();
}
