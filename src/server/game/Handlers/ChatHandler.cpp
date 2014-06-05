/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Common.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "GuildMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"

#include "CellImpl.h"
#include "Chat.h"
#include "ChannelMgr.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Guild.h"
#include "Language.h"
#include "Log.h"
#include "Opcodes.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "Util.h"
#include "ScriptMgr.h"
#include "AccountMgr.h"

bool WorldSession::processChatmessageFurtherAfterSecurityChecks(std::string& msg, uint32 lang)
{
    if (lang != LANG_ADDON)
    {
        // strip invisible characters for non-addon messages
        if (sWorld->getBoolConfig(CONFIG_CHAT_FAKE_MESSAGE_PREVENTING))
            stripLineInvisibleChars(msg);

        if (sWorld->getIntConfig(CONFIG_CHAT_STRICT_LINK_CHECKING_SEVERITY) && AccountMgr::IsPlayerAccount(GetSecurity())
                && !ChatHandler(this).isValidChatMessage(msg.c_str()))
        {
            sLog->outError(LOG_FILTER_NETWORKIO, "Player %s (GUID: %u) sent a chatmessage with an invalid link: %s", GetPlayer()->GetName(),
                    GetPlayer()->GetGUIDLow(), msg.c_str());
            if (sWorld->getIntConfig(CONFIG_CHAT_STRICT_LINK_CHECKING_KICK))
                KickPlayer();
            return false;
        }
    }

    return true;
}

void WorldSession::HandleMessagechatOpcode(WorldPacket& p_RecvData)
{
    uint32 l_Type       = 0;
    uint32 l_Language   = 0;

    switch (p_RecvData.GetOpcode())
    {
        case CMSG_CHAT_MESSAGE_SAY:
            l_Type = CHAT_MSG_SAY;
            break;

        case CMSG_CHAT_MESSAGE_YELL:
            l_Type = CHAT_MSG_YELL;
            break;

        case CMSG_CHAT_MESSAGE_CHANNEL:
            l_Type = CHAT_MSG_CHANNEL;
            break;

        case CMSG_CHAT_MESSAGE_WHISPER:
            l_Type = CHAT_MSG_WHISPER;
            break;

        case CMSG_CHAT_MESSAGE_GUILD:
            l_Type = CHAT_MSG_GUILD;
            break;

        case CMSG_CHAT_MESSAGE_OFFICER:
            l_Type = CHAT_MSG_OFFICER;
            break;

        case CMSG_CHAT_MESSAGE_AFK:
            l_Type = CHAT_MSG_AFK;
            break;

        case CMSG_CHAT_MESSAGE_DND:
            l_Type = CHAT_MSG_DND;
            break;

        case CMSG_CHAT_MESSAGE_EMOTE:
            l_Type = CHAT_MSG_EMOTE;
            break;

        case CMSG_CHAT_MESSAGE_PARTY:
            l_Type = CHAT_MSG_PARTY;
            break;

        case CMSG_CHAT_MESSAGE_RAID:
            l_Type = CHAT_MSG_RAID;
            break;

        case CMSG_CHAT_MESSAGE_RAID_WARNING:
            l_Type = CHAT_MSG_RAID_WARNING;
            break;

        default:
            sLog->outError(LOG_FILTER_NETWORKIO, "HandleMessagechatOpcode : Unknown chat opcode (%u)", p_RecvData.GetOpcode());
            p_RecvData.hexlike();
            return;

    }

    if (l_Type >= MAX_CHAT_MSG_TYPE)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "CHAT: Wrong message type received: %u", l_Type);
        p_RecvData.rfinish();
        return;
    }

    Player * l_Sender = GetPlayer();

    //sLog->outDebug(LOG_FILTER_GENERAL, "CHAT: packet received. type %u, lang %u", type, lang);

    /// no language sent with emote packet.
    if (l_Type != CHAT_MSG_EMOTE && l_Type != CHAT_MSG_AFK && l_Type != CHAT_MSG_DND)
    {
        p_RecvData >> l_Language;

        /// prevent talking at unknown language (cheating)
        LanguageDesc const * l_LanguageDesc = GetLanguageDescByID(l_Language);

        if (!l_LanguageDesc)
        {
            SendNotification(LANG_UNKNOWN_LANGUAGE);
            p_RecvData.rfinish();
            return;
        }

        if (l_LanguageDesc->skill_id != 0 && !l_Sender->HasSkill(l_LanguageDesc->skill_id))
        {
            /// also check SPELL_AURA_COMPREHEND_LANGUAGE (client offers option to speak in that language)
            Unit::AuraEffectList const& l_LanguageAuras = l_Sender->GetAuraEffectsByType(SPELL_AURA_COMPREHEND_LANGUAGE);
            bool l_AuraIsFound = false;

            for (Unit::AuraEffectList::const_iterator l_I = l_LanguageAuras.begin(); l_I != l_LanguageAuras.end(); ++l_I)
            {
                if ((*l_I)->GetMiscValue() == int32(l_Language))
                {
                    l_AuraIsFound = true;
                    break;
                }
            }

            if (!l_AuraIsFound)
            {
                SendNotification(LANG_NOT_LEARNED_LANGUAGE);
                p_RecvData.rfinish();
                return;
            }
        }

        if (l_Language == LANG_ADDON)
        {
            if (sWorld->getBoolConfig(CONFIG_CHATLOG_ADDON))
            {
                uint32 l_MessageLenght = 0;
                std::string l_Message = "";
                
                l_MessageLenght = p_RecvData.ReadBits(8);
                p_RecvData >> l_Message;

                if (l_Message.empty())
                    return;

                sScriptMgr->OnPlayerChat(l_Sender, uint32(CHAT_MSG_ADDON), l_Language, l_Message);
            }

            if (l_Type == CHAT_MSG_WHISPER)
            {
                if (!l_Sender->UpdatePmChatTime())
                {
                    SendNotification("You have sent too many whisper messages in a short time interval.");
                    p_RecvData.rfinish();
                    return;
                }
            }

            // Disabled addon channel?
            if (!sWorld->getBoolConfig(CONFIG_ADDON_CHANNEL))
                return;
        }
        // LANG_ADDON should not be changed nor be affected by flood control
        else
        {
            // send in universal language if player in .gm on mode (ignore spell effects)
            if (l_Sender->isGameMaster())
                l_Language = LANG_UNIVERSAL;
            else
            {
                // send in universal language in two side iteration allowed mode
                if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT))
                    l_Language = LANG_UNIVERSAL;
                else
                {
                    switch (l_Type)
                    {
                        case CHAT_MSG_PARTY:
                        case CHAT_MSG_PARTY_LEADER:
                        case CHAT_MSG_RAID:
                        case CHAT_MSG_RAID_LEADER:
                        case CHAT_MSG_RAID_WARNING:
                            // allow two side chat at group channel if two side group allowed
                            if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GROUP))
                                l_Language = LANG_UNIVERSAL;
                            break;
                        case CHAT_MSG_GUILD:
                        case CHAT_MSG_OFFICER:
                            // allow two side chat at guild channel if two side guild allowed
                            if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD))
                                l_Language = LANG_UNIVERSAL;
                            break;
                    }
                }

                // but overwrite it by SPELL_AURA_MOD_LANGUAGE auras (only single case used)
                Unit::AuraEffectList const& ModLangAuras = l_Sender->GetAuraEffectsByType(SPELL_AURA_MOD_LANGUAGE);
                if (!ModLangAuras.empty())
                    l_Language = ModLangAuras.front()->GetMiscValue();
            }

            if (!l_Sender->CanSpeak())
            {
                std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
                SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
                p_RecvData.rfinish(); // Prevent warnings
                return;
            }
        }
    }
    else
        l_Language = LANG_UNIVERSAL;

    /// Check if silenced http://wowhead.com/spell=1852
    if (l_Sender->HasAura(1852) && l_Type != CHAT_MSG_WHISPER)
    {
        p_RecvData.rfinish();
        SendNotification(GetTrinityString(LANG_GM_SILENCE), l_Sender->GetName());
        return;
    }

    uint32 l_TextLenght     = 0;
    uint32 l_ReceiverLength = 0;
    uint32 l_ChannelLength  = 0;

    std::string l_ReceiverName;
    std::string l_ChannelName;
    std::string l_Text;

    bool l_IgnoreChecks = false;

    switch (l_Type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_YELL:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_WARNING:
            l_TextLenght    = p_RecvData.ReadBits(8);
            l_Text          = p_RecvData.ReadString(l_TextLenght);
            break;

        case CHAT_MSG_WHISPER:
            l_ReceiverLength    = p_RecvData.ReadBits(9);
            l_TextLenght        = p_RecvData.ReadBits(8);
            l_ReceiverName      = p_RecvData.ReadString(l_ReceiverLength);
            l_Text              = p_RecvData.ReadString(l_TextLenght);
            break;

        case CHAT_MSG_CHANNEL:
            l_ChannelLength     = p_RecvData.ReadBits(9);
            l_TextLenght        = p_RecvData.ReadBits(8);
            l_ChannelName       = p_RecvData.ReadString(l_ChannelLength);
            l_Text              = p_RecvData.ReadString(l_TextLenght);
            break;

        case CHAT_MSG_AFK:
        case CHAT_MSG_DND:
            l_TextLenght    = p_RecvData.ReadBits(8);
            l_Text          = p_RecvData.ReadString(l_TextLenght);

            l_IgnoreChecks = true;
            break;

    }

    if (!l_IgnoreChecks)
    {
        if (l_Text.empty())
            return;

        if (ChatHandler(this).ParseCommands(l_Text.c_str()) > 0)
            return;

        if (!processChatmessageFurtherAfterSecurityChecks(l_Text, l_Language))
            return;

        if (l_Text.empty())
            return;
    }

    switch (l_Type)
    {
        case CHAT_MSG_SAY:
        case CHAT_MSG_EMOTE:
        case CHAT_MSG_YELL:
        {
            if (l_Sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ))
            {
                SendNotification(GetTrinityString(LANG_SAY_REQ), sWorld->getIntConfig(CONFIG_CHAT_SAY_LEVEL_REQ));
                return;
            }

            if (l_Type == CHAT_MSG_SAY)
                l_Sender->Say(l_Text, l_Language);
            else if (l_Type == CHAT_MSG_EMOTE)
                l_Sender->TextEmote(l_Text);
            else if (l_Type == CHAT_MSG_YELL)
                l_Sender->Yell(l_Text, l_Language);

            break;
        }
        case CHAT_MSG_WHISPER:
        {
            if (l_Sender->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ))
            {
                SendNotification(GetTrinityString(LANG_WHISPER_REQ), sWorld->getIntConfig(CONFIG_CHAT_WHISPER_LEVEL_REQ));
                return;
            }

            if (!normalizePlayerName(l_ReceiverName))
            {
                SendPlayerNotFoundNotice(l_ReceiverName);
                break;
            }

            Player* l_Receiver = sObjectAccessor->FindPlayerByName(l_ReceiverName.c_str());

            bool l_SenderIsPlayer   = AccountMgr::IsPlayerAccount(GetSecurity());
            bool l_ReceiverIsPlayer = AccountMgr::IsPlayerAccount(l_Receiver ? l_Receiver->GetSession()->GetSecurity() : SEC_PLAYER);

            if (!l_Receiver || (l_SenderIsPlayer && !l_ReceiverIsPlayer && !l_Receiver->IsAcceptWhispers() && !l_Receiver->IsInWhisperWhiteList(l_Sender->GetGUID())))
            {
                SendPlayerNotFoundNotice(l_ReceiverName);
                return;
            }

            if (!sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_CHAT) && l_SenderIsPlayer && l_ReceiverIsPlayer)
            {
                if (GetPlayer()->GetTeam() != l_Receiver->GetTeam())
                {
                    SendWrongFactionNotice();
                    return;
                }
            }

            /// Check if silenced http://wowhead.com/spell=1852
            if (GetPlayer()->HasAura(1852) && !l_Receiver->isGameMaster())
            {
                SendNotification(GetTrinityString(LANG_GM_SILENCE), GetPlayer()->GetName());
                return;
            }

            /// If player is a Gamemaster and doesn't accept whisper, we auto-whitelist every player that the Gamemaster is talking to
            if (!l_SenderIsPlayer && !l_Sender->IsAcceptWhispers() && !l_Sender->IsInWhisperWhiteList(l_Receiver->GetGUID()))
                l_Sender->AddWhisperWhiteList(l_Receiver->GetGUID());

            GetPlayer()->Whisper(l_Text, l_Language, l_Receiver->GetGUID());

            break;
        }
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        {
            /// if player is in battleground, he cannot say to battleground members by /p
            Group * l_Group = GetPlayer()->GetOriginalGroup();

            if (!l_Group)
            {
                l_Group = m_Player->GetGroup();
                if (!l_Group || l_Group->isBGGroup())
                    return;
            }

            if (l_Group->IsLeader(GetPlayer()->GetGUID()))
                l_Type = CHAT_MSG_PARTY_LEADER;

            sScriptMgr->OnPlayerChat(GetPlayer(), l_Type, l_Language, l_Text, l_Group);

            WorldPacket l_Data;

            ChatHandler::FillMessageData(&l_Data, this, uint8(l_Type), l_Language, NULL, 0, l_Text.c_str(), NULL);
            l_Group->BroadcastPacket(&l_Data, false, l_Group->GetMemberGroup(GetPlayer()->GetGUID()));

            break;
        }
        case CHAT_MSG_GUILD:
        {
            if (GetPlayer()->GetGuildId())
            {
                if (Guild * l_Guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
                {
                    sScriptMgr->OnPlayerChat(GetPlayer(), l_Type, l_Language, l_Text, l_Guild);

                    l_Guild->BroadcastToGuild(this, false, l_Text, l_Language == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
                }
            }

            break;
        }
        case CHAT_MSG_OFFICER:
        {
            if (GetPlayer()->GetGuildId())
            {
                if (Guild * l_Guild = sGuildMgr->GetGuildById(GetPlayer()->GetGuildId()))
                {
                    sScriptMgr->OnPlayerChat(GetPlayer(), l_Type, l_Language, l_Text, l_Guild);

                    l_Guild->BroadcastToGuild(this, true, l_Text, l_Language == LANG_ADDON ? LANG_ADDON : LANG_UNIVERSAL);
                }
            }

            break;
        }
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
        {
            /// if player is in battleground, he cannot say to battleground members by /ra
            Group * l_Group = GetPlayer()->GetOriginalGroup();

            if (!l_Group)
            {
                l_Group = GetPlayer()->GetGroup();
                if (!l_Group || l_Group->isBGGroup() || !l_Group->isRaidGroup())
                    return;
            }

            if (l_Group->IsLeader(GetPlayer()->GetGUID()))
                l_Type = CHAT_MSG_RAID_LEADER;

            sScriptMgr->OnPlayerChat(GetPlayer(), l_Type, l_Language, l_Text, l_Group);

            WorldPacket l_Data;

            ChatHandler::FillMessageData(&l_Data, this, uint8(l_Type), l_Language, "", 0, l_Text.c_str(), NULL);
            l_Group->BroadcastPacket(&l_Data, false);

            break;
        }
        case CHAT_MSG_RAID_WARNING:
        {
            Group * l_Group = GetPlayer()->GetGroup();

            if (!l_Group || !l_Group->isRaidGroup() || !(l_Group->IsLeader(GetPlayer()->GetGUID()) || l_Group->IsAssistant(GetPlayer()->GetGUID()) || l_Group->GetGroupType() & GROUPTYPE_EVERYONE_IS_ASSISTANT) || l_Group->isBGGroup())
                return;

            sScriptMgr->OnPlayerChat(GetPlayer(), l_Type, l_Language, l_Text, l_Group);

            WorldPacket l_Data;

            /// in battleground, raid warning is sent only to players in battleground - code is ok
            ChatHandler::FillMessageData(&l_Data, this, CHAT_MSG_RAID_WARNING, l_Language, "", 0, l_Text.c_str(), NULL);
            l_Group->BroadcastPacket(&l_Data, false);

            break;
        }
        case CHAT_MSG_INSTANCE_CHAT:
        case CHAT_MSG_INSTANCE_CHAT_LEADER:
        {
            /// battleground raid is always in Player->GetGroup(), never in GetOriginalGroup()
            Group * l_Group = GetPlayer()->GetGroup();

            if (!l_Group)
                return;

            if (!l_Group->isBGGroup())
            {
                l_Type = CHAT_MSG_RAID;

                if (l_Group->IsLeader(GetPlayer()->GetGUID()))
                    l_Type = CHAT_MSG_RAID_LEADER;
            }
            else if (l_Group->IsLeader(GetPlayer()->GetGUID()))
                l_Type = CHAT_MSG_INSTANCE_CHAT_LEADER;

            sScriptMgr->OnPlayerChat(GetPlayer(), l_Type, l_Language, l_Text, l_Group);

            WorldPacket l_Data;

            ChatHandler::FillMessageData(&l_Data, this, uint8(l_Type), l_Language, "", 0, l_Text.c_str(), NULL);
            l_Group->BroadcastPacket(&l_Data, false);

            break;
        }
        case CHAT_MSG_CHANNEL:
        {
            if (AccountMgr::IsPlayerAccount(GetSecurity()))
            {
                if (m_Player->getLevel() < sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ))
                {
                    SendNotification(GetTrinityString(LANG_CHANNEL_REQ), sWorld->getIntConfig(CONFIG_CHAT_CHANNEL_LEVEL_REQ));
                    return;
                }
            }

            if (ChannelMgr* l_ChannelManager = channelMgr(m_Player->GetTeam()))
            {
                if (Channel* l_Channel = l_ChannelManager->GetChannel(l_ChannelName, m_Player))
                {
                    sScriptMgr->OnPlayerChat(m_Player, l_Type, l_Language, l_Text, l_Channel);

                    l_Channel->Say(m_Player->GetGUID(), l_Text.c_str(), l_Language);
                }
            }

            break;
        }
        case CHAT_MSG_AFK:
        {
            if ((l_Text.empty() || !m_Player->isAFK()) && !m_Player->isInCombat())
            {
                if (!m_Player->isAFK())
                {
                    if (l_Text.empty())
                        l_Text  = GetTrinityString(LANG_PLAYER_AFK_DEFAULT);

                    m_Player->afkMsg = l_Text;
                }

                sScriptMgr->OnPlayerChat(m_Player, l_Type, l_Language, l_Text);

                m_Player->ToggleAFK();

                if (m_Player->isAFK() && m_Player->isDND())
                    m_Player->ToggleDND();
            }

            break;
        }
        case CHAT_MSG_DND:
        {
            if (l_Text.empty() || !m_Player->isDND())
            {
                if (!m_Player->isDND())
                {
                    if (l_Text.empty())
                        l_Text = GetTrinityString(LANG_PLAYER_DND_DEFAULT);
                    m_Player->dndMsg = l_Text;
                }

                sScriptMgr->OnPlayerChat(m_Player, l_Type, l_Language, l_Text);

                m_Player->ToggleDND();

                if (m_Player->isDND() && m_Player->isAFK())
                    m_Player->ToggleAFK();
            }

            break;
        }
        default:
            sLog->outError(LOG_FILTER_NETWORKIO, "CHAT: unknown message type %u, lang: %u", l_Type, l_Language);
            break;
    }
}

void WorldSession::HandleAddonMessagechatOpcode(WorldPacket& p_RecvData)
{
    Player *    l_Sender = GetPlayer();
    ChatMsg     l_Type;

    switch (p_RecvData.GetOpcode())
    {
        case CMSG_CHAT_ADDON_MESSAGE_BATTLEGROUND:
            l_Type = CHAT_MSG_INSTANCE_CHAT;
            break;
        case CMSG_CHAT_ADDON_MESSAGE_GUILD:
            l_Type = CHAT_MSG_GUILD;
            break;
        case CMSG_CHAT_ADDON_MESSAGE_OFFICER:
            l_Type = CHAT_MSG_OFFICER;
            break;
        case CMSG_CHAT_ADDON_MESSAGE_PARTY:
            l_Type = CHAT_MSG_PARTY;
            break;
        case CMSG_CHAT_ADDON_MESSAGE_RAID:
            l_Type = CHAT_MSG_RAID;
            break;
        case CMSG_CHAT_ADDON_MESSAGE_WHISPER:
            l_Type = CHAT_MSG_WHISPER;
            break;
        default:
            sLog->outError(LOG_FILTER_NETWORKIO, "HandleAddonMessagechatOpcode: Unknown addon chat opcode (%u)", p_RecvData.GetOpcode());
            p_RecvData.hexlike();
            return;
    }

    std::string l_AddonMessage;
    std::string l_AddonPrefix;
    std::string l_TargetName;

    switch (l_Type)
    {
        case CHAT_MSG_WHISPER:
        {
            uint32 l_TargetNameLenght   = p_RecvData.ReadBits(9);
            uint32 l_AddonPrefixLenght  = p_RecvData.ReadBits(7);
            uint32 l_AddonMessageLenght = p_RecvData.ReadBits(8);
            l_TargetName    = p_RecvData.ReadString(l_TargetNameLenght);
            l_AddonMessage  = p_RecvData.ReadString(l_AddonMessageLenght);
            l_AddonPrefix   = p_RecvData.ReadString(l_AddonPrefixLenght);
            break;
        }
        case CHAT_MSG_OFFICER:
        case CHAT_MSG_GUILD:
        case CHAT_MSG_RAID:
        case CHAT_MSG_PARTY:
        case CHAT_MSG_INSTANCE_CHAT:
        {
            uint32 l_AddonPrefixLenght  = p_RecvData.ReadBits(7);
            uint32 l_AddonMessageLenght = p_RecvData.ReadBits(8);
            l_AddonPrefix  = p_RecvData.ReadString(l_AddonPrefixLenght);
            l_AddonMessage = p_RecvData.ReadString(l_AddonMessageLenght);
            break;
        }
        default:
            break;
    }

    /// Logging enabled?
    if (sWorld->getBoolConfig(CONFIG_CHATLOG_ADDON))
    {
        if (l_AddonMessage.empty())
            return;

        /// Weird way to log stuff...
        sScriptMgr->OnPlayerChat(l_Sender, CHAT_MSG_ADDON, LANG_ADDON, l_AddonMessage);
    }

    /// Disabled addon channel?
    if (!sWorld->getBoolConfig(CONFIG_ADDON_CHANNEL))
        return;

    switch (l_Type)
    {
        case CHAT_MSG_INSTANCE_CHAT:
        {
            Group * l_Group = l_Sender->GetGroup();
            if (!l_Group || !l_Group->isBGGroup())
                return;

            WorldPacket l_Data;

            ChatHandler::FillMessageData(&l_Data, this, l_Type, LANG_ADDON, "", 0, l_AddonMessage.c_str(), NULL);
            l_Group->BroadcastAddonMessagePacket(&l_Data, l_AddonPrefix, false);

            break;
        }
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
        {
            if (l_Sender->GetGuildId())
                if (Guild* l_Guild = sGuildMgr->GetGuildById(l_Sender->GetGuildId()))
                    l_Guild->BroadcastAddonToGuild(this, l_Type == CHAT_MSG_OFFICER, l_AddonMessage, l_AddonPrefix);

            break;
        }
        case CHAT_MSG_WHISPER:
        {
            if (!normalizePlayerName(l_TargetName))
                break;

            Player* l_Receiver = sObjectAccessor->FindPlayerByName(l_TargetName.c_str());

            if (!l_Receiver)
                break;

            l_Sender->WhisperAddon(l_AddonMessage, l_AddonPrefix, l_Receiver);
            break;
        }
        /// Messages sent to "RAID" while in a party will get delivered to "PARTY"
        case CHAT_MSG_PARTY:
        case CHAT_MSG_RAID:
        {
            Group * l_Group = l_Sender->GetGroup();

            if (!l_Group || l_Group->isBGGroup())
                break;

            WorldPacket l_Data;

            ChatHandler::FillMessageData(&l_Data, this, l_Type, LANG_ADDON, "", 0, l_AddonMessage.c_str(), NULL, l_AddonPrefix.c_str());
            l_Group->BroadcastAddonMessagePacket(&l_Data, l_AddonPrefix, true, -1, l_Group->GetMemberGroup(l_Sender->GetGUID()));

            break;
        }
        default:
        {
            sLog->outError(LOG_FILTER_GENERAL, "HandleAddonMessagechatOpcode: unknown addon message type %u", l_Type);
            break;
        }
    }
}

void WorldSession::HandleEmoteOpcode(WorldPacket & recvData)
{
    if (!GetPlayer()->isAlive() || GetPlayer()->HasUnitState(UNIT_STATE_DIED))
        return;

    if (!GetPlayer()->CanSpeak())
    {
        std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
        SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
        recvData.rfinish(); // Prevent warnings
        return;
    }

    GetPlayer()->UpdateSpeakTime();

    uint32 emote;
    recvData >> emote;

    sScriptMgr->OnPlayerEmote(GetPlayer(), emote);
    GetPlayer()->HandleEmoteCommand(emote);
}

namespace JadeCore
{
    class EmoteChatBuilder
    {
        public:
            EmoteChatBuilder(Player const& player, uint32 text_emote, uint32 emote_num, Unit const* target)
                : i_player(player), i_text_emote(text_emote), i_emote_num(emote_num), i_target(target) {}

            void operator()(WorldPacket& data, LocaleConstant loc_idx)
            {
                ObjectGuid playerGuid = i_player.GetGUID();
                ObjectGuid targetGuid = i_target ? i_target->GetGUID() : 0;

                data.Initialize(SMSG_TEXT_EMOTE);
                data.WriteBit(playerGuid[0]);
                data.WriteBit(targetGuid[3]);
                data.WriteBit(targetGuid[4]);
                data.WriteBit(playerGuid[6]);
                data.WriteBit(playerGuid[7]);
                data.WriteBit(playerGuid[3]);
                data.WriteBit(targetGuid[6]);
                data.WriteBit(targetGuid[7]);
                data.WriteBit(playerGuid[5]);
                data.WriteBit(playerGuid[2]);
                data.WriteBit(playerGuid[1]);
                data.WriteBit(targetGuid[0]);
                data.WriteBit(playerGuid[4]);
                data.WriteBit(targetGuid[1]);
                data.WriteBit(targetGuid[5]);
                data.WriteBit(targetGuid[2]);

                data.WriteByteSeq(playerGuid[4]);
                data.WriteByteSeq(playerGuid[5]);
                data.WriteByteSeq(playerGuid[1]);
                data.WriteByteSeq(targetGuid[6]);
                data << int32(i_text_emote);
                data.WriteByteSeq(targetGuid[7]);
                data.WriteByteSeq(targetGuid[1]);
                data.WriteByteSeq(targetGuid[4]);
                data << uint32(i_emote_num);
                data.WriteByteSeq(targetGuid[0]);
                data.WriteByteSeq(playerGuid[7]);
                data.WriteByteSeq(playerGuid[3]);
                data.WriteByteSeq(targetGuid[2]);
                data.WriteByteSeq(playerGuid[6]);
                data.WriteByteSeq(playerGuid[2]);
                data.WriteByteSeq(targetGuid[5]);
                data.WriteByteSeq(playerGuid[0]);
                data.WriteByteSeq(targetGuid[3]);
            }

        private:
            Player const& i_player;
            uint32        i_text_emote;
            uint32        i_emote_num;
            Unit const*   i_target;
    };
}                                                           // namespace JadeCore

void WorldSession::HandleTextEmoteOpcode(WorldPacket & recvData)
{
    if (!GetPlayer()->isAlive())
        return;

    if (!GetPlayer()->CanSpeak())
    {
        std::string timeStr = secsToTimeString(m_muteTime - time(NULL));
        SendNotification(GetTrinityString(LANG_WAIT_BEFORE_SPEAKING), timeStr.c_str());
        return;
    }

    GetPlayer()->UpdateSpeakTime();

    uint32 text_emote, emoteNum;
    ObjectGuid guid;

    recvData >> emoteNum;
    recvData >> text_emote;

    uint8 bitsOrder[8] = { 4, 2, 5, 6, 0, 3, 7, 1 };
    recvData.ReadBitInOrder(guid, bitsOrder);

    recvData.FlushBits();

    uint8 bytesOrder[8] = { 4, 1, 5, 2, 3, 0, 6, 7 };
    recvData.ReadBytesSeq(guid, bytesOrder);

    sScriptMgr->OnPlayerTextEmote(GetPlayer(), text_emote, emoteNum, guid);

    EmotesTextEntry const* em = sEmotesTextStore.LookupEntry(text_emote);
    if (!em)
        return;

    uint32 emote_anim = em->textid;

    switch (emote_anim)
    {
        case EMOTE_STATE_SLEEP:
        case EMOTE_STATE_SIT:
        case EMOTE_STATE_KNEEL:
        case EMOTE_ONESHOT_NONE:
            break;
        default:
            // Only allow text-emotes for "dead" entities (feign death included)
            if (GetPlayer()->HasUnitState(UNIT_STATE_DIED))
                break;
             GetPlayer()->HandleEmoteCommand(emote_anim);
             break;
    }

    Unit* unit = ObjectAccessor::GetUnit(*m_Player, guid);

    CellCoord p = JadeCore::ComputeCellCoord(GetPlayer()->GetPositionX(), GetPlayer()->GetPositionY());

    Cell cell(p);
    cell.SetNoCreate();

    JadeCore::EmoteChatBuilder emote_builder(*GetPlayer(), emote_anim, text_emote, unit);
    JadeCore::LocalizedPacketDo<JadeCore::EmoteChatBuilder > emote_do(emote_builder);
    JadeCore::PlayerDistWorker<JadeCore::LocalizedPacketDo<JadeCore::EmoteChatBuilder > > emote_worker(GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE), emote_do);
    TypeContainerVisitor<JadeCore::PlayerDistWorker<JadeCore::LocalizedPacketDo<JadeCore::EmoteChatBuilder> >, WorldTypeMapContainer> message(emote_worker);
    cell.Visit(p, message, *GetPlayer()->GetMap(), *GetPlayer(), sWorld->getFloatConfig(CONFIG_LISTEN_RANGE_TEXTEMOTE));

    GetPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DO_EMOTE, text_emote, 0, 0, unit);

    //Send scripted event call
    if (unit && unit->GetTypeId() == TYPEID_UNIT && ((Creature*)unit)->AI())
        ((Creature*)unit)->AI()->ReceiveEmote(GetPlayer(), text_emote);
}

void WorldSession::HandleChatIgnoredOpcode(WorldPacket& recvData)
{
    ObjectGuid guid;
    uint8 unk;
    //sLog->outDebug(LOG_FILTER_PACKETIO, "WORLD: Received CMSG_CHAT_IGNORED");

    recvData >> unk;                                       // probably related to spam reporting

    uint8 bitOrder[8] = { 7, 1, 5, 3, 2, 6, 0, 4 };
    recvData.ReadBitInOrder(guid, bitOrder);

    recvData.FlushBits();

    uint8 byteOrder[8] = { 5, 0, 1, 2, 3, 6, 4, 7 };
    recvData.ReadBytesSeq(guid, byteOrder);

    Player* player = ObjectAccessor::FindPlayer(guid);
    if (!player || !player->GetSession())
        return;

    WorldPacket data;
    ChatHandler::FillMessageData(&data, this, CHAT_MSG_IGNORED, LANG_UNIVERSAL, NULL, GetPlayer()->GetGUID(), GetPlayer()->GetName(), NULL);
    player->GetSession()->SendPacket(&data);
}

void WorldSession::HandleChannelDeclineInvite(WorldPacket &recvPacket)
{
    sLog->outDebug(LOG_FILTER_NETWORKIO, "Opcode %u", recvPacket.GetOpcode());
}

void WorldSession::SendPlayerNotFoundNotice(std::string name)
{
    WorldPacket data(SMSG_CHAT_PLAYER_NOTFOUND, name.size()+1);

    data.WriteBits((name.size() - (name.size() % 2)) / 2, 8);
    data.WriteBit((name.size() % 2));
    data.FlushBits();
    data.append(name.c_str(), name.size());

    SendPacket(&data);
}

void WorldSession::SendPlayerAmbiguousNotice(std::string p_Name)
{
    WorldPacket l_Data(SMSG_CHAT_PLAYER_AMBIGUOUS, p_Name.size() + 2);
    l_Data.WriteBits(p_Name.length(), 9);
    l_Data.FlushBits();
    l_Data.WriteString(p_Name);

    SendPacket(&l_Data);
}

void WorldSession::SendWrongFactionNotice()
{
    //WorldPacket data(SMSG_CHAT_WRONG_FACTION, 0);
    //SendPacket(&data);
}

void WorldSession::SendChatRestrictedNotice(ChatRestrictionType restriction)
{
    WorldPacket data(SMSG_CHAT_RESTRICTED, 1);
    data << uint8(restriction);
    SendPacket(&data);
}
