////////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
////////////////////////////////////////////////////////////////////////
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
////////////////////////////////////////////////////////////////////////
#include "otpch.h"
#include "resources.h"

#include <boost/function.hpp>
#include <iostream>

#include "protocolgame.h"
#include "textlogger.h"

#include "waitlist.h"
#include "player.h"

#include "connection.h"
#include "networkmessage.h"
#include "outputmessage.h"

#include "iologindata.h"
#include "ioban.h"
#include "iodatalog.h"
// #include "iomarket.h"

#include "items.h"
#include "tile.h"
#include "house.h"

#include "actions.h"
#include "creatureevent.h"
#include "quests.h"

#include "chat.h"
#include "configmanager.h"
#include "game.h"

#include "combat.h"

#include "polls.h"

#if defined(WINDOWS) && !defined(__CONSOLE__)
#include "gui.h"
#endif

extern Game g_game;
extern ConfigManager g_config;
extern Actions actions;
extern CreatureEvents* g_creatureEvents;
extern Chat g_chat;

template<class FunctionType>
void ProtocolGame::addGameTaskInternal(uint32_t delay, const FunctionType& func)
{
	if(delay > 0)
		Dispatcher::getInstance().addTask(createTask(delay, func));
	else
		Dispatcher::getInstance().addTask(createTask(func));
}

#ifdef __ENABLE_SERVER_DIAGNOSTIC__
uint32_t ProtocolGame::protocolGameCount = 0;
#endif

void ProtocolGame::setPlayer(Player* p)
{
	player = p;
}

void ProtocolGame::setTVOwner(Player* newTVOwner)
{
    if(newTVOwner)
    {
        tvOwner = newTVOwner;
        sendTVStart();
    }
    else
    {
        Player* oldTVOwner = NULL;
        if(tvOwner)
            oldTVOwner = tvOwner;
        tvOwner = NULL;
        sendTVEnd(oldTVOwner);
    }
}

void ProtocolGame::setTVChannel(TVChannel* newTVChannel)
{
    tvChannel = newTVChannel;
    if(tvChannel)
    {
        tvChannel->addUser(player);
        sendChannel(tvChannel->getId(), tvChannel->getName());
    }
}

void ProtocolGame::releaseProtocol()
{
	if(player && player->client == this) {
#ifdef __LOG_PACKETS_
        std::stringstream ss;
		for (std::vector<std::string>::iterator it = m_lastPackets.begin(); it != m_lastPackets.end(); ++it) {
            ss << *it;
            ss << " ";
        }
        LOG_MESSAGE(LOGTYPE_NOTICE, ss.str(), "LAST PACKETS " + player->getName());
		m_lastPackets.clear();
#endif
        
        player->client = NULL;
    }

	Protocol::releaseProtocol();
}

void ProtocolGame::deleteProtocolTask()
{
	if(player)
	{
		g_game.freeThing(player);
		player = NULL;
	}

	Protocol::deleteProtocolTask();
}

bool ProtocolGame::login(const std::string& name, uint32_t id, const std::string& password,
	OperatingSystem_t operatingSystem, uint16_t version, bool gamemaster)
{
	//dispatcher thread
	PlayerVector players = g_game.getPlayersByName(name);
	Player* _player = NULL;
	if(!players.empty())
		_player = players[random_range(0, (players.size() - 1))];

	if(!_player || name == "Account Manager" || g_config.getNumber(ConfigManager::ALLOW_CLONES) > (int32_t)players.size())
	{
		player = new Player(name, this);
		player->addRef();

		player->setID();
		if(!IOLoginData::getInstance()->loadPlayer(player, name, true))
		{
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		Ban ban;
		ban.value = player->getID();
		ban.param = PLAYERBAN_BANISHMENT;

		ban.type = BAN_PLAYER;
		if(IOBan::getInstance()->getData(ban) && !player->hasFlag(PlayerFlag_CannotBeBanned))
		{
			bool deletion = ban.expires < 0;
			std::string name_ = "Automatic ";
			if(!ban.adminId)
				name_ += (deletion ? "deletion" : "banishment");
			else
				IOLoginData::getInstance()->getNameByGuid(ban.adminId, name_, true);

			char buffer[500 + ban.comment.length()];
			sprintf(buffer, "Your character has been %s at:\n%s by: %s,\nfor the following reason:\n%s.\nThe action taken was:\n%s.\nThe comment given was:\n%s.\nYour %s%s.",
				(deletion ? "deleted" : "banished"), formatDateShort(ban.added).c_str(), name_.c_str(),
				getReason(ban.reason).c_str(), getAction(ban.action, false).c_str(), ban.comment.c_str(),
				(deletion ? "character won't be undeleted" : "banishment will be lifted at:\n"),
				(deletion ? "." : formatDateShort(ban.expires, true).c_str()));

			disconnectClient(0x14, buffer);
			return false;
		}

		if(IOBan::getInstance()->isPlayerBanished(player->getGUID(), PLAYERBAN_LOCK) && id != 1)
		{
			if(g_config.getBool(ConfigManager::NAMELOCK_MANAGER))
			{
				player->name = "Account Manager";
				player->accountManager = MANAGER_NAMELOCK;

				player->managerNumber = id;
				player->managerString2 = name;
			}
			else
			{
				disconnectClient(0x14, "Your character has been namelocked.");
				return false;
			}
		}
		else if(player->getName() == "Account Manager" && g_config.getBool(ConfigManager::ACCOUNT_MANAGER))
		{
			if(id != 1)
			{
				player->accountManager = MANAGER_ACCOUNT;
				player->managerNumber = id;
			}
			else
				player->accountManager = MANAGER_NEW;
		}

		if(gamemaster && !player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges))
		{
			disconnectClient(0x14, "You are not a gamemaster! Turn off the gamemaster mode in your IP changer.");
			return false;
		}

		if(!player->hasFlag(PlayerFlag_CanAlwaysLogin))
		{
			if(g_game.getGameState() == GAME_STATE_CLOSING)
			{
				disconnectClient(0x14, "Gameworld is just going down, please come back later.");
				return false;
			}

			if(g_game.getGameState() == GAME_STATE_CLOSED)
			{
				disconnectClient(0x14, "Gameworld is currently closed, please come back later.");
				return false;
			}
		}

		if(g_config.getBool(ConfigManager::ONE_PLAYER_ON_ACCOUNT) && !player->isAccountManager() &&
			!IOLoginData::getInstance()->hasCustomFlag(id, PlayerCustomFlag_CanLoginMultipleCharacters))
		{
			bool found = false;
			PlayerVector tmp = g_game.getPlayersByAccount(id);
			for(PlayerVector::iterator it = tmp.begin(); it != tmp.end(); ++it)
			{
				if((*it)->getName() != name)
					continue;

				found = true;
				break;
			}

			if((tmp.size() > 0 && !found) || IOLoginData::getInstance()->isPlayerOnlineByAccount(player->getAccount()))
			{
				disconnectClient(0x14, "You may only login with one character\nof your account at the same time.");
				return false;
			}
		}

		if(!WaitingList::getInstance()->login(player))
		{
			if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false))
			{
				TRACK_MESSAGE(output);
				std::stringstream ss;
				ss << "Too many players online.\n" << "You are ";

				int32_t slot = WaitingList::getInstance()->getSlot(player);
				if(slot)
				{
					ss << "at ";
					if(slot > 0)
						ss << slot;
					else
						ss << "unknown";

					ss << " place on the waiting list.";
				}
				else
					ss << "awaiting connection...";

				output->AddByte(0x16);
				output->AddString(ss.str());
				output->AddByte(WaitingList::getTime(slot));
				OutputMessagePool::getInstance()->send(output);
			}

			getConnection()->close();
			return false;
		}

		if(!IOLoginData::getInstance()->loadPlayer(player, name))
		{
			disconnectClient(0x14, "Your character could not be loaded.");
			return false;
		}

		player->setOperatingSystem(operatingSystem);
		player->setClientVersion(version);
		player->setLanguage(IOLoginData::getInstance()->getAccountLanguage(id));
		if(!g_game.placeCreature(player, player->getLoginPosition(), false, true) && !g_game.placeCreature(player, player->getMasterPosition(), false, true))
		{
			disconnectClient(0x14, "Temple position is wrong. Contact with the administration.");
			return false;

		}
		
		if(player->isUsingOtclient()) {
            player->registerCreatureEvent("ExtendedOpcode");
        }

		player->lastIP = player->getIP();
		player->lastLoad = OTSYS_TIME();
		player->lastLogin = std::max(time(NULL), player->lastLogin + 1);
		IODatalog::getInstance()->logLogin(player->getGUID(), player->getLastLogin(), player->getIP());

		m_acceptPackets = true;
		return true;
	}
	else if(_player->client)
	{
		if(m_eventConnect || !g_config.getBool(ConfigManager::REPLACE_KICK_ON_LOGIN))
		{
			//A task has already been scheduled just bail out (should not be overriden)
			disconnectClient(0x14, "You are already logged in.");
			return false;
		}

		g_chat.removeUserFromAllChannels(_player);
		_player->disconnect();
		_player->isConnecting = true;

		addRef();
		m_eventConnect = Scheduler::getInstance().addEvent(createSchedulerTask(
			1000, boost::bind(&ProtocolGame::connect, this, _player->getID(), operatingSystem, version)));
		return true;
	}

	addRef();
	return connect(_player->getID(), operatingSystem, version);
}

bool ProtocolGame::logout(bool displayEffect, bool forceLogout)
{
	//dispatcher thread
	if(!player)
		return false;

	if(!player->isRemoved())
	{
		if(!forceLogout)
		{
			if(!IOLoginData::getInstance()->hasCustomFlag(player->getAccount(), PlayerCustomFlag_CanLogoutAnytime))
			{
				if(player->getTile()->hasFlag(TILESTATE_NOLOGOUT) || player->getPvpArena() || player->getTournamentReady())
				{
					player->sendCancelMessage(RET_YOUCANNOTLOGOUTHERE);
					return false;
				}

				if(player->hasCondition(CONDITION_INFIGHT))
				{
					player->sendCancelMessage(RET_YOUMAYNOTLOGOUTDURINGAFIGHT);
					return false;
				}

				if(!g_creatureEvents->playerLogout(player, false)) //let the script handle the error message
					return false;
			}
			else
				g_creatureEvents->playerLogout(player, false);
		}
		else if(!g_creatureEvents->playerLogout(player, true))
			return false;
	}
	else
		displayEffect = false;

	if(displayEffect && !player->isGhost())
		g_game.addMagicEffect(player->getPosition(), MAGIC_EFFECT_POFF);

	if(Connection_ptr connection = getConnection())
		connection->close();

	return g_game.removeCreature(player);
}

bool ProtocolGame::connect(uint32_t playerId, OperatingSystem_t operatingSystem, uint16_t version)
{
	unRef();
	m_eventConnect = 0;

	Player* _player = g_game.getPlayerByID(playerId);
	if(!_player || _player->isRemoved() || _player->client)
	{
		disconnectClient(0x14, "You are already logged in.");
		return false;
	}

	player = _player;
	player->addRef();
	player->isConnecting = false;

	player->client = this;
	player->sendCreatureAppear(player);

	player->setOperatingSystem(operatingSystem);
	player->setClientVersion(version);

	player->lastIP = player->getIP();
	player->lastLoad = OTSYS_TIME();
	player->lastLogin = std::max(time(NULL), player->lastLogin + 1);
	IODatalog::getInstance()->logLogin(player->getGUID(), player->getLastLogin(), player->getIP());
	
	if (TVChannel* tv = g_chat.getTVChannel(player)) { // If player reconect the Tv Channel will freeze, let's delete it
        g_chat.deleteChannel(player, tv->getId());
    }

	m_acceptPackets = true;	
	g_creatureEvents->playerKickLogin(player);
	
	return true;
}

void ProtocolGame::disconnect()
{
	if(getConnection())
		getConnection()->close();
}

void ProtocolGame::disconnectClient(uint8_t error, const char* message)
{
	if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false))
	{
		TRACK_MESSAGE(output);
		output->AddByte(error);
		output->AddString(message);
		OutputMessagePool::getInstance()->send(output);
	}

	disconnect();
}

void ProtocolGame::onConnect()
{
	if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false))
	{
		TRACK_MESSAGE(output);
		enableChecksum();

		output->AddByte(0x1F);
		output->AddU16(random_range(0, 0xFFFF));
		output->AddU16(0x00);
		output->AddByte(random_range(0, 0xFF));

		OutputMessagePool::getInstance()->send(output);
	}
}

void ProtocolGame::onRecvFirstMessage(NetworkMessage& msg)
{
	parseFirstPacket(msg);
}

bool ProtocolGame::parseFirstPacket(NetworkMessage& msg)
{
	if(
#if defined(WINDOWS) && !defined(__CONSOLE__)
		!GUI::getInstance()->m_connections ||
#endif
		g_game.getGameState() == GAME_STATE_SHUTDOWN)
	{
		getConnection()->close();
		return false;
	}

	OperatingSystem_t operatingSystem = (OperatingSystem_t)msg.GetU16();
	uint16_t version = msg.GetU16();
	if(!RSA_decrypt(msg))
	{
		std::cout << "[Warning - Protocol::RSA_decrypt] IP: " << convertIPAddress(getConnection()->getIP()) << std::endl;
        getConnection()->close();
		return false;
	}

	uint32_t key[4] = {msg.GetU32(), msg.GetU32(), msg.GetU32(), msg.GetU32()};
	enableXTEAEncryption();
	setXTEAKey(key);

	bool gamemaster = msg.GetByte();
	std::string name = msg.GetString(), character = msg.GetString(), password = msg.GetString();

	msg.SkipBytes(6); //841- wtf?
	// if(version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX)
	// {
		// disconnectClient(0x14, CLIENT_VERSION_STRING);
		// return false;
	// }

	if(name.empty())
	{
		if(!g_config.getBool(ConfigManager::ACCOUNT_MANAGER))
		{
			disconnectClient(0x14, "Invalid account name.");
			return false;
		}

		name = "1";
		password = "1";
	}

	if(g_game.getGameState() < GAME_STATE_NORMAL)
	{
		std::string clientMessage = g_config.getString(ConfigManager::SERVER_NAME) + " is starting up. Please wait.";
        disconnectClient(0x14, clientMessage.c_str());
		return false;
	}

	if(g_game.getGameState() == GAME_STATE_MAINTAIN)
	{
		disconnectClient(0x14, "Gameworld is under maintenance, please re-connect in a while.");
		return false;
	}

	if(ConnectionManager::getInstance()->isDisabled(getIP(), protocolId))
	{
		disconnectClient(0x14, "Too many connections attempts from your IP address, please try again later.");
		return false;
	}

	if(IOBan::getInstance()->isIpBanished(getIP()))
	{
		disconnectClient(0x14, "Your IP is banished!");
		return false;
	}

	uint32_t id = 1;
	if(!IOLoginData::getInstance()->getAccountId(name, id))
	{
		ConnectionManager::getInstance()->addAttempt(getIP(), protocolId, false);
		disconnectClient(0x14, "Invalid account name.");
		return false;
	}

	std::string hash;
	if(!IOLoginData::getInstance()->getPassword(id, hash, character) || !encryptTest(password, hash))
	{
		ConnectionManager::getInstance()->addAttempt(getIP(), protocolId, false);
		disconnectClient(0x14, "Invalid password.");
		return false;
	}

	Ban ban;
	ban.value = id;

	ban.type = BAN_ACCOUNT;
	if(IOBan::getInstance()->getData(ban) && !IOLoginData::getInstance()->hasFlag(id, PlayerFlag_CannotBeBanned))
	{
		bool deletion = ban.expires < 0;
		std::string name_ = "Automatic ";
		if(!ban.adminId)
			name_ += (deletion ? "deletion" : "banishment");
		else
			IOLoginData::getInstance()->getNameByGuid(ban.adminId, name_, true);

		char buffer[500 + ban.comment.length()];
		sprintf(buffer, "Your account has been %s at:\n%s by: %s,\nfor the following reason:\n%s.\nThe action taken was:\n%s.\nThe comment given was:\n%s.\nYour %s%s.",
			(deletion ? "deleted" : "banished"), formatDateShort(ban.added).c_str(), name_.c_str(),
			getReason(ban.reason).c_str(), getAction(ban.action, false).c_str(), ban.comment.c_str(),
			(deletion ? "account won't be undeleted" : "banishment will be lifted at:\n"),
			(deletion ? "." : formatDateShort(ban.expires, true).c_str()));

		disconnectClient(0x14, buffer);
		return false;
	}

	ConnectionManager::getInstance()->addAttempt(getIP(), protocolId, true);
	Dispatcher::getInstance().addTask(createTask(boost::bind(
		&ProtocolGame::login, this, character, id, password, operatingSystem, version, gamemaster)));
	return true;
}

void ProtocolGame::parsePacket(NetworkMessage &msg)
{
	if(!player || !m_acceptPackets || g_game.getGameState() == GAME_STATE_SHUTDOWN
		|| msg.getMessageLength() <= 0)
		return;

	uint8_t recvbyte = msg.GetByte();
	//a dead player cannot performs actions
	if(player->isRemoved() && recvbyte != 0x14)
		return;

	if(player->isAccountManager())
	{
		switch(recvbyte)
		{
			case 0x14:
				parseLogout(msg);
				break;

			case 0x96:
				parseSay(msg);
				break;

			default:
				sendCancelWalk();
				break;
		}
	}
	else
	{
		switch(recvbyte)
		{
			case 0x14: // logout
				parseLogout(msg);
				break;

			case 0x1E: // keep alive / ping response
				parseReceivePing(msg);
				break;

			case 0x64: // move with steps
				parseAutoWalk(msg);
				break;

			case 0x65: // move north
			case 0x66: // move east
			case 0x67: // move south
			case 0x68: // move west
				parseMove(msg, (Direction)(recvbyte - 0x65));
				break;

			case 0x69: // stop-autowalk
				addGameTask(&Game::playerStopAutoWalk, player->getID());
				break;

			case 0x6A:
				parseMove(msg, NORTHEAST);
				break;

			case 0x6B:
				parseMove(msg, SOUTHEAST);
				break;

			case 0x6C:
				parseMove(msg, SOUTHWEST);
				break;

			case 0x6D:
				parseMove(msg, NORTHWEST);
				break;

			case 0x6F: // turn north
			case 0x70: // turn east
			case 0x71: // turn south
			case 0x72: // turn west
				parseTurn(msg, (Direction)(recvbyte - 0x6F));
				break;

			case 0x78: // throw item
				parseThrow(msg);
				break;

			case 0x79: // description in shop window
				parseLookInShop(msg);
				break;

			case 0x7A: // player bought from shop
				parsePlayerPurchase(msg);
				break;

			case 0x7B: // player sold to shop
				parsePlayerSale(msg);
				break;

			case 0x7C: // player closed shop window
				parseCloseShop(msg);
				break;

			case 0x7D: // Request trade
				parseRequestTrade(msg);
				break;

			case 0x7E: // Look at an item in trade
				parseLookInTrade(msg);
				break;

			case 0x7F: // Accept trade
				parseAcceptTrade(msg);
				break;

			case 0x80: // close/cancel trade
				parseCloseTrade();
				break;

			case 0x82: // use item
				parseUseItem(msg);
				break;

			case 0x83: // use item
				parseUseItemEx(msg);
				break;

			case 0x84: // battle window
				parseBattleWindow(msg);
				break;

			case 0x85: //rotate item
				parseRotateItem(msg);
				break;

			case 0x87: // close container
				parseCloseContainer(msg);
				break;

			case 0x88: //"up-arrow" - container
				parseUpArrowContainer(msg);
				break;

			case 0x89:
				parseTextWindow(msg);
				break;

			case 0x8A:
				parseHouseWindow(msg);
				break;

			case 0x8C: // throw item
				parseLookAt(msg);
				break;

			case 0x96: // say something
				parseSay(msg);
				break;

			case 0x97: // request channels
				parseGetChannels(msg);
				break;

			case 0x98: // open channel
				parseOpenChannel(msg);
				break;

			case 0x99: // close channel
				parseCloseChannel(msg);
				break;

			case 0x9A: // open priv
				parseOpenPriv(msg);
				break;

			case 0x9B: //process report
				parseProcessRuleViolation(msg);
				break;

			case 0x9C: //gm closes report
				parseCloseRuleViolation(msg);
				break;

			case 0x9D: //player cancels report
				parseCancelRuleViolation(msg);
				break;

			case 0x9E: // close NPC
				parseCloseNpc(msg);
				break;

			case 0xA0: // set attack and follow mode
				parseFightModes(msg);
				break;

			case 0xA1: // attack
				parseAttack(msg);
				break;

			case 0xA2: //follow
				parseFollow(msg);
				break;

			case 0xA3: // invite party
				parseInviteToParty(msg);
				break;

			case 0xA4: // join party
				parseJoinParty(msg);
				break;

			case 0xA5: // revoke party
				parseRevokePartyInvite(msg);
				break;

			case 0xA6: // pass leadership
				parsePassPartyLeadership(msg);
				break;

			case 0xA7: // leave party
				parseLeaveParty(msg);
				break;

			case 0xA8: // share exp
				parseSharePartyExperience(msg);
				break;

			case 0xAA:
				parseCreatePrivateChannel(msg);
				break;

			case 0xAB:
				parseChannelInvite(msg);
				break;

			case 0xAC:
				parseChannelExclude(msg);
				break;

			case 0xBE: // cancel move
				parseCancelMove(msg);
				break;

			case 0xC9: //client request to resend the tile
				parseUpdateTile(msg);
				break;

			case 0xCA: //client request to resend the container (happens when you store more than container maxsize)
				parseUpdateContainer(msg);
				break;

			case 0xD2: // request outfit
				if((!player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges) || !g_config.getBool(
					ConfigManager::DISABLE_OUTFITS_PRIVILEGED)) && (g_config.getBool(ConfigManager::ALLOW_CHANGEOUTFIT)
					|| g_config.getBool(ConfigManager::ALLOW_CHANGECOLORS) || g_config.getBool(ConfigManager::ALLOW_CHANGEADDONS)))
					parseRequestOutfit(msg);
				break;

			case 0xD3: // set outfit
				if((!player->hasCustomFlag(PlayerCustomFlag_GamemasterPrivileges) || !g_config.getBool(ConfigManager::DISABLE_OUTFITS_PRIVILEGED))
					&& (g_config.getBool(ConfigManager::ALLOW_CHANGECOLORS) || g_config.getBool(ConfigManager::ALLOW_CHANGEOUTFIT)))
				parseSetOutfit(msg);
				break;

			case 0xDC:
				parseAddVip(msg);
				break;

			case 0xDD:
				parseRemoveVip(msg);
				break;

			case 0xE6:
				parseBugReport(msg);
				break;

			case 0xE7:
				parseViolationWindow(msg);
				break;

			case 0xE8:
				parseDebugAssert(msg);
				break;

			case 0xF0:
				parseQuests(msg);
				break;

			case 0xF1:
				parseQuestInfo(msg);
				break;
				
			// case 0xF4: parseMarketLeave(); break;
		    // case 0xF5: parseMarketBrowse(msg); break;
		    // case 0xF6: parseMarketCreateOffer(msg); break;
		    // case 0xF7: parseMarketCancelOffer(msg); break;
		    // case 0xF8: parseMarketAcceptOffer(msg); break;
		    case 0xFA: parseRequestPollWindow(msg); break;
		    case 0xFB: parsePollVote(msg); break;
		    case 0x1D: addGameTask(&Game::playerReceivePingBack, player->getID()); break;
		    case 0x32: parseExtendedOpcode(msg); break; // otclient extended opcode

			default:
			{
				if(g_config.getBool(ConfigManager::BAN_UNKNOWN_BYTES))
				{
					int64_t banTime = -1;
					ViolationAction_t action = ACTION_BANISHMENT;
					Account tmp = IOLoginData::getInstance()->loadAccount(player->getAccount(), true);

					tmp.warnings++;
					if(tmp.warnings >= g_config.getNumber(ConfigManager::WARNINGS_TO_DELETION))
						action = ACTION_DELETION;
					else if(tmp.warnings >= g_config.getNumber(ConfigManager::WARNINGS_TO_FINALBAN))
					{
						banTime = time(NULL) + g_config.getNumber(ConfigManager::FINALBAN_LENGTH);
						action = ACTION_BANFINAL;
					}
					else
						banTime = time(NULL) + g_config.getNumber(ConfigManager::BAN_LENGTH);

					if(IOBan::getInstance()->addAccountBanishment(tmp.number, banTime, 13, action,
						"Sending unknown packets to the server.", 0, player->getGUID()))
					{
						IOLoginData::getInstance()->saveAccount(tmp);
						player->sendTextMessage(MSG_INFO_DESCR, "You have been banished.");

						g_game.addMagicEffect(player->getPosition(), MAGIC_EFFECT_WRAPS_GREEN);
						Scheduler::getInstance().addEvent(createSchedulerTask(1000, boost::bind(
							&Game::kickPlayer, &g_game, player->getID(), false)));
					}
				}

				std::stringstream hex, s;
				hex << "0x" << std::hex << (int16_t)recvbyte << std::dec;
				s << player->getName() << " sent unknown byte: " << hex << std::endl;

				LOG_MESSAGE(LOGTYPE_NOTICE, s.str(), "PLAYER")
				Logger::getInstance()->eFile(getFilePath(FILE_TYPE_LOG, "bots/" + player->getName() + ".log").c_str(),
					"[" + formatDate() + "] Received byte " + hex.str(), false);
				break;
			}
		}
	}
}

void ProtocolGame::GetTileDescription(const Tile* tile, NetworkMessage_ptr msg)
{
    if(!tile)
		return;

	int32_t count = 0;
	if(tile->ground)
	{
		msg->AddItem(tile->ground);
		count++;
	}

	const TileItemVector* items = tile->getItemList();
	const CreatureVector* creatures = tile->getCreatures();

	ItemVector::const_iterator it;
	if(items)
	{
		for(it = items->getBeginTopItem(); (it != items->getEndTopItem() && count < 10); ++it, ++count)
			msg->AddItem(*it);
	}

	if(creatures)
	{
        for(CreatureVector::const_reverse_iterator cit = creatures->rbegin(); (cit != creatures->rend() && count < 10); ++cit)
		{
            if(!player->canSeeCreature(*cit))
				continue;

			bool known;
			uint32_t removedKnown;
			checkCreatureAsKnown(/*(*cit)->getID()*/getCreatureID(*cit), known, removedKnown);

			AddCreature(msg, (*cit), known, removedKnown);
			count++;
		}
	}

	if(items)
	{
		for(it = items->getBeginDownItem(); (it != items->getEndDownItem() && count < 10); ++it, ++count)
			msg->AddItem(*it);
	}
}

void ProtocolGame::GetMapDescription(int32_t x, int32_t y, int32_t z,
	int32_t width, int32_t height, NetworkMessage_ptr msg)
{
	int32_t skip = -1, startz, endz, zstep = 0;
	if(z > 7)
	{
		startz = z - 2;
		endz = std::min((int32_t)MAP_MAX_LAYERS - 1, z + 2);
		zstep = 1;
	}
	else
	{
		startz = 7;
		endz = 0;
		zstep = -1;
	}

	for(int32_t nz = startz; nz != endz + zstep; nz += zstep)
		GetFloorDescription(msg, x, y, nz, width, height, z - nz, skip);

	if(skip >= 0)
	{
		msg->AddByte(skip);
		msg->AddByte(0xFF);
		//cc += skip;
	}
}

void ProtocolGame::GetFloorDescription(NetworkMessage_ptr msg, int32_t x, int32_t y, int32_t z,
		int32_t width, int32_t height, int32_t offset, int32_t& skip)
{
	Tile* tile = NULL;
	for(int32_t nx = 0; nx < width; nx++)
	{
		for(int32_t ny = 0; ny < height; ny++)
		{
			if((tile = g_game.getTile(Position(x + nx + offset, y + ny + offset, z))))
			{
				if(skip >= 0)
				{
					msg->AddByte(skip);
					msg->AddByte(0xFF);
				}

				skip = 0;
				GetTileDescription(tile, msg);
			}
			else
			{
				++skip;
				if(skip == 0xFF)
				{
					msg->AddByte(0xFF);
					msg->AddByte(0xFF);
					skip = -1;
				}
			}
		}
	}
}

void ProtocolGame::removeKnownCreature(uint32_t id)
{
     // Find him and get rid of him, we don't know him anymore!
     std::list<uint32_t>::iterator i = std::find(knownCreatureList.begin(), knownCreatureList.end(), id);
     if(i != knownCreatureList.end()) {
          knownCreatureList.erase(i);
     }
 
     removedCreatureList.push_back(id); // Prompt ProtocolGame::checkCreatureAsKnown for an update
}

void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	// loop through the known creature list and check if the given creature is in
	for(std::list<uint32_t>::iterator it = knownCreatureList.begin(); it != knownCreatureList.end(); ++it)
	{
		if((*it) != id)
			continue;

		// know... make the creature even more known...
		knownCreatureList.erase(it);
		knownCreatureList.push_back(id);

		known = true;
		return;
	}
	// ok, he is unknown...
	known = false;
	// ... but not in future
	knownCreatureList.push_back(id);
	// too many known creatures?
	if(knownCreatureList.size() > 250)
	{
		// lets try to remove one from the end of the list
		removedKnown = knownCreatureList.front();
		Creature* c = NULL;
		for(int32_t n = 0; n < 250; n++)
		{
			removedKnown = knownCreatureList.front();
			if(!(c = g_game.getCreatureByID(removedKnown)) || !canSee(c))
				break;

			// this creature we can't remove, still in sight, so back to the end
			knownCreatureList.pop_front();
			knownCreatureList.push_back(removedKnown);
		}

		// hopefully we found someone to remove :S, we got only 250 tries
		// if not... lets kick some players with debug errors :)
		knownCreatureList.pop_front();
	}
	else // we can cache without problems :)
	{
		if(removedCreatureList.size() > 0) // We have a creature in our removed list, get him out of there stat!
		{
			removedKnown = removedCreatureList.front();
			removedCreatureList.pop_front();
			
			std::list<uint32_t>::iterator remove = std::find(removedCreatureList.begin(), removedCreatureList.end(), removedKnown);
			
			if(remove != removedCreatureList.end()) // I see you but, I think i've seen you before? You look different!
			{
				removedCreatureList.erase(remove);
			}
		}
		else 
			removedKnown = 0;
	}	
}

/*
void ProtocolGame::checkCreatureAsKnown(uint32_t id, bool& known, uint32_t& removedKnown)
{
	// loop through the known creature list and check if the given creature is in
	for(std::list<uint32_t>::iterator it = knownCreatureList.begin(); it != knownCreatureList.end(); ++it)
	{
		if((*it) != id)
			continue;

		// know... make the creature even more known...
		knownCreatureList.erase(it);
		knownCreatureList.push_back(id);

		known = true;
		return;
	}

	// ok, he is unknown...
	known = false;
	// ... but not in future
	knownCreatureList.push_back(id);
	// too many known creatures?
	if(knownCreatureList.size() > 250)
	{
		// lets try to remove one from the end of the list
		Creature* c = NULL;
		for(int32_t n = 0; n < 250; n++)
		{
			removedKnown = knownCreatureList.front();
			if(!(c = g_game.getCreatureByID(removedKnown)) || !canSee(c))
				break;

			// this creature we can't remove, still in sight, so back to the end
			knownCreatureList.pop_front();
			knownCreatureList.push_back(removedKnown);
		}

		// hopefully we found someone to remove :S, we got only 250 tries
		// if not... lets kick some players with debug errors :)
		knownCreatureList.pop_front();
	}
	else // we can cache without problems :)
		removedKnown = 0;
}
*/

bool ProtocolGame::canSee(const Creature* c) const
{    
    return !c->isRemoved() && ((tvOwner && tvOwner->canSeeCreature(c)) || (player->canSeeCreature(c))) && canSee(c->getPosition());
}

bool ProtocolGame::canSee(const Position& pos) const
{
    return canSee(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSee(uint16_t x, uint16_t y, uint16_t z) const
{
#ifdef __DEBUG__
	if(z < 0 || z >= MAP_MAX_LAYERS)
		std::cout << "[Warning - ProtocolGame::canSee] Z-value is out of range!" << std::endl;
#endif
    Player* tmpPlayer = NULL;
    if(tvOwner)
        tmpPlayer = tvOwner;
    else
        tmpPlayer = player;
    const Position& myPos = tmpPlayer->getPosition();  
      
	if(myPos.z <= 7)
	{
		//we are on ground level or above (7 -> 0), view is from 7 -> 0
		if(z > 7)
			return false;
	}
	else if(myPos.z >= 8 && std::abs(myPos.z - z) > 2) //we are underground (8 -> 15), view is +/- 2 from the floor we stand on
		return false;

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.z - z;
	return ((x >= myPos.x - Map::maxClientViewportX + offsetz) && (x <= myPos.x + (Map::maxClientViewportX+1) + offsetz) &&
     	(y >= myPos.y - Map::maxClientViewportY + offsetz) && (y <= myPos.y + (Map::maxClientViewportY+1) + offsetz));
}

bool ProtocolGame::canSeeIgnoringTV(const Creature* c) const
{    
    return !c->isRemoved() && player->canSeeCreatureIgnoringTV(c) && canSeeIgnoringTV(c->getPosition());
}

bool ProtocolGame::canSeeIgnoringTV(const Position& pos) const
{
    return canSeeIgnoringTV(pos.x, pos.y, pos.z);
}

bool ProtocolGame::canSeeIgnoringTV(uint16_t x, uint16_t y, uint16_t z) const
{
#ifdef __DEBUG__
	if(z < 0 || z >= MAP_MAX_LAYERS)
		std::cout << "[Warning - ProtocolGame::canSee] Z-value is out of range!" << std::endl;
#endif
    const Position& myPos = player->getPosition();  
      
	if(myPos.z <= 7)
	{
		//we are on ground level or above (7 -> 0), view is from 7 -> 0
		if(z > 7)
			return false;
	}
	else if(myPos.z >= 8 && std::abs(myPos.z - z) > 2) //we are underground (8 -> 15), view is +/- 2 from the floor we stand on
		return false;

	//negative offset means that the action taken place is on a lower floor than ourself
	int32_t offsetz = myPos.z - z;
	return ((x >= myPos.x - 8 + offsetz) && (x <= myPos.x + 9 + offsetz) &&
		(y >= myPos.y - 6 + offsetz) && (y <= myPos.y + 7 + offsetz));
}

uint32_t ProtocolGame::getCreatureID(Creature* creature)
{
    if(tvOwner)
    {
        if(creature == tvOwner)
            return player->getID();
        else if(creature == player)
            return tvOwner->getID();
    }
    return creature->getID();
}

uint32_t ProtocolGame::getCreatureID(const Creature* creature)
{
    if(tvOwner)
    {
        if(creature == tvOwner)
            return player->getID();
        else if(creature == player)
            return tvOwner->getID();
    }
    return creature->getID();
}

//********************** Parse methods *******************************//
void ProtocolGame::parseLogout(NetworkMessage& msg)
{
	Dispatcher::getInstance().addTask(createTask(boost::bind(&ProtocolGame::logout, this, true, false)));
}

void ProtocolGame::parseCreatePrivateChannel(NetworkMessage& msg)
{
	addGameTask(&Game::playerCreatePrivateChannel, player->getID());
}

void ProtocolGame::parseChannelInvite(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelInvite, player->getID(), name);
}

void ProtocolGame::parseChannelExclude(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	addGameTask(&Game::playerChannelExclude, player->getID(), name);
}

void ProtocolGame::parseGetChannels(NetworkMessage& msg)
{
	addGameTask(&Game::playerRequestChannels, player->getID());
}

void ProtocolGame::parseOpenChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.GetU16();
	addGameTask(&Game::playerOpenChannel, player->getID(), channelId);
}

void ProtocolGame::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.GetU16();
	addGameTask(&Game::playerCloseChannel, player->getID(), channelId);
}

void ProtocolGame::parseOpenPriv(NetworkMessage& msg)
{
	const std::string receiver = msg.GetString();
	addGameTask(&Game::playerOpenPrivateChannel, player->getID(), receiver);
}

void ProtocolGame::parseProcessRuleViolation(NetworkMessage& msg)
{
	const std::string reporter = msg.GetString();
	addGameTask(&Game::playerProcessRuleViolation, player->getID(), reporter);
}

void ProtocolGame::parseCloseRuleViolation(NetworkMessage& msg)
{
	const std::string reporter = msg.GetString();
	addGameTask(&Game::playerCloseRuleViolation, player->getID(), reporter);
}

void ProtocolGame::parseCancelRuleViolation(NetworkMessage& msg)
{
	addGameTask(&Game::playerCancelRuleViolation, player->getID());
}

void ProtocolGame::parseCloseNpc(NetworkMessage& msg)
{
	addGameTask(&Game::playerCloseNpcChannel, player->getID());
}

void ProtocolGame::parseCancelMove(NetworkMessage& msg)
{
	addGameTask(&Game::playerCancelAttackAndFollow, player->getID());
}

void ProtocolGame::parseReceivePing(NetworkMessage& msg)
{
	addGameTask(&Game::playerReceivePing, player->getID());
}

void ProtocolGame::parseAutoWalk(NetworkMessage& msg)
{
	if(tvOwner)
    {
        sendCancelWalk();
        return;
    }    
    // first we get all directions...
	std::list<Direction> path;
	size_t dirCount = msg.GetByte();
	for(size_t i = 0; i < dirCount; ++i)
	{
		uint8_t rawDir = msg.GetByte();
		Direction dir = SOUTH;
		switch(rawDir)
		{
			case 1:
				dir = EAST;
				break;
			case 2:
				dir = NORTHEAST;
				break;
			case 3:
				dir = NORTH;
				break;
			case 4:
				dir = NORTHWEST;
				break;
			case 5:
				dir = WEST;
				break;
			case 6:
				dir = SOUTHWEST;
				break;
			case 7:
				dir = SOUTH;
				break;
			case 8:
				dir = SOUTHEAST;
				break;
			default:
				continue;
		}

		path.push_back(dir);
	}

	addGameTask(&Game::playerAutoWalk, player->getID(), path);
}

void ProtocolGame::parseMove(NetworkMessage& msg, Direction dir)
{
    if(tvOwner)
    {
        sendCancelWalk();
        return;
    }        
    addGameTask(&Game::playerMove, player->getID(), dir);
}

void ProtocolGame::parseTurn(NetworkMessage& msg, Direction dir)
{
	if(tvOwner)
        return;
    addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerTurn, player->getID(), dir);
}

void ProtocolGame::parseRequestOutfit(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    addGameTask(&Game::playerRequestOutfit, player->getID());
}

void ProtocolGame::parseSetOutfit(NetworkMessage& msg)
{
	Outfit_t newOutfit = player->defaultOutfit;
	if(g_config.getBool(ConfigManager::ALLOW_CHANGEOUTFIT))
		newOutfit.lookType = msg.GetU16();
	else
		msg.SkipBytes(2);

	if(g_config.getBool(ConfigManager::ALLOW_CHANGECOLORS))
	{
		newOutfit.lookHead = msg.GetByte();
		newOutfit.lookBody = msg.GetByte();
		newOutfit.lookLegs = msg.GetByte();
		newOutfit.lookFeet = msg.GetByte();
	}
	else
		msg.SkipBytes(4);

	if(g_config.getBool(ConfigManager::ALLOW_CHANGEADDONS))
		newOutfit.lookAddons = msg.GetByte();
	else
		msg.SkipBytes(1);

	addGameTask(&Game::playerChangeOutfit, player->getID(), newOutfit);
}

void ProtocolGame::parseUseItem(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	int16_t stackpos = msg.GetByte();
	uint8_t index = msg.GetByte();
	bool isHotkey = (pos.x == 0xFFFF && !pos.y && !pos.z);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItem, player->getID(), pos, stackpos, index, spriteId, isHotkey);
}

void ProtocolGame::parseUseItemEx(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    Position fromPos = msg.GetPosition();
	uint16_t fromSpriteId = msg.GetSpriteId();
	int16_t fromStackpos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint16_t toSpriteId = msg.GetU16();
	int16_t toStackpos = msg.GetByte();
	bool isHotkey = (fromPos.x == 0xFFFF && !fromPos.y && !fromPos.z);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseItemEx, player->getID(),
		fromPos, fromStackpos, fromSpriteId, toPos, toStackpos, toSpriteId, isHotkey);
}

void ProtocolGame::parseBattleWindow(NetworkMessage& msg)
{
	Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	int16_t fromStackpos = msg.GetByte();
	uint32_t creatureId = msg.GetU32();
	bool isHotkey = (fromPos.x == 0xFFFF && !fromPos.y && !fromPos.z);
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerUseBattleWindow, player->getID(), fromPos, fromStackpos, creatureId, spriteId, isHotkey);
}

void ProtocolGame::parseCloseContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerCloseContainer, player->getID(), cid);
}

void ProtocolGame::parseUpArrowContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerMoveUpContainer, player->getID(), cid);
}

void ProtocolGame::parseUpdateTile(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	//addGameTask(&Game::playerUpdateTile, player->getID(), pos);
}

void ProtocolGame::parseUpdateContainer(NetworkMessage& msg)
{
	uint8_t cid = msg.GetByte();
	addGameTask(&Game::playerUpdateContainer, player->getID(), cid);
}

void ProtocolGame::parseThrow(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    Position fromPos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	int16_t fromStackpos = msg.GetByte();
	Position toPos = msg.GetPosition();
	uint16_t count = msg.GetU16();//65kitem
	if(toPos != fromPos)
		addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerMoveThing,
			player->getID(), fromPos, spriteId, fromStackpos, toPos, count);
}

void ProtocolGame::parseLookAt(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	int16_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookAt, player->getID(), pos, spriteId, stackpos);
}

void ProtocolGame::parseSay(NetworkMessage& msg)
{
	std::string receiver;
	uint16_t channelId = 0;

	SpeakClasses type = (SpeakClasses)msg.GetByte();
	switch(type)
	{
		case SPEAK_PRIVATE:
		case SPEAK_PRIVATE_RED:
		case SPEAK_RVR_ANSWER:
			receiver = msg.GetString();
			break;

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_RN:
		case SPEAK_CHANNEL_RA:
			channelId = msg.GetU16();
			break;

		default:
			break;
	}

	const std::string text = msg.GetString();
	if(text.length() > 255) //client limit
	{
		std::stringstream s;
		s << text.length();

		Logger::getInstance()->eFile("bots/" + player->getName() + ".log", "Attempt to send message with size " + s.str() + " - client is limited to 255 characters.", true);
		return;
	}

	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSay, player->getID(), channelId, type, receiver, text);
}

void ProtocolGame::parseFightModes(NetworkMessage& msg)
{
	uint8_t rawFightMode = msg.GetByte(); //1 - offensive, 2 - balanced, 3 - defensive
	uint8_t rawChaseMode = msg.GetByte(); //0 - stand while fightning, 1 - chase opponent
	uint8_t rawSecureMode = msg.GetByte(); //0 - can't attack unmarked, 1 - can attack unmarked

	chaseMode_t chaseMode = CHASEMODE_STANDSTILL;
	if(rawChaseMode == 1)
		chaseMode = CHASEMODE_FOLLOW;

	fightMode_t fightMode = FIGHTMODE_ATTACK;
	if(rawFightMode == 2)
		fightMode = FIGHTMODE_BALANCED;
	else if(rawFightMode == 3)
		fightMode = FIGHTMODE_DEFENSE;

	secureMode_t secureMode = SECUREMODE_OFF;
	if(rawSecureMode == 1)
		secureMode = SECUREMODE_ON;

	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSetFightModes, player->getID(), fightMode, chaseMode, secureMode);
}

void ProtocolGame::parseAttack(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    uint32_t creatureId = msg.GetU32();
	addGameTask(&Game::playerSetAttackedCreature, player->getID(), creatureId);
}

void ProtocolGame::parseFollow(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    uint32_t creatureId = msg.GetU32();
	addGameTask(&Game::playerFollowCreature, player->getID(), creatureId);
}

void ProtocolGame::parseTextWindow(NetworkMessage& msg)
{
	uint32_t windowTextId = msg.GetU32();
	const std::string newText = msg.GetString();
	addGameTask(&Game::playerWriteItem, player->getID(), windowTextId, newText);
}

void ProtocolGame::parseHouseWindow(NetworkMessage &msg)
{
	uint8_t doorId = msg.GetByte();
	uint32_t id = msg.GetU32();
	const std::string text = msg.GetString();
	addGameTask(&Game::playerUpdateHouseWindow, player->getID(), doorId, id, text);
}

void ProtocolGame::parseLookInShop(NetworkMessage &msg)
{
	uint16_t id = msg.GetU16();
	uint16_t count = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInShop, player->getID(), id, count);
}

void ProtocolGame::parsePlayerPurchase(NetworkMessage &msg)
{
	uint16_t id = msg.GetU16();
	uint16_t count = msg.GetByte();
	uint16_t amount = msg.GetByte();
	bool ignoreCap = msg.GetByte();
	bool inBackpacks = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerPurchaseItem, player->getID(), id, count, amount, ignoreCap, inBackpacks);
}

void ProtocolGame::parsePlayerSale(NetworkMessage &msg)
{
	uint16_t id = msg.GetU16();
	uint16_t count = msg.GetByte();
	uint16_t amount = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerSellItem, player->getID(), id, count, amount);
}

void ProtocolGame::parseCloseShop(NetworkMessage &msg)
{
	addGameTask(&Game::playerCloseShop, player->getID());
}

void ProtocolGame::parseRequestTrade(NetworkMessage& msg)
{
	Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	int16_t stackpos = msg.GetByte();
	uint32_t playerId = msg.GetU32();
	addGameTask(&Game::playerRequestTrade, player->getID(), pos, stackpos, playerId, spriteId);
}

void ProtocolGame::parseAcceptTrade(NetworkMessage& msg)
{
	addGameTask(&Game::playerAcceptTrade, player->getID());
}

void ProtocolGame::parseLookInTrade(NetworkMessage& msg)
{
	bool counter = msg.GetByte();
	int32_t index = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerLookInTrade, player->getID(), counter, index);
}

void ProtocolGame::parseCloseTrade()
{
	addGameTask(&Game::playerCloseTrade, player->getID());
}

void ProtocolGame::parseAddVip(NetworkMessage& msg)
{
	const std::string name = msg.GetString();
	if(name.size() > 32)
		return;

	addGameTask(&Game::playerRequestAddVip, player->getID(), name);
}

void ProtocolGame::parseRemoveVip(NetworkMessage& msg)
{
	uint32_t guid = msg.GetU32();
	addGameTask(&Game::playerRequestRemoveVip, player->getID(), guid);
}

void ProtocolGame::parseRotateItem(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    Position pos = msg.GetPosition();
	uint16_t spriteId = msg.GetSpriteId();
	int16_t stackpos = msg.GetByte();
	addGameTaskTimed(DISPATCHER_TASK_EXPIRATION, &Game::playerRotateItem, player->getID(), pos, stackpos, spriteId);
}

void ProtocolGame::parseDebugAssert(NetworkMessage& msg)
{
	if(m_debugAssertSent)
		return;

	std::stringstream s;
	s << "----- " << formatDate() << " - " << player->getName() << " (" << convertIPAddress(getIP())
		<< ") -----" << std::endl << msg.GetString() << std::endl << msg.GetString()
		<< std::endl << msg.GetString() << std::endl << msg.GetString()
		<< std::endl << std::endl;

	m_debugAssertSent = true;
	Logger::getInstance()->iFile(LOGFILE_CLIENT_ASSERTION, s.str(), false);
}

void ProtocolGame::parseBugReport(NetworkMessage& msg)
{
	std::string comment = msg.GetString();
    addGameTask(&Game::playerReportBug, player->getID(), comment);
}

void ProtocolGame::parseInviteToParty(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerInviteToParty, player->getID(), targetId);
}

void ProtocolGame::parseJoinParty(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerJoinParty, player->getID(), targetId);
}

void ProtocolGame::parseRevokePartyInvite(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerRevokePartyInvitation, player->getID(), targetId);
}

void ProtocolGame::parsePassPartyLeadership(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    uint32_t targetId = msg.GetU32();
	addGameTask(&Game::playerPassPartyLeadership, player->getID(), targetId);
}

void ProtocolGame::parseLeaveParty(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    addGameTask(&Game::playerLeaveParty, player->getID());
}

void ProtocolGame::parseSharePartyExperience(NetworkMessage& msg)
{
	if(tvOwner)
        return;
    bool activate = msg.GetByte();
	uint8_t unknown = msg.GetByte(); //TODO: find out what is this byte
	addGameTask(&Game::playerSharePartyExperience, player->getID(), activate, unknown);
}

// void ProtocolGame::parseMarketLeave()
// {
	// addGameTask(&Game::playerLeaveMarket, player->getID());
// }

// void ProtocolGame::parseMarketBrowse(NetworkMessage& msg)
// {
	// uint16_t browseId = msg.GetU16();

	// if (browseId == MARKETREQUEST_OWN_OFFERS) {
		// addGameTask(&Game::playerBrowseMarketOwnOffers, player->getID());
	// } else if (browseId == MARKETREQUEST_OWN_HISTORY) {
		// addGameTask(&Game::playerBrowseMarketOwnHistory, player->getID());
	// } else {
		// addGameTask(&Game::playerBrowseMarket, player->getID(), browseId);
	// }
// }

// void ProtocolGame::parseMarketCreateOffer(NetworkMessage& msg)
// {
	// uint8_t type = msg.GetByte();
	// uint16_t spriteId = msg.GetU16();
	// uint16_t amount = msg.GetU16();
	// uint32_t price = msg.GetU32();
	// bool anonymous = (msg.GetByte() != 0);
	// addGameTask(&Game::playerCreateMarketOffer, player->getID(), type, spriteId, amount, price, anonymous);
// }

// void ProtocolGame::parseMarketCancelOffer(NetworkMessage& msg)
// {
	// uint32_t timestamp = msg.GetU32();
	// uint16_t counter = msg.GetU16();
	// addGameTask(&Game::playerCancelMarketOffer, player->getID(), timestamp, counter);
// }

// void ProtocolGame::parseMarketAcceptOffer(NetworkMessage& msg)
// {
	// uint32_t timestamp = msg.GetU32();
	// uint16_t counter = msg.GetU16();
	// uint16_t amount = msg.GetU16();
	// addGameTask(&Game::playerAcceptMarketOffer, player->getID(), timestamp, counter, amount);
// }

void ProtocolGame::parseQuests(NetworkMessage& msg)
{
	addGameTask(&Game::playerQuests, player->getID());
}

void ProtocolGame::parseQuestInfo(NetworkMessage& msg)
{
	uint16_t questId = msg.GetU16();
	addGameTask(&Game::playerQuestInfo, player->getID(), questId);
}

void ProtocolGame::parseViolationWindow(NetworkMessage& msg)
{
	std::string target = msg.GetString();
	uint8_t reason = msg.GetByte();
	ViolationAction_t action = (ViolationAction_t)msg.GetByte();
	std::string comment = msg.GetString();
	std::string statement = msg.GetString();
	uint32_t statementId = (uint32_t)msg.GetU16();
	bool ipBanishment = msg.GetByte();
	addGameTask(&Game::playerViolationWindow, player->getID(), target, reason, action, comment, statement, statementId, ipBanishment);
}

void ProtocolGame::parseRequestPollWindow(NetworkMessage& msg)
{
    addGameTask(&Game::playerRequestPollWindow, player->getID());
}

void ProtocolGame::parsePollVote(NetworkMessage& msg)
{
    Poll* poll = Polls::getInstance()->getAvailablePollForAccount(player->getAccount());
    if (!poll) {
        return;
    }
    
    if (poll->getTextMode()) {
        std::string text = msg.GetString();
        addGameTask(&Game::playerPollVoteText, player->getID(), text);
    }
    else {    
        uint8_t pollVote = msg.GetByte();
        addGameTask(&Game::playerPollVote, player->getID(), pollVote);
    }
}

void ProtocolGame::parseExtendedOpcode(NetworkMessage& msg)
{
    uint8_t opcode = msg.GetByte();
    std::string buffer = msg.GetString();
    
    // process additional opcodes via lua script event
    addGameTask(&Game::parsePlayerExtendedOpcode, player->getID(), opcode, buffer);
}

void ProtocolGame::sendExtendedOpcode(uint8_t opcode, const std::string& buffer)
{
    // extended opcodes can only be send to players using otclient, cipsoft's tibia can't understand them
    if(player && !player->isUsingOtclient()) {
        return;
    }
    
    NetworkMessage_ptr msg = getOutputBuffer();
    if(msg) {
		TRACK_MESSAGE(msg);
		msg->AddByte(0x32);
		msg->AddByte(opcode);
		msg->AddString(buffer);
    }
}

//********************** Send methods *******************************//
void ProtocolGame::sendOpenPrivateChannel(const std::string& receiver)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("openPrivateChannel");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAD);
		msg->AddString(receiver);
	}
}

void ProtocolGame::sendCreatureOutfit(const Creature* creature, const Outfit_t& outfit)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureOutfit");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x8E);
		msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
		AddCreatureOutfit(msg, creature, outfit);
	}
	
	if(tvChannel)
        tvChannel->replayCreatureOutfit(creature, outfit);
}

void ProtocolGame::sendCreatureLight(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureLight");
#endif
        TRACK_MESSAGE(msg);
		AddCreatureLight(msg, creature);
	}
	
	if(tvChannel)
        tvChannel->replayCreatureLight(creature);
}

void ProtocolGame::sendWorldLight(const LightInfo& lightInfo)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("worldLight");
#endif
        TRACK_MESSAGE(msg);
		AddWorldLight(msg, lightInfo);
	}
	
	if(tvChannel)
        tvChannel->replayWorldLight(lightInfo);
}

void ProtocolGame::sendCreatureShield(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureShield");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x91);
		msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
		msg->AddByte(player->getPartyShield(creature));
	}
	
	if(tvChannel)
        tvChannel->replayCreatureShield(creature);
}

void ProtocolGame::sendCreatureSkull(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureSkull");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x90);
		msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
		msg->AddByte(player->getSkullClient(creature));
	}
	
	if(tvChannel)
        tvChannel->replayCreatureSkull(creature);
}

void ProtocolGame::sendCreatureSquare(const Creature* creature, SquareColor_t color)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureSquare");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x86);
		msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
		msg->AddByte((uint8_t)color);
	}
}

void ProtocolGame::sendTutorial(uint8_t tutorialId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("tutorial");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xDC);
		msg->AddByte(tutorialId);
	}
}

void ProtocolGame::sendAddMarker(const Position& pos, MapMarks_t markType, const std::string& desc)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("addMarker");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xDD);
		msg->AddPosition(pos);
		msg->AddByte(markType);
		msg->AddString(desc);
	}
}

void ProtocolGame::sendReLoginWindow()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("reLoginWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x28);
	}
}

void ProtocolGame::sendStats()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("stats");
#endif
        TRACK_MESSAGE(msg);
		AddPlayerStats(msg);
	}
}

void ProtocolGame::sendTextMessage(MessageClasses mClass, const std::string& message)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("textMessage");
#endif
        TRACK_MESSAGE(msg);
		AddTextMessage(msg, mClass, message);
	}
}

void ProtocolGame::sendClosePrivate(uint16_t channelId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("closePrivate");
#endif
        TRACK_MESSAGE(msg);
		if(channelId == CHANNEL_GUILD || channelId == CHANNEL_PARTY)
			g_chat.removeUserFromChannel(player, channelId);

		msg->AddByte(0xB3);
		msg->AddU16(channelId);
	}
}

void ProtocolGame::sendCreatePrivateChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("createPrivateChannel");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xB2);
		msg->AddU16(channelId);
		msg->AddString(channelName);
	}
}

void ProtocolGame::sendChannelsDialog()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("channelsDialog");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAB);
		ChannelList list = g_chat.getChannelList(player);
		msg->AddU16(list.size());
		for(ChannelList::iterator it = list.begin(); it != list.end(); ++it)
		{
			if(ChatChannel* channel = (*it))
			{
				msg->AddU16(channel->getId());
				msg->AddString(channel->getName());
			}
		}
	}
}

void ProtocolGame::sendChannelsDialog(std::vector<uint16_t> channelsIds) // PS
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("channelsDialog[2]");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAB);
		
		ChannelList list;
		std::vector<uint16_t>::const_iterator it;
        for(it = channelsIds.begin(); it != channelsIds.end(); it++)
        {
            if(ChatChannel* channel = g_chat.getChannel(player, *it))
                list.push_back(channel);
        }
		
		msg->AddU16(list.size());
		for(ChannelList::iterator it = list.begin(); it != list.end(); ++it)
		{
			if(ChatChannel* channel = (*it))
			{
				msg->AddU16(channel->getId());
				msg->AddString(channel->getName());
			}
		}
	}
}

void ProtocolGame::sendTVChannelsDialog()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("tvChannelsDialog");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAB);
		
		ChannelList list = g_chat.getTVChannelList();
		if(list.size() > 0)
		{
            msg->AddU16(list.size());
		    for(ChannelList::iterator it = list.begin(); it != list.end(); ++it)
		    {
			    if(ChatChannel* channel = (*it))
			    {
				    msg->AddU16(channel->getId());
				    msg->AddString(channel->getName());
			    }
		    }
        }
		
		else
		{
            msg->AddU16(1);
            msg->AddU16(CHANNEL_TV);
            msg->AddString((player ? Localization::t(player->getLanguage(), "No TV channels available") : "No TV channels available"));
        }
	}
}

void ProtocolGame::sendChannel(uint16_t channelId, const std::string& channelName)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("channel");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAC);
		msg->AddU16(channelId);
		msg->AddString(channelName);
	}
}

void ProtocolGame::sendRuleViolationsChannel(uint16_t channelId)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("ruleViolationsChannel");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAE);
		msg->AddU16(channelId);
		for(RuleViolationsMap::const_iterator it = g_game.getRuleViolations().begin(); it != g_game.getRuleViolations().end(); ++it)
		{
			RuleViolation& rvr = *it->second;
			if(rvr.isOpen && rvr.reporter)
				AddCreatureSpeak(msg, rvr.reporter, SPEAK_RVR_CHANNEL, rvr.text, channelId, rvr.time);
		}
	}
}

void ProtocolGame::sendRemoveReport(const std::string& name)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("removeReport");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xAF);
		msg->AddString(name);
	}
}

void ProtocolGame::sendRuleViolationCancel(const std::string& name)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("ruleViolationCancel");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xB0);
		msg->AddString(name);
	}
}

void ProtocolGame::sendLockRuleViolation()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("lockRuleViolation");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xB1);
	}
}

void ProtocolGame::sendIcons(int32_t icons)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("icons");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xA2);
		msg->AddU16(icons);
	}
}

void ProtocolGame::sendContainer(uint32_t cid, const Container* container, bool hasParent)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("container");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x6E);
		msg->AddByte(cid);

		msg->AddItemId(container);
		msg->AddString(container->getName());
		msg->AddByte(container->capacity());

		msg->AddByte(hasParent ? 0x01 : 0x00);
		msg->AddByte(std::min(container->size(), (uint32_t)255));

		ItemList::const_iterator cit = container->getItems();
		for(uint32_t i = 0; cit != container->getEnd() && i < 255; ++cit, ++i){
			msg->AddItem(*cit);
			Item* item = *cit;
		    std::string pname = "none";
			if(item->getStringAttribute("10002"))
			{
				const std::string* namePoke = (*cit)->getStringAttribute("10002");
				pname = *namePoke;
			}
			msg->AddString(pname);
			if(pname != "none"){
			    uint32_t level = 0;
			    if(item->getIntegerAttribute("10005"))
			    {
			    	const int32_t* levelPoke = (*cit)->getIntegerAttribute("10005");
			    	level = *levelPoke;
			    }
			    msg->AddU32(level);
		        uint32_t gender = 0;
			    if(item->getIntegerAttribute("10016"))
			    {
			    	const int32_t* genderPoke = (*cit)->getIntegerAttribute("10016");
			    	gender = *genderPoke;
			    }
			    msg->AddU32(gender);
			}
		}
	}
}

void ProtocolGame::sendShop(const ShopInfoList& shop)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("shop");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x7A);
		msg->AddByte(std::min(shop.size(), (size_t)255));

		ShopInfoList::const_iterator it = shop.begin();
		for(uint32_t i = 0; it != shop.end() && i < 255; ++it, ++i)
			AddShopItem(msg, (*it));
	}
}

void ProtocolGame::sendCloseShop()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("closeShop");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x7C);
	}
}

void ProtocolGame::sendGoods(const ShopInfoList& shop)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("goods");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x7B);
		msg->AddU32(g_game.getMoney(player));

		std::map<uint32_t, uint32_t> goodsMap;
		if(shop.size() >= 5)
		{
			for(ShopInfoList::const_iterator sit = shop.begin(); sit != shop.end(); ++sit)
			{
				if(sit->sellPrice < 0)
					continue;

				int8_t subType = -1;
				if(sit->subType)
				{
					const ItemType& it = Item::items[sit->itemId];
					if(it.hasSubType() && !it.stackable)
						subType = sit->subType;
				}

				uint32_t count = player->__getItemTypeCount(sit->itemId, subType);
				if(count > 0)
					goodsMap[sit->itemId] = count;
			}
		}
		else
		{
			std::map<uint32_t, uint32_t> tmpMap;
			player->__getAllItemTypeCount(tmpMap);
			for(ShopInfoList::const_iterator sit = shop.begin(); sit != shop.end(); ++sit)
			{
				if(sit->sellPrice < 0)
					continue;

				int8_t subType = -1;
				if(sit->subType)
				{
					const ItemType& it = Item::items[sit->itemId];
					if(it.hasSubType() && !it.stackable)
						subType = sit->subType;
				}

				if(subType != -1)
				{
					uint32_t count = player->__getItemTypeCount(sit->itemId, subType);
					if(count > 0)
						goodsMap[sit->itemId] = count;
				}
				else
					goodsMap[sit->itemId] = tmpMap[sit->itemId];
			}
		}

		msg->AddByte(std::min(goodsMap.size(), (size_t)255));
		std::map<uint32_t, uint32_t>::const_iterator it = goodsMap.begin();
		for(uint32_t i = 0; it != goodsMap.end() && i < 255; ++it, ++i)
		{
			msg->AddItemId(it->first);
			msg->AddByte(std::min(it->second, (uint32_t)255));
		}
	}
}

void ProtocolGame::sendTradeItemRequest(const Player* player, const Item* item, bool ack)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("tradeItemRequest");
#endif
        TRACK_MESSAGE(msg);
		if(ack)
			msg->AddByte(0x7D);
		else
			msg->AddByte(0x7E);

		msg->AddString(player->getName());
		if(const Container* container = item->getContainer())
		{
			msg->AddByte(container->getItemHoldingCount() + 1);
			msg->AddItem(item);
			for(ContainerIterator it = container->begin(); it != container->end(); ++it)
				msg->AddItem(*it);
		}
		else
		{
			msg->AddByte(1);
			msg->AddItem(item);
		}
	}
}

void ProtocolGame::sendCloseTrade()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("closeTrade");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x7F);
	}
}

void ProtocolGame::sendCloseContainer(uint32_t cid)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("closeContainer");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x6F);
		msg->AddByte(cid);
	}
}

void ProtocolGame::sendCreatureTurn(const Creature* creature, int16_t stackpos)
{
    if(stackpos >= 10 || !canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureTurn");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x6B);
		msg->AddPosition(creature->getPosition());
		msg->AddByte(stackpos);
		msg->AddU16(0x63); /*99*/
		msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
		msg->AddByte(creature->getDirection());
	}
	
	if(tvChannel)
        tvChannel->replayCreatureTurn(creature, stackpos);
}

void ProtocolGame::sendCreatureSay(const Creature* creature, SpeakClasses type, const std::string& text, Position* pos/* = NULL*/)
{
    NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureSay");
#endif
        TRACK_MESSAGE(msg);
		AddCreatureSpeak(msg, creature, type, text, 0, 0, pos);
	}
	
	if(tvChannel && (type == SPEAK_SAY || type == SPEAK_MONSTER_YELL || type == SPEAK_MONSTER_SAY))
        tvChannel->replayCreatureSay(creature, type, text, pos);
}

void ProtocolGame::sendToChannel(const Creature* creature, SpeakClasses type, const std::string& text, uint16_t channelId, uint32_t time /*= 0*/)
{
    NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("toChannel");
#endif
        TRACK_MESSAGE(msg);
		AddCreatureSpeak(msg, creature, type, text, channelId, time);
	}
}

void ProtocolGame::sendCancel(const std::string& message)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("cancel");
#endif
        TRACK_MESSAGE(msg);
		AddTextMessage(msg, MSG_STATUS_SMALL, message);
	}
}

void ProtocolGame::sendCancelTarget()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("cancelTarget");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xA3);
	}
}

void ProtocolGame::sendChangeSpeed(const Creature* creature, uint32_t speed)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("changeSpeed");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x8F);
		msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
		msg->AddU16(speed);
	}
}

void ProtocolGame::sendCancelWalk()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("cancelWalk");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xB5);
		msg->AddByte(player->getDirection());
	}
}

void ProtocolGame::sendSkills()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("skills");
#endif
        TRACK_MESSAGE(msg);
		AddPlayerSkills(msg);
	}
}

void ProtocolGame::sendPing()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("ping");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x1E);
	}
}

void ProtocolGame::sendDistanceShoot(const Position& from, const Position& to, uint8_t type)
{
    //if(type > SHOOT_EFFECT_LAST || (!canSee(from) && !canSee(to)))
	if(type > 254 || (!canSee(from) && !canSee(to))) // PS
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("distanceShoot");
#endif
        TRACK_MESSAGE(msg);
		AddDistanceShoot(msg, from, to, type);
	}
    	
    if(tvChannel)
        tvChannel->replayDistanceShoot(from, to, type);
}

void ProtocolGame::sendMagicEffect(const Position& pos, uint16_t type)
{
    if(!canSee(pos)) // PS //if(type > MAGIC_EFFECT_LAST || !canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("magicEffect");
#endif
        TRACK_MESSAGE(msg);
		AddMagicEffect(msg, pos, type);
	}
	
	if(tvChannel)
        tvChannel->replayMagicEffect(pos, type);
}

void ProtocolGame::sendAnimatedText(const Position& pos, uint8_t color, std::string text)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("animatedText");
#endif
        TRACK_MESSAGE(msg);
		AddAnimatedText(msg, pos, color, text);
	}
	
	if(tvChannel)
        tvChannel->replayAnimatedText(pos, color, text);
}

void ProtocolGame::sendCreatureHealth(const Creature* creature)
{
	if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("creatureHealth");
#endif
        TRACK_MESSAGE(msg);
		AddCreatureHealth(msg, creature);
	}
	
	if(tvChannel)
        tvChannel->replayCreatureHealth(creature);
}

void ProtocolGame::sendFYIBox(const std::string& message)
{
	if(message.empty() || message.length() > 1018) //Prevent client debug when message is empty or length is > 1018 (not confirmed)
	{
		std::cout << "[Warning - ProtocolGame::sendFYIBox] Trying to send an empty or too huge message." << std::endl;
		return;
	}

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("fyiBox");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x15);
		msg->AddString(message);
	}
}

//tile
void ProtocolGame::sendAddTileItem(const Tile* tile, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(!canSee(pos))
		return;
    
    NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("addTileItem");
#endif
        TRACK_MESSAGE(msg);
		AddTileItem(msg, pos, stackpos, item);
	}
	
	if(tvChannel)
        tvChannel->replayAddTileItem(tile, pos, stackpos, item);
}

void ProtocolGame::sendUpdateTileItem(const Tile* tile, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("updateTileItem");
#endif
        TRACK_MESSAGE(msg);
		UpdateTileItem(msg, pos, stackpos, item);
	}
	
	if(tvChannel)
        tvChannel->replayUpdateTileItem(tile, pos, stackpos, item);
}

void ProtocolGame::sendRemoveTileItem(const Tile* tile, const Position& pos, uint32_t stackpos)
{
    if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("removeTileItem");
#endif
        TRACK_MESSAGE(msg);
		RemoveTileItem(msg, pos, stackpos);
	}
	
	if(tvChannel)
        tvChannel->replayRemoveTileItem(tile, pos, stackpos);
}

void ProtocolGame::sendUpdateTile(const Tile* tile, const Position& pos)
{
	if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("updateTile");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x69);
		msg->AddPosition(pos);
		if(tile)
		{
			GetTileDescription(tile, msg);
			msg->AddByte(0x00);
			msg->AddByte(0xFF);
		}
		else
		{
			msg->AddByte(0x01);
			msg->AddByte(0xFF);
		}
	}
	
	if(tvChannel)
        tvChannel->replayUpdateTile(tile, pos);
}

void ProtocolGame::sendAddCreature(const Creature* creature, const Position& pos, uint32_t stackpos)
{
    if(!canSee(creature))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;
		
#ifdef __LOG_PACKETS__
    recordPacket("addCreature");
#endif
		
	if(tvChannel)
        tvChannel->replayAddCreature(creature, pos, stackpos);

	TRACK_MESSAGE(msg);
	if(creature != player || tvOwner)
	{
		AddTileCreature(msg, pos, stackpos, creature);
		return;
	}

	msg->AddByte(0x0A);
	msg->AddU32(player->getID());
	msg->AddU16(0x32);

	msg->AddByte(player->hasFlag(PlayerFlag_CanReportBugs));
	if (player->isUsingOtclient()) {
        msg->AddU16(g_game.getRealLightHour());
    }
	
	if(Group* group = player->getGroup())
	{
		int32_t reasons = group->getViolationReasons();
		if(reasons > 1)
		{
			msg->AddByte(0x0B);
			for(int32_t i = 0; i < 20; ++i)
			{
				if(i < 4)
					msg->AddByte(group->getNameViolationFlags());
				else if(i < reasons)
					msg->AddByte(group->getStatementViolationFlags());
				else
					msg->AddByte(0x00);
			}
		}
	}

	AddMapDescription(msg, pos);
	for(int32_t i = SLOT_FIRST; i < SLOT_LAST; ++i)
		AddInventoryItem(msg, (slots_t)i, player->getInventoryItem((slots_t)i));

	AddPlayerStats(msg);
	AddPlayerSkills(msg);

	//gameworld light-settings
	LightInfo lightInfo;
	g_game.getWorldLightInfo(lightInfo);
	AddWorldLight(msg, lightInfo);

	//player light level
	AddCreatureLight(msg, creature);
	player->sendIcons();
	for(VIPListSet::iterator it = player->VIPList.begin(); it != player->VIPList.end(); it++)
	{
		std::string vipName;
		if(IOLoginData::getInstance()->getNameByGuid((*it), vipName))
		{
			Player* tmpPlayer = g_game.getPlayerByName(vipName);
			sendVIP((*it), vipName, (tmpPlayer && player->canSeeCreature(tmpPlayer)));
		}
	}
}

void ProtocolGame::sendRemoveCreature(const Creature* creature, const Position& pos, uint32_t stackpos)
{
    if(!canSee(pos))
		return;

	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("removeCreature");
#endif
        TRACK_MESSAGE(msg);
		RemoveTileItem(msg, pos, stackpos);
	}
	
	if(tvChannel)
        tvChannel->replayRemoveCreature(creature, pos, stackpos);
}

void ProtocolGame::sendUpdateCreature(const Creature* creature, const Position& pos, uint32_t stackpos)
{     
#ifdef __LOG_PACKETS__
    recordPacket("updateCreature");
#endif
    
    std::list<uint32_t>::iterator known = std::find(knownCreatureList.begin(), knownCreatureList.end(), /*creature->getID()*/getCreatureID(creature));
    if(known != knownCreatureList.end()) // I've seen this guy before, we need to do something!
	{        
        if(canSee(pos)) // We can see him! Get that bugger!
		{
            if(creature == player || (tvOwner && creature == tvOwner))
			{
                NetworkMessage_ptr msg = getOutputBuffer();
                RemoveTileItem(msg, pos, stackpos);
                removeKnownCreature(/*creature->getID()*/getCreatureID(creature));
                AddMapDescription(msg, pos);
            }
            else // I'm not looking at myself here!
		    {
                sendRemoveCreature(creature, pos, stackpos);
			    removeKnownCreature(/*creature->getID()*/getCreatureID(creature));
			    sendAddCreature(creature, pos, stackpos);
            }
        }
    }
    else // We can't see him, so we might as well get rid of him
    {
        //removeKnownCreature(creature->getID());
        //std::cout << "ATENCAO - known esta no fim da lista" << std::endl;
        ///sendRemoveCreature(creature, pos, stackpos);
        sendAddCreature(creature, pos, stackpos);
    }    
    
    if(tvChannel)
        tvChannel->replayUpdateCreature(creature, pos, stackpos);
}

void ProtocolGame::sendMoveCreature(const Creature* creature, const Tile* newTile, const Position& newPos,
	uint32_t newStackpos, const Tile* oldTile, const Position& oldPos, uint32_t oldStackpos, bool teleport)
{           
#ifdef __LOG_PACKETS__
    recordPacket("moveCreature");
#endif
    
    if(creature == player || (tvOwner && creature == tvOwner))
	{
        NetworkMessage_ptr msg = getOutputBuffer();
		if(msg)
		{
            TRACK_MESSAGE(msg);
			if(teleport || oldStackpos >= 10)
			{
                RemoveTileItem(msg, oldPos, oldStackpos);
				AddMapDescription(msg, newPos);
			}
			else
			{
                if(oldPos.z != 7 || newPos.z < 8)
				{
                    msg->AddByte(0x6D);
					msg->AddPosition(oldPos);
					msg->AddByte(oldStackpos);
					msg->AddPosition(newPos);
				}
				else
                    RemoveTileItem(msg, oldPos, oldStackpos);

				if(newPos.z > oldPos.z)
					MoveDownCreature(msg, creature, newPos, oldPos, oldStackpos);
				else if(newPos.z < oldPos.z)
					MoveUpCreature(msg, creature, newPos, oldPos, oldStackpos);

				if (oldPos.y > newPos.y) { // north, for old x
				msg->AddByte(0x65);
				GetMapDescription(oldPos.x - Map::maxClientViewportX, newPos.y - Map::maxClientViewportY, newPos.z, (Map::maxClientViewportX+1)*2, 1, msg);
				} 
				else if (oldPos.y < newPos.y) { // south, for old x
				msg->AddByte(0x67);
				GetMapDescription(oldPos.x - Map::maxClientViewportX, newPos.y + (Map::maxClientViewportY+1), newPos.z, (Map::maxClientViewportX+1)*2, 1, msg);
				}
				if (oldPos.x < newPos.x) { // east, [with new y]
				msg->AddByte(0x66);
				GetMapDescription(newPos.x + (Map::maxClientViewportX+1), newPos.y - Map::maxClientViewportY, newPos.z, 1, (Map::maxClientViewportY+1)*2, msg);
				} 
				else if (oldPos.x > newPos.x) { // west, [with new y]
				msg->AddByte(0x68);
				GetMapDescription(newPos.x - Map::maxClientViewportX, newPos.y - Map::maxClientViewportY, newPos.z, 1, (Map::maxClientViewportY+1)*2, msg);
				}
			}
		}
		if(tvChannel)
            tvChannel->replayMoveCreature(creature, newTile, newPos, newStackpos, oldTile, oldPos, oldStackpos, teleport);
	}
	else if(canSee(oldPos) && canSee(newPos))
	{
        if(!player->canSeeCreature(creature))
			return;

		NetworkMessage_ptr msg = getOutputBuffer();
		if(msg)
		{
            TRACK_MESSAGE(msg);
			if(!teleport && (oldPos.z != 7 || newPos.z < 8) && oldStackpos < 10)
			{
                msg->AddByte(0x6D);
				msg->AddPosition(oldPos);
				msg->AddByte(oldStackpos);
				msg->AddPosition(newPos);
			}
			else
			{
                RemoveTileItem(msg, oldPos, oldStackpos);
				AddTileCreature(msg, newPos, newStackpos, creature);
			}
		}
		if(tvChannel)
            tvChannel->replayMoveCreature(creature, newTile, newPos, newStackpos, oldTile, oldPos, oldStackpos, teleport);
	}
	else if(canSee(oldPos))
	{
        if(!player->canSeeCreature(creature))
			return;

		NetworkMessage_ptr msg = getOutputBuffer();
		if(msg)
		{
            TRACK_MESSAGE(msg);
			RemoveTileItem(msg, oldPos, oldStackpos);
		}
		if(tvChannel)
            tvChannel->replayMoveCreature(creature, newTile, newPos, newStackpos, oldTile, oldPos, oldStackpos, teleport);
	}
	else if(canSee(newPos) && player->canSeeCreature(creature))
	{
        NetworkMessage_ptr msg = getOutputBuffer();
		if(msg)
		{
            TRACK_MESSAGE(msg);
			AddTileCreature(msg, newPos, newStackpos, creature);
		}
		if(tvChannel)
            tvChannel->replayMoveCreature(creature, newTile, newPos, newStackpos, oldTile, oldPos, oldStackpos, teleport);
	}
}

//inventory
void ProtocolGame::sendAddInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("addInventoryItem");
#endif
        TRACK_MESSAGE(msg);
		AddInventoryItem(msg, slot, item);
	}
}

void ProtocolGame::sendUpdateInventoryItem(slots_t slot, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("updateInventoryItem");
#endif
        TRACK_MESSAGE(msg);
		UpdateInventoryItem(msg, slot, item);
	}
}

void ProtocolGame::sendRemoveInventoryItem(slots_t slot)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("removeInventoryItem");
#endif
        TRACK_MESSAGE(msg);
		RemoveInventoryItem(msg, slot);
	}
}

void ProtocolGame::sendPokemonSkills(uint16_t iconItemId, std::vector<uint16_t> skills) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonSkills");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x1);
        output->AddU16(iconItemId);
        
        output->AddByte(skills.size());
        std::vector<uint16_t>::const_iterator it;
        for(it = skills.begin(); it != skills.end(); it++)
        {
            output->AddU16(*it);
        }
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonSkillContainerClose() // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonSkillContainerClose");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x2);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonSkillContainerOpen() // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonSkillContainerOpen");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x3);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonWindowAddPokemonIcon(uint16_t itemId, uint16_t fastcallNumber, uint8_t textColor, std::string text, uint8_t pokeLevel, uint16_t pokeMaxMana, uint16_t pokeMana, uint16_t pokeGender, uint8_t pokeExp)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonWindowAddPokemonIcon");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x4);
        output->AddU16(itemId);
        output->AddU16(fastcallNumber);
        output->AddByte(textColor);
        output->AddString(text);
        output->AddByte(pokeLevel);
        output->AddU16(pokeMaxMana);
        output->AddU16(pokeMana);
        output->AddU16(pokeGender);
        output->AddByte(pokeExp);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonWindowRemovePokemonIcon(uint16_t fastcallNumber)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonWindowRemovePokemonIcon");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x5);
        output->AddU16(fastcallNumber);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonWindowUpdatePokemonIcon(uint16_t fastcallNumber, uint8_t textColor, std::string newText, uint8_t pokeLevel, uint16_t pokeMaxMana, uint16_t pokeMana, uint16_t pokeGender, uint8_t pokeExp)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonWindowUpdatePokemonIcon");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x6);
        output->AddU16(fastcallNumber);
        output->AddByte(textColor);
        output->AddString(newText);
        output->AddByte(pokeLevel);
        output->AddU16(pokeMaxMana);
        output->AddU16(pokeMana);
        output->AddU16(pokeGender);
        output->AddByte(pokeExp);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonWindowOpen()
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonWindowOpen");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x7);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonWindowClose()
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonWindowClose");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x8);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonSkillCooldown(uint16_t itemId, uint8_t cooldown)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonSkillCooldown");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x9);
        output->AddU16(itemId);
        output->AddByte(cooldown);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokedexStatus(std::vector<uint8_t>status) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokedexStatus");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0xA);
        
        output->AddU16(status.size());
        std::vector<uint8_t>::const_iterator it;
        for(it = status.begin(); it != status.end(); it++)
            output->AddByte(*it);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokedexOpen() // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false))
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokedexOpen");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0xB);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokedexItemUpdate(uint16_t pokemonNumber, uint8_t status)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false))
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokedexItemUpdate");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0xC);
        output->AddU16(pokemonNumber);
        output->AddByte(status);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokedexInfo(uint16_t pokemonId, std::string details, std::string moves, std::string effectiveness, std::string families)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false))
    {
#ifdef __LOG_PACKETS__
        recordPacket("sendPokedexInfo");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x11);
        output->AddU16(pokemonId);
        output->AddString(details);
        output->AddString(moves);
        output->AddString(effectiveness);
        output->AddString(families);
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendTmWindow(uint16_t tmMove, std::vector<uint16_t>moves)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("tmWindow");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0xD);
        
        output->AddU16(tmMove);
        output->AddByte(moves.size());
        std::vector<uint16_t>::const_iterator it;
        for(it = moves.begin(); it != moves.end(); it++) {
            output->AddU16(*it);
        }
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonStatusAdd(uint16_t itemId, uint8_t cooldown)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonStatusAdd");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0xE);
        
        output->AddU16(itemId);
        output->AddByte(cooldown);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonStatusRemove(uint16_t itemId)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonStatusRemove");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0xF);
        
        output->AddU16(itemId);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonStatusClear()
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pokemonStatusClear");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x10);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendCreatureJump(const Creature* creature)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("creatureJump");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x12);
        output->AddU32(getCreatureID(creature)/*creature->getID()*/);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendCreatureEffect(const Creature* creature, uint8_t effectId, uint32_t var)
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("creatureEffect");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x13);
        output->AddU32(getCreatureID(creature)/*creature->getID()*/);
        output->AddByte(effectId);
        output->AddU32(var);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

//containers
void ProtocolGame::sendAddContainerItem(uint8_t cid, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("addContainerItem");
#endif
        TRACK_MESSAGE(msg);
		AddContainerItem(msg, cid, item);
	}
}

void ProtocolGame::sendUpdateContainerItem(uint8_t cid, uint8_t slot, const Item* item)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("updateContainerItem");
#endif
        TRACK_MESSAGE(msg);
		UpdateContainerItem(msg, cid, slot, item);
	}
}

void ProtocolGame::sendRemoveContainerItem(uint8_t cid, uint8_t slot)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("removeContainerItem");
#endif
        TRACK_MESSAGE(msg);
		RemoveContainerItem(msg, cid, slot);
	}
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, Item* item, uint16_t maxLen, bool canWrite)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("textWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x96);
		msg->AddU32(windowTextId);
		msg->AddItemId(item);
		if(canWrite)
		{
			msg->AddU16(maxLen);
			msg->AddString(item->getText());
		}
		else
		{
			msg->AddU16(item->getText().size());
			msg->AddString(item->getText());
		}

		const std::string& writer = item->getWriter();
		if(writer.size())
			msg->AddString(writer);
		else
			msg->AddString("");

		time_t writtenDate = item->getDate();
		if(writtenDate > 0)
			msg->AddString(formatDate(writtenDate));
		else
			msg->AddString("");
	}
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("textWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x96);
		msg->AddU32(windowTextId);
		msg->AddItemId(itemId);

		msg->AddU16(text.size());
		msg->AddString(text);

		msg->AddString("");
		msg->AddString("");
	}
}

void ProtocolGame::sendTextWindow(uint32_t windowTextId, uint32_t itemId, const std::string& text, uint16_t maxLen,
                                  bool canWrite)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("textWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x96);
		msg->AddU32(windowTextId);
		msg->AddItemId(itemId);
		
		if(canWrite)
		{
			msg->AddU16(maxLen + text.size());
			msg->AddString(text);
		}
		else
		{
			msg->AddU16(text.size());
		    msg->AddString(text);
		}

		msg->AddString("");
		msg->AddString("");
	}
}

void ProtocolGame::sendHouseWindow(uint32_t windowTextId, House* _house,
	uint32_t listId, const std::string& text)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("houseWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0x97);
		msg->AddByte(0x00);
		msg->AddU32(windowTextId);
		msg->AddString(text);
	}
}

void ProtocolGame::sendOutfitWindow()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("outfitWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xC8);
		AddCreatureOutfit(msg, player, player->getDefaultOutfit(), true);

		std::list<Outfit> outfitList;
		for(OutfitMap::iterator it = player->outfits.begin(); it != player->outfits.end(); ++it)
		{
			if(player->canWearOutfit(it->first, it->second.addons))
				outfitList.push_back(it->second);
		}

 		if(outfitList.size())
		{
            uint16_t maxOutfits = (player->isUsingOtclient() ? outfitList.size() : (size_t)std::min((size_t)OUTFITS_MAX_NUMBER, outfitList.size()));
			msg->AddByte(maxOutfits);
			std::list<Outfit>::iterator it = outfitList.begin();
			for(int32_t i = 0; it != outfitList.end() && i < maxOutfits; ++it, ++i)
			{
				msg->AddU16(it->lookType);
				msg->AddString(it->name);
				if(player->hasCustomFlag(PlayerCustomFlag_CanWearAllAddons))
					msg->AddByte(0x03);
				else if(!g_config.getBool(ConfigManager::ADDONS_PREMIUM) || player->isPremium())
					msg->AddByte(it->addons);
				else
					msg->AddByte(0x00);
			}
		}
		else
		{
			msg->AddByte(1);
			msg->AddU16(player->getDefaultOutfit().lookType);
			msg->AddString("Outfit");
			msg->AddByte(player->getDefaultOutfit().lookAddons);
		}

		player->hasRequestedOutfit(true);
	}
}

void ProtocolGame::sendCustomOutfitWindow(const Outfit_t& defaultOutfit, std::list<Outfit>& outfits)
{
	if (!outfits.size()) {
        return;
    }
    
    NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("customOutfitWindow");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xC8);
		AddCreatureOutfit(msg, player, defaultOutfit, true);

        if (!player->isUsingOtclient()) {
            msg->AddByte((size_t)std::min((size_t)OUTFITS_MAX_NUMBER, outfits.size()));
        }
        else {
            msg->AddByte(outfits.size());
        }
        
        std::list<Outfit>::iterator it = outfits.begin();
        for (int32_t i = 0; it != outfits.end() && i < OUTFITS_MAX_NUMBER; ++it, ++i) {
            msg->AddU16(it->lookType);
            msg->AddString(it->name);
            msg->AddByte(it->addons);
        }

		player->hasRequestedCustomOutfit(true);
	}
}

// void ProtocolGame::sendMarketEnter(uint32_t depotId)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketEnter");
// #endif
    // TRACK_MESSAGE(msg);

	// msg->AddByte(0xF6);

	// msg->AddU64(player->balance);
	// msg->AddByte(std::min<int32_t>(0xFF, IOMarket::getPlayerOfferCount(player->getGUID())));

	// Depot* depotChest = player->getDepot(depotId, false);
	// if (!depotChest) {
		// msg->AddU16(0x00);
		// writeToOutputBuffer(msg);
		// return;
	// }

	// player->setInMarket(true);

	// std::map<uint16_t, uint32_t> depotItems;
	// std::list<Container*> containerList;
	// containerList.push_back(depotChest);
	// containerList.push_back(player->getInbox());

	// do {
        // Container* container = containerList.front();
		// containerList.pop_front();
		
	    // for (int32_t i = 0; i < (int32_t)container->size(); ++i) {
			// Item* item = container->getItem(i);

			// Container* c = item->getContainer();
			// if (c && !c->empty()) {
				// containerList.push_back(c);
				// continue;
			// }
			
			// if (item->getUniqueOwner() > 0) {
                // continue;
            // }

			// const ItemType& itemType = Item::items[item->getID()];
			// if (itemType.wareId == 0) {
				// continue;
			// }

			// /*if (item->hasAttributes()) {
				// bool badAttribute = false;

				// ItemAttributes* attributes = item->getAttributes();
				// for (const auto& attr : attributes->getList()) {
					// if (attr.type == ITEM_ATTRIBUTE_CHARGES) {
						// uint16_t charges = static_cast<uint16_t>(0xFFFF & reinterpret_cast<ptrdiff_t>(attr.value));
						// if (charges != itemType.charges) {
							// badAttribute = true;
							// break;
						// }
					// } else if (attr.type == ITEM_ATTRIBUTE_DURATION) {
						// uint32_t duration = static_cast<uint32_t>(0xFFFFFFFF & reinterpret_cast<ptrdiff_t>(attr.value));
						// if (duration != itemType.decayTime) {
							// badAttribute = true;
							// break;
						// }
					// } else {
						// badAttribute = true;
						// break;
					// }
				// }

				// if (badAttribute) {
					// continue;
				// }
			// }*/

			// depotItems[itemType.wareId] += Item::countByType(item, -1, false);
		// }
	// } while (!containerList.empty());

	// msg->AddU16(std::min<size_t>(0xFFFF, depotItems.size()));

	// uint16_t i = 0;

	// for (std::map<uint16_t, uint32_t>::const_iterator it = depotItems.begin(), end = depotItems.end(); it != end && i < 0xFFFF; ++it, ++i) {
		// msg->AddU16(it->first);
		// msg->AddU16(std::min<uint32_t>(0xFFFF, it->second));
	// }

	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketLeave()
// {
    // NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketLeave");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF7);
	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketBrowseItem(uint16_t itemId, const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketBrowseItem");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF9);
	// msg->AddItemId(itemId);

	// msg->AddU32(buyOffers.size());
    // for (MarketOfferList::const_iterator it = buyOffers.begin(); it != buyOffers.end(); ++it) {
		// msg->AddU32((*it).timestamp);
		// msg->AddU16((*it).counter);
		// msg->AddU16((*it).amount);
		// msg->AddU32((*it).price);
		// msg->AddString((*it).playerName);
	// }

	// msg->AddU32(sellOffers.size());
	// for (MarketOfferList::const_iterator it = sellOffers.begin(); it != sellOffers.end(); ++it) {
		// msg->AddU32((*it).timestamp);
		// msg->AddU16((*it).counter);
		// msg->AddU16((*it).amount);
		// msg->AddU32((*it).price);
		// msg->AddString((*it).playerName);
	// }

	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketAcceptOffer(const MarketOfferEx& offer)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketAcceptOffer");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF9);
	// msg->AddItemId(offer.itemId);

	// if (offer.type == MARKETACTION_BUY) {
		// msg->AddU32(0x01);
		// msg->AddU32(offer.timestamp);
		// msg->AddU16(offer.counter);
		// msg->AddU16(offer.amount);
		// msg->AddU32(offer.price);
		// msg->AddString(offer.playerName);
		// msg->AddU32(0x00);
	// } else {
		// msg->AddU32(0x00);
		// msg->AddU32(0x01);
		// msg->AddU32(offer.timestamp);
		// msg->AddU16(offer.counter);
		// msg->AddU16(offer.amount);
		// msg->AddU32(offer.price);
		// msg->AddString(offer.playerName);
	// }

	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketBrowseOwnOffers(const MarketOfferList& buyOffers, const MarketOfferList& sellOffers)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketBrowseOwnOffers");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF9);
	// msg->AddU16(MARKETREQUEST_OWN_OFFERS);

	// msg->AddU32(buyOffers.size());
    // for (MarketOfferList::const_iterator it = buyOffers.begin(); it != buyOffers.end(); ++it) {
		// msg->AddU32((*it).timestamp);
		// msg->AddU16((*it).counter);
		// msg->AddItemId((*it).itemId);
		// msg->AddU16((*it).amount);
		// msg->AddU32((*it).price);
	// }

	// msg->AddU32(sellOffers.size());
	// for (MarketOfferList::const_iterator it = sellOffers.begin(); it != sellOffers.end(); ++it) {
		// msg->AddU32((*it).timestamp);
		// msg->AddU16((*it).counter);
		// msg->AddItemId((*it).itemId);
		// msg->AddU16((*it).amount);
		// msg->AddU32((*it).price);
	// }

	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketCancelOffer(const MarketOfferEx& offer)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketCancelOffer");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF9);
	// msg->AddU16(MARKETREQUEST_OWN_OFFERS);

	// if (offer.type == MARKETACTION_BUY) {
		// msg->AddU32(0x01); // 1 buy offer
		// msg->AddU32(offer.timestamp);
		// msg->AddU16(offer.counter);
		// msg->AddItemId(offer.itemId);
		// msg->AddU16(offer.amount);
		// msg->AddU32(offer.price);
		// msg->AddU32(0x00); // 0 sell offer
	// } else {
		// msg->AddU32(0x00); // 0 buy offer
		// msg->AddU32(0x01); // 1 sell offer
		// msg->AddU32(offer.timestamp);
		// msg->AddU16(offer.counter);
		// msg->AddItemId(offer.itemId);
		// msg->AddU16(offer.amount);
		// msg->AddU32(offer.price);
	// }

	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketBrowseOwnHistory(const HistoryMarketOfferList& buyOffers, const HistoryMarketOfferList& sellOffers)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketBrowseOwnHistory");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF9);
	// msg->AddU16(MARKETREQUEST_OWN_HISTORY);

	// std::map<uint32_t, uint16_t> counterMap;

	// uint32_t i = 0;
	// msg->AddU32(std::min<uint32_t>(800, buyOffers.size()));

	// for (HistoryMarketOfferList::const_iterator it = buyOffers.begin(), end = buyOffers.end(); it != end && i < 800; ++it, ++i) {
		// msg->AddU32(it->timestamp);
		// msg->AddU16(counterMap[it->timestamp]++);
		// msg->AddItemId(it->itemId);
		// msg->AddU16(it->amount);
		// msg->AddU32(it->price);
		// msg->AddByte(it->state);
	// }

	// counterMap.clear();
	// i = 0;

	// msg->AddU32(std::min<uint32_t>(800, sellOffers.size()));

	// for (HistoryMarketOfferList::const_iterator it = sellOffers.begin(), end = sellOffers.end(); it != end && i < 800; ++it, ++i) {
		// msg->AddU32(it->timestamp);
		// msg->AddU16(counterMap[it->timestamp]++);
		// msg->AddItemId(it->itemId);
		// msg->AddU16(it->amount);
		// msg->AddU32(it->price);
		// msg->AddByte(it->state);
	// }

	// writeToOutputBuffer(msg);
// }

// void ProtocolGame::sendMarketDetail(uint16_t itemId)
// {
	// NetworkMessage_ptr msg = getOutputBuffer();
	// if (!msg) {
        // return;
    // }
    
// #ifdef __LOG_PACKETS__
    // recordPacket("sendMarketDetail");
// #endif
    // TRACK_MESSAGE(msg);
    
	// msg->AddByte(0xF8);
	// msg->AddItemId(itemId);

	// const ItemType& it = Item::items[itemId];
	// /*if (it.armor != 0) {
        // std::stringstream ss;
        // ss << it.armor;
		// msg->AddString(ss.str());
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// /*if (it.attack != 0) {
		// TODO: chance to hit, range
		// example:
		// "attack +x, chance to hit +y%, z fields"
		// if (it.abilities && it.abilities->elementType != COMBAT_NONE && it.decayTo > 0) {
			// std::ostringstream ss;
			// ss << it.attack << " physical +" << it.abilities->elementDamage << ' ' << getCombatName(it.abilities->elementType);
			// msg->AddString(ss.str());
		// } else {
			// msg->AddString(std::to_string(it.attack));
		// }
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// if (it.isContainer()) {
        // std::stringstream ss;
        // ss << it.maxItems;
		// msg->AddString(ss.str());
	// } else {
		// msg->AddU16(0x00);
	// }

	// /*if (it.defense != 0) {
		// if (it.extraDefense != 0) {
			// std::ostringstream ss;
			// ss << it.defense << ' ' << std::showpos << it.extraDefense << std::noshowpos;
			// msg->AddString(ss.str());
		// } else {
			// msg->AddString(std::to_string(it.defense));
		// }
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// if (!it.description.empty()) {
		// /*const std::string& descr = it.description;
		// if (descr.back() == '.') {
			// msg->AddString(std::string(descr, 0, descr.length() - 1));
		// } else {
			// msg->AddString(descr);
		// }*/
		// msg->AddString(it.description);
	// } else {
		// msg->AddU16(0x00);
	// }

	// if (it.decayTime != 0) {
		// std::ostringstream ss;
		// ss << it.decayTime << " seconds";
		// msg->AddString(ss.str());
	// } else {
		// msg->AddU16(0x00);
	// }

	// /*if (it.abilities) {
		// std::ostringstream ss;
		// bool separator = false;

		// for (uint32_t i = (COMBAT_FIRST + 1); i <= COMBAT_COUNT; ++i) {
			// if (!it.abilities->absorbPercent[i]) {
				// continue;
			// }

			// if (separator) {
				// ss << ", ";
			// } else {
				// separator = true;
			// }

			// ss << getCombatName(indexToCombatType(i)) << ' ' << std::showpos << it.abilities->absorbPercent[i] << std::noshowpos << '%';
		// }

		// msg->AddString(ss.str());
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// /*if (it.minReqLevel != 0) {
		// msg->AddString(std::to_string(it.minReqLevel));
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// /*if (it.minReqMagicLevel != 0) {
		// msg->AddString(std::to_string(it.minReqMagicLevel));
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// msg->AddString(it.vocationString);

	// msg->AddString(it.runeSpellName);

	// /*if (it.abilities) {
		// std::ostringstream ss;
		// bool separator = false;

		// for (uint8_t i = SKILL_FIRST; i <= SKILL_LAST; i++) {
			// if (!it.abilities->skills[i]) {
				// continue;
			// }

			// if (separator) {
				// ss << ", ";
			// } else {
				// separator = true;
			// }

			// ss << getSkillName(i) << ' ' << std::showpos << it.abilities->skills[i] << std::noshowpos;
		// }

		// if (it.abilities->stats[STAT_MAGICPOINTS] != 0) {
			// if (separator) {
				// ss << ", ";
			// } else {
				// separator = true;
			// }

			// ss << "magic level " << std::showpos << it.abilities->stats[STAT_MAGICPOINTS] << std::noshowpos;
		// }

		// if (it.abilities->speed != 0) {
			// if (separator) {
				// ss << ", ";
			// }

			// ss << "speed " << std::showpos << (it.abilities->speed >> 1) << std::noshowpos;
		// }

		// msg->AddString(ss.str());
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// /*if (it.charges != 0) {
		// msg->AddString(std::to_string(it.charges));
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// /*std::string weaponName = getWeaponName(it.weaponType);

	// if (it.slotPosition & SLOTP_TWO_HAND) {
		// if (!weaponName.empty()) {
			// weaponName += ", two-handed";
		// } else {
			// weaponName = "two-handed";
		// }
	// }

	// msg->AddString(weaponName);*/
	// msg->AddU16(0x00);

	// /*if (it.weight != 0) {
		// std::ostringstream ss;
		// if (it.weight < 10) {
			// ss << "0.0" << it.weight;
		// } else if (it.weight < 100) {
			// ss << "0." << it.weight;
		// } else {
			// std::string weightString = std::to_string(it.weight);
			// weightString.insert(weightString.end() - 2, '.');
			// ss << weightString;
		// }
		// ss << " oz";
		// msg->AddString(ss.str());
	// } else {
		// msg->AddU16(0x00);
	// }*/
	// msg->AddU16(0x00);

	// MarketStatistics* statistics = IOMarket::getInstance()->getPurchaseStatistics(itemId);
	// if (statistics) {
		// msg->AddByte(0x01);
		// msg->AddU32(statistics->numTransactions);
		// msg->AddU32(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), statistics->totalPrice));
		// msg->AddU32(statistics->highestPrice);
		// msg->AddU32(statistics->lowestPrice);
	// } else {
		// msg->AddByte(0x00);
	// }

	// statistics = IOMarket::getInstance()->getSaleStatistics(itemId);
	// if (statistics) {
		// msg->AddByte(0x01);
		// msg->AddU32(statistics->numTransactions);
		// msg->AddU32(std::min<uint64_t>(std::numeric_limits<uint32_t>::max(), statistics->totalPrice));
		// msg->AddU32(statistics->highestPrice);
		// msg->AddU32(statistics->lowestPrice);
	// } else {
		// msg->AddByte(0x00);
	// }

	// writeToOutputBuffer(msg);
// }

void ProtocolGame::sendQuests()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("quests");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xF0);

		msg->AddU16(Quests::getInstance()->getQuestCount(player));
		for(QuestList::const_iterator it = Quests::getInstance()->getFirstQuest(); it != Quests::getInstance()->getLastQuest(); ++it)
		{
			if(!(*it)->isStarted(player))
				continue;

			msg->AddU16((*it)->getId());
			msg->AddString((*it)->getName(player));
			msg->AddByte((*it)->isCompleted(player));
		}
	}
}

void ProtocolGame::sendQuestInfo(Quest* quest)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("questInfo");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xF1);
		msg->AddU16(quest->getId());

		msg->AddByte(quest->getMissionCount(player));
		for(MissionList::const_iterator it = quest->getFirstMission(); it != quest->getLastMission(); ++it)
		{
			if(!(*it)->isStarted(player))
				continue;

			msg->AddString((*it)->getName(player));
			msg->AddString((*it)->getDescription(player));
		}
	}
}

void ProtocolGame::sendVIPLogIn(uint32_t guid)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("VIPLogIn");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xD3);
		msg->AddU32(guid);
	}
}

void ProtocolGame::sendVIPLogOut(uint32_t guid)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("VIPLogOut");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xD4);
		msg->AddU32(guid);
	}
}

void ProtocolGame::sendVIP(uint32_t guid, const std::string& name, bool isOnline)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
#ifdef __LOG_PACKETS__
        recordPacket("VIP");
#endif
        TRACK_MESSAGE(msg);
		msg->AddByte(0xD2);
		msg->AddU32(guid);
		msg->AddString(name);
		msg->AddByte(isOnline ? 1 : 0);
	}
}

////////////// Add common messages
void ProtocolGame::AddMapDescription(NetworkMessage_ptr msg, const Position& pos)
{
    msg->AddByte(0x64);
	if(tvOwner)
	    msg->AddPosition(tvOwner->getPosition());
    else
	    msg->AddPosition(player->getPosition());
	GetMapDescription(pos.x - Map::maxClientViewportX, pos.y - Map::maxClientViewportY, pos.z, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, msg);
}

void ProtocolGame::AddTextMessage(NetworkMessage_ptr msg, MessageClasses mclass, const std::string& message)
{
	msg->AddByte(0xB4);
	msg->AddByte(mclass);
	msg->AddString(message);
}

void ProtocolGame::AddAnimatedText(NetworkMessage_ptr msg, const Position& pos,
	uint8_t color, const std::string& text)
{
	msg->AddByte(0x84);
	msg->AddPosition(pos);
	msg->AddByte(color);
	msg->AddString(text);
}

void ProtocolGame::AddMagicEffect(NetworkMessage_ptr msg,const Position& pos, uint16_t type)
{
	msg->AddByte(0x83);
	msg->AddPosition(pos);
	msg->AddU16(type + 1); /* PS working with more then 255 effects msg->AddByte(type + 1);*/
}

void ProtocolGame::AddDistanceShoot(NetworkMessage_ptr msg, const Position& from, const Position& to,
	uint8_t type)
{
	msg->AddByte(0x85);
	msg->AddPosition(from);
	msg->AddPosition(to);
	msg->AddByte(type + 1);
}

void ProtocolGame::AddCreature(NetworkMessage_ptr msg, const Creature* creature, bool known, uint32_t remove)
{   
    uint32_t id = getCreatureID(creature);
    if(!known)
	{   
        msg->AddU16(0x61);
		msg->AddU32(remove);
		msg->AddU32(id);
		// if (const Monster* monster = creature->getMonster())// PS
		// {
            // std::stringstream s;
            // s << monster->getNickname() << " [" << monster->getLevel() << "]";
            // msg->AddString(monster->getHideName() ? "" : s.str().c_str());
        // }
        // else
        msg->AddString(creature->getHideName() ? "" : creature->getNickname().c_str());
	}
	else
	{
        msg->AddU16(0x62);
		msg->AddU32(id);
	}

	if(!creature->getHideHealth())
		msg->AddByte((int32_t)std::ceil(((float)creature->getHealth()) * 100 / std::max(creature->getMaxHealth(), (int32_t)1)));
	else
		msg->AddByte(0x00);

	msg->AddByte((uint8_t)creature->getDirection());
	AddCreatureOutfit(msg, creature, creature->getCurrentOutfit());

	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);
	msg->AddByte(player->hasCustomFlag(PlayerCustomFlag_HasFullLight) ? 0xFF : lightInfo.level);
	msg->AddByte(lightInfo.color);

	msg->AddU16(creature->getStepSpeed());
	msg->AddByte(player->getSkullClient(creature));
	msg->AddByte(player->getPartyShield(creature));
	if(!known)
		msg->AddByte(0x00); // war emblem

	if (player->isUsingOtclient()) {
        msg->AddByte(creature->getCreatureIcon());
    }
    
    msg->AddByte(!player->canWalkthrough(creature));
	
	if (player->isUsingOtclient()) {
        msg->AddByte(creature->getMaster() == player);
        msg->AddByte(Combat::canDoCombat(player, creature) == RET_NOERROR);
    }
	
	msg->AddByte(creature->getFirstType());
	msg->AddByte(creature->getSecondType());

	if (const Creature* master = creature->getMaster()) {
		if (const Player* trainer = master->getPlayer()) {
			uint32_t level = 0;
			uint32_t experience = 0;
			if (Item* item = trainer->getInventoryItem((slots_t)8)) {
				if (item->getIntegerAttribute("10005")) {
					const int32_t* levelPoke = item->getIntegerAttribute("10005");
					level = *levelPoke;
				}
				if (item->getIntegerAttribute("10006")) {
					const int32_t* experiencePoke = item->getIntegerAttribute("10006");
					experience = *experiencePoke;
				}
			}
			msg->AddU16(level);
			msg->AddU32(experience);
		} else {
			msg->AddU16(0);
			msg->AddU32(0);
		}
	} else {
		if (const Monster* monster = creature->getMonster()) {
			msg->AddU16(monster->getLevel());
			msg->AddU32(0);
		} else{
			msg->AddU16(0);
			msg->AddU32(0);
		}
	}
}

void ProtocolGame::AddPlayerStats(NetworkMessage_ptr msg)
{
	msg->AddByte(0xA0);
	msg->AddU16(player->getHealth());
	msg->AddU16(player->getPlayerInfo(PLAYERINFO_MAXHEALTH));
	msg->AddU32(uint32_t(player->getFreeCapacity() * 100));
	uint64_t experience = player->getExperience();
	if(experience > 0x7FFFFFFF) // client debugs after 2,147,483,647 exp
		msg->AddU32(0x7FFFFFFF);
	else
		msg->AddU32(experience);

	msg->AddU16(player->getPlayerInfo(PLAYERINFO_LEVEL));
	msg->AddByte(player->getPlayerInfo(PLAYERINFO_LEVELPERCENT));
	msg->AddU16(player->getPlayerInfo(PLAYERINFO_MANA));
	msg->AddU16(player->getPlayerInfo(PLAYERINFO_MAXMANA));
	msg->AddByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVEL));
	msg->AddByte(player->getPlayerInfo(PLAYERINFO_MAGICLEVELPERCENT));
	msg->AddByte(player->getPlayerInfo(PLAYERINFO_SOUL));
	msg->AddU16(player->getStaminaMinutes());
}

void ProtocolGame::AddPlayerSkills(NetworkMessage_ptr msg)
{
	msg->AddByte(0xA1);
	msg->AddByte(player->getSkill(SKILL_FIST, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_FIST, SKILL_PERCENT));
	msg->AddByte(player->getSkill(SKILL_CLUB, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_CLUB, SKILL_PERCENT));
	msg->AddByte(player->getSkill(SKILL_SWORD, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_SWORD, SKILL_PERCENT));
	msg->AddByte(player->getSkill(SKILL_AXE, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_AXE, SKILL_PERCENT));
	msg->AddByte(player->getSkill(SKILL_DIST, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_DIST, SKILL_PERCENT));
	msg->AddByte(player->getSkill(SKILL_SHIELD, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_SHIELD, SKILL_PERCENT));
	msg->AddByte(player->getSkill(SKILL_FISH, SKILL_LEVEL));
	msg->AddByte(player->getSkill(SKILL_FISH, SKILL_PERCENT));
}

void ProtocolGame::AddCreatureSpeak(NetworkMessage_ptr msg, const Creature* creature, SpeakClasses type,
	std::string text, uint16_t channelId, uint32_t time/*= 0*/, Position* pos/* = NULL*/)
{
	msg->AddByte(0xAA);
	if(creature)
	{
		const Player* speaker = creature->getPlayer();
		if(speaker)
		{
			msg->AddU32(++g_chat.statement);
			g_chat.statementMap[g_chat.statement] = text;
		}
		else
			msg->AddU32(0x00);

		if(creature->getSpeakType() != SPEAK_CLASS_NONE)
			type = creature->getSpeakType();

		switch(type)
		{
			case SPEAK_CHANNEL_RA:
				msg->AddString("");
				break;
			case SPEAK_RVR_ANSWER:
				msg->AddString("Gamemaster");
				break;
			default:
				msg->AddString(!creature->getHideName() ? creature->getName() : "");
				break;
		}

		if(speaker && type != SPEAK_RVR_ANSWER && !speaker->isAccountManager()
			&& !speaker->hasCustomFlag(PlayerCustomFlag_HideLevel))
			msg->AddU16(speaker->getPlayerInfo(PLAYERINFO_LEVEL));
		else
			msg->AddU16(0x00);

	}
	else
	{
		msg->AddU32(0x00);
		msg->AddString("");
		msg->AddU16(0x00);
	}

	msg->AddByte(type);
	switch(type)
	{
		case SPEAK_SAY:
		case SPEAK_WHISPER:
		case SPEAK_YELL:
		case SPEAK_MONSTER_SAY:
		case SPEAK_MONSTER_YELL:
		case SPEAK_PRIVATE_NP:
		{
			if(pos)
				msg->AddPosition(*pos);
			else if(creature)
				msg->AddPosition(creature->getPosition());
			else
				msg->AddPosition(Position(0,0,7));

			break;
		}

		case SPEAK_CHANNEL_Y:
		case SPEAK_CHANNEL_RN:
		case SPEAK_CHANNEL_RA:
		case SPEAK_CHANNEL_O:
		case SPEAK_CHANNEL_W:
			msg->AddU16(channelId);
			break;

		case SPEAK_RVR_CHANNEL:
		{
			msg->AddU32(uint32_t(OTSYS_TIME() / 1000 & 0xFFFFFFFF) - time);
			break;
		}

		default:
			break;
	}

	msg->AddString(text);
}

void ProtocolGame::AddCreatureHealth(NetworkMessage_ptr msg,const Creature* creature)
{
	msg->AddByte(0x8C);
	msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
	if(!creature->getHideHealth())
		msg->AddByte((int32_t)std::ceil(((float)creature->getHealth()) * 100 / std::max(creature->getMaxHealth(), (int32_t)1)));
	else
		msg->AddByte(0x00);
}

void ProtocolGame::AddCreatureOutfit(NetworkMessage_ptr msg, const Creature* creature, const Outfit_t& outfit, bool outfitWindow/* = false*/)
{
    if(outfitWindow || !creature->getPlayer() || (!creature->isInvisible() && (!creature->isGhost()
		|| !g_config.getBool(ConfigManager::GHOST_INVISIBLE_EFFECT))))
	{
		msg->AddU16(outfit.lookType);
		if(outfit.lookType)
		{
			msg->AddByte(outfit.lookHead);
			msg->AddByte(outfit.lookBody);
			msg->AddByte(outfit.lookLegs);
			msg->AddByte(outfit.lookFeet);
			msg->AddByte(outfit.lookAddons);
		}
		else if(outfit.lookTypeEx)
			msg->AddItemId(outfit.lookTypeEx);
		else
			msg->AddU16(outfit.lookTypeEx);
	}
	else
		msg->AddU32(0x00);
}

void ProtocolGame::AddWorldLight(NetworkMessage_ptr msg, const LightInfo& lightInfo)
{
	msg->AddByte(0x82);
	msg->AddByte((player->hasCustomFlag(PlayerCustomFlag_HasFullLight) ? 0xFF : lightInfo.level));
	msg->AddByte(lightInfo.color);
}

void ProtocolGame::AddCreatureLight(NetworkMessage_ptr msg, const Creature* creature)
{
	LightInfo lightInfo;
	creature->getCreatureLight(lightInfo);
	msg->AddByte(0x8D);
	msg->AddU32(getCreatureID(creature)/*creature->getID()*/);
	msg->AddByte((player->hasCustomFlag(PlayerCustomFlag_HasFullLight) ? 0xFF : lightInfo.level));
	msg->AddByte(lightInfo.color);
}

//tile
void ProtocolGame::AddTileItem(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6A);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);
	msg->AddItem(item);
}

void ProtocolGame::AddTileCreature(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos, const Creature* creature)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6A);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);

	bool known;
	uint32_t removedKnown;
	checkCreatureAsKnown(/*creature->getID()*/getCreatureID(creature), known, removedKnown);
	AddCreature(msg, creature, known, removedKnown);
}

void ProtocolGame::UpdateTileItem(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos, const Item* item)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6B);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);
	msg->AddItem(item);
}

void ProtocolGame::RemoveTileItem(NetworkMessage_ptr msg, const Position& pos, uint32_t stackpos)
{
	if(stackpos >= 10)
		return;

	msg->AddByte(0x6C);
	msg->AddPosition(pos);
	msg->AddByte(stackpos);
}

void ProtocolGame::MoveUpCreature(NetworkMessage_ptr msg, const Creature* creature,
	const Position& newPos, const Position& oldPos, uint32_t oldStackpos)
{
    if(creature == player || (tvOwner && creature == tvOwner))
	{
		msg->AddByte(0xBE); //floor change up
		if(newPos.z == 7) //going to surface
		{
			int32_t skip = -1;
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, 5, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, 3, skip); //(floor 7 and 6 already set)
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, 4, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, 4, skip);
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, 3, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, 5, skip);
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, 2, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, 6, skip);
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, 1, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, 7, skip);
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, 0, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, 8, skip);
			if(skip >= 0)
			{
				msg->AddByte(skip);
				msg->AddByte(0xFF);
			}
		}
		else if(newPos.z > 7) //underground, going one floor up (still underground)
		{
			int32_t skip = -1;
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, oldPos.z - 3, (Map::maxClientViewportX+1), (Map::maxClientViewportY+1)*2, 3, skip);
			if(skip >= 0)
			{
				msg->AddByte(skip);
				msg->AddByte(0xFF);
			}
		}

    	//moving up a floor up makes us out of sync
    	//west
    	msg->AddByte(0x68);
    	GetMapDescription(oldPos.x - Map::maxClientViewportX, oldPos.y - (Map::maxClientViewportY-1), newPos.z, 1, (Map::maxClientViewportY+1)*2, msg);
    
    	//north
    	msg->AddByte(0x65);
    	GetMapDescription(oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z, (Map::maxClientViewportX+1)*2, 1, msg);
    }
}

void ProtocolGame::MoveDownCreature(NetworkMessage_ptr msg, const Creature* creature,
	const Position& newPos, const Position& oldPos, uint32_t oldStackpos)
{
    if(creature == player || (tvOwner && creature == tvOwner))
	{

		msg->AddByte(0xBF); //floor change down
		if(newPos.z == 8) //going from surface to underground
		{
			int32_t skip = -1;
     		GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, -1, skip);
     		GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z + 1, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, -2, skip);
     		GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z + 2, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, -3, skip);
			if(skip >= 0)
			{
				msg->AddByte(skip);
				msg->AddByte(0xFF);
			}
		}
		else if(newPos.z > oldPos.z && newPos.z > 8 && newPos.z < 14) //going further down
		{
			int32_t skip = -1;
		    GetFloorDescription(msg, oldPos.x - Map::maxClientViewportX, oldPos.y - Map::maxClientViewportY, newPos.z + 2, (Map::maxClientViewportX+1)*2, (Map::maxClientViewportY+1)*2, -3, skip);
			if(skip >= 0)
			{
				msg->AddByte(skip);
				msg->AddByte(0xFF);
			}
		}

		//moving down a floor makes us out of sync
		//east
		msg->AddByte(0x66);
		GetMapDescription(oldPos.x + Map::maxClientViewportX+1, oldPos.y - (Map::maxClientViewportY+1), newPos.z, 1, ((Map::maxClientViewportY+1)*2), msg);

		//south
		msg->AddByte(0x67);
		GetMapDescription(oldPos.x - Map::maxClientViewportX, oldPos.y + (Map::maxClientViewportY+1), newPos.z, ((Map::maxClientViewportX+1)*2), 1, msg);
	}
}

//inventory
void ProtocolGame::AddInventoryItem(NetworkMessage_ptr msg, slots_t slot, const Item* item)
{
	if(item)
	{
		msg->AddByte(0x78);
		msg->AddByte(slot);
		msg->AddItem(item);
		std::string pname = "none";
		if(item->getStringAttribute("10002"))
		{
			const std::string* namePoke = item->getStringAttribute("10002");
			pname = *namePoke;
		}
		msg->AddString(pname);
		if(pname != "none") {
			uint32_t level = 0;
			if(item->getIntegerAttribute("10005"))
			{
				const int32_t* levelPoke = item->getIntegerAttribute("10005");
				level = *levelPoke;
			}
			msg->AddU32(level);
			uint32_t gender = 0;
			if(item->getIntegerAttribute("10016"))
			{
				const int32_t* genderPoke = item->getIntegerAttribute("10016");
				gender = *genderPoke;
			}
			msg->AddU32(gender);
		}
	}
	else
		RemoveInventoryItem(msg, slot);
}

void ProtocolGame::RemoveInventoryItem(NetworkMessage_ptr msg, slots_t slot)
{
	msg->AddByte(0x79);
	msg->AddByte(slot);
}

void ProtocolGame::UpdateInventoryItem(NetworkMessage_ptr msg, slots_t slot, const Item* item)
{
	AddInventoryItem(msg, slot, item);
}

//containers
void ProtocolGame::AddContainerItem(NetworkMessage_ptr msg, uint8_t cid, const Item* item)
{
	msg->AddByte(0x70);
	msg->AddByte(cid);
	msg->AddItem(item);
	std::string pname = "none";
	if(item->getStringAttribute("10002"))
	{
		const std::string* namePoke = item->getStringAttribute("10002");
		pname = *namePoke;
	}
	msg->AddString(pname);
	if(pname != "none") {
		uint32_t level = 0;
	    if(item->getIntegerAttribute("10005"))
	    {
	    	const int32_t* levelPoke = item->getIntegerAttribute("10005");
	    	level = *levelPoke;
	    }
	    msg->AddU32(level);
	    uint32_t gender = 0;
	    if(item->getIntegerAttribute("10016"))
	    {
	    	const int32_t* genderPoke = item->getIntegerAttribute("10016");
	    	gender = *genderPoke;
	    }
	    msg->AddU32(gender);
	}
}

void ProtocolGame::UpdateContainerItem(NetworkMessage_ptr msg, uint8_t cid, uint8_t slot, const Item* item)
{
	msg->AddByte(0x71);
	msg->AddByte(cid);
	msg->AddByte(slot);
	msg->AddItem(item);
	std::string pname = "none";
	if(item->getStringAttribute("10002"))
	{
		const std::string* namePoke = item->getStringAttribute("10002");
		pname = *namePoke;
	}
	msg->AddString(pname);
	if(pname != "none") {
		uint32_t level = 0;
	    if(item->getIntegerAttribute("10005"))
	    {
	    	const int32_t* levelPoke = item->getIntegerAttribute("10005");
	    	level = *levelPoke;
	    }
	    msg->AddU32(level);
	    uint32_t gender = 0;
	    if(item->getIntegerAttribute("10016"))
	    {
	    	const int32_t* genderPoke = item->getIntegerAttribute("10016");
	    	gender = *genderPoke;
	    }
	    msg->AddU32(gender);
	}
}

void ProtocolGame::RemoveContainerItem(NetworkMessage_ptr msg, uint8_t cid, uint8_t slot)
{
	msg->AddByte(0x72);
	msg->AddByte(cid);
	msg->AddByte(slot);
}

void ProtocolGame::sendChannelMessage(std::string author, std::string text, SpeakClasses type, uint16_t channel)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
		TRACK_MESSAGE(msg);
		msg->AddByte(0xAA);
		msg->AddU32(0x00);
		msg->AddString(author);
		msg->AddU16(0x00);
		msg->AddByte(type);
		msg->AddU16(channel);
		msg->AddString(text);
	}
}

void ProtocolGame::AddShopItem(NetworkMessage_ptr msg, const ShopInfo item)
{   
    const ItemType& it = Item::items[item.itemId];
	msg->AddU16(it.clientId);
	if(it.isSplash() || it.isFluidContainer())
		msg->AddByte(fluidMap[item.subType % 8]);
	else if(it.stackable || it.charges)
		msg->AddByte(item.subType);
	else
		msg->AddByte(0x01);

	msg->AddString(item.itemName);
	msg->AddU32(uint32_t(it.weight * 100));
	msg->AddU32(item.buyPrice);
	msg->AddU32(item.sellPrice);
}

void ProtocolGame::sendTVStart()
{    
    NetworkMessage_ptr msg = getOutputBuffer();
    if(!msg || !tvOwner)
        return;

    TRACK_MESSAGE(msg);
    
    removeKnownCreature(player->getID());
    /*if(canSeeIgnoringTV(tvOwner))
        removeKnownCreature(tvOwner->getID());*/
    
    AddMapDescription(msg, tvOwner->getPosition());
}

void ProtocolGame::sendTVEnd(Player* oldTVOwner)
{
    NetworkMessage_ptr msg = getOutputBuffer();
	if(!msg)
		return;

	TRACK_MESSAGE(msg);

    /*if(oldTVOwner)
    {
        if(canSeeIgnoringTV(oldTVOwner))
            removeKnownCreature(oldTVOwner->getID());
        else
        {
            std::list<uint32_t>::iterator i = std::find(knownCreatureList.begin(), knownCreatureList.end(), oldTVOwner->getID());
            if(i != knownCreatureList.end())
                knownCreatureList.erase(i);
        }
    }
    removeKnownCreature(player->getID());*/
    
    removeKnownCreature(player->getID());
	AddMapDescription(msg, player->getPosition());
}

void ProtocolGame::sendCustomQuestInfo(uint16_t questId, std::map<std::string, std::string> &missions)
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
		TRACK_MESSAGE(msg);
		msg->AddByte(0xF1);
		msg->AddU16(questId);

		msg->AddByte(missions.size());
		for(std::map<std::string, std::string>::const_iterator it = missions.begin(); it != missions.end(); ++it) {
			msg->AddString(it->first);
			msg->AddString(it->second);
		}
	}
}
#ifdef __LOG_PACKETS__
void ProtocolGame::recordPacket(const std::string& packet)
{
    if (m_lastPackets.size() >= 20) {
        m_lastPackets.erase(m_lastPackets.begin());
    }
    m_lastPackets.push_back(packet);
}
#endif

void ProtocolGame::sendDollCaseStatus(std::vector<uint8_t>status) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("dollCaseStatus");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x14);
        
        output->AddU16(status.size());
        std::vector<uint8_t>::const_iterator it;
        for(it = status.begin(); it != status.end(); it++)
            output->AddByte(*it);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendDollCaseUpdate(uint16_t pokemonNumber, uint8_t status) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("dollCaseUpdate");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x15);
        
        output->AddU16(pokemonNumber);
        output->AddByte(status);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendSlotMachine(uint8_t result1, uint8_t result2, uint8_t result3) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("slotMachine");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x16);
        
        output->AddByte(result1);
        output->AddByte(result2);
        output->AddByte(result3);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendTip(uint8_t id) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("tip");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x17);
        
        output->AddByte(id);
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPollWindow(Poll* poll) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("pollWindow");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x18);
        
        output->AddString(poll->getQuestion());
        
        if (poll->getTextMode()) {
            output->AddByte(0x01);
        }
        
        else {
            output->AddByte(0x00);
            output->AddByte(poll->getPollOptions()->size());
            for (PollOptionsMap::iterator it = poll->getPollOptions()->begin(); it != poll->getPollOptions()->end(); ++it) {
                output->AddByte(it->first);
                output->AddString(it->second);
            }
        }
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPokemonLevelUp(uint16_t pokemonNumber, uint8_t newLevel, std::vector<uint16_t> newMoves) // PS
{
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("sendPokemonLevelUp");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x19);
        
        output->AddU16(pokemonNumber);
        output->AddByte(newLevel);
        
        output->AddU16(newMoves.size());
        std::vector<uint16_t>::const_iterator it;
        for(it = newMoves.begin(); it != newMoves.end(); it++) {
            output->AddU16(*it);
        }
        
        OutputMessagePool::getInstance()->send(output);
    }
}

void ProtocolGame::sendPingBack()
{
	NetworkMessage_ptr msg = getOutputBuffer();
	if(msg)
	{
		TRACK_MESSAGE(msg);
		msg->AddByte(0x1E);
	}
}

void ProtocolGame::sendLootList(std::list<Item*>& lootList)
{
    // extended opcodes can only be send to players using otclient, cipsoft's tibia can't understand them
    if(player && !player->isUsingOtclient()) {
        return;
    }
    
    if(OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false)) //single message, with this doesn't need to parse in the client
    {
#ifdef __LOG_PACKETS__
        recordPacket("sendLootList");
#endif
        TRACK_MESSAGE(output);
        output->AddByte(0xFF);
        output->AddByte(0x1A);
        
        output->AddByte(lootList.size());
        while(lootList.size() > 0) {
    		Item* item = lootList.front();
    		lootList.pop_front();
    		output->AddU16(item->getClientID());
    		output->AddByte(item->getItemCount());
    	}
        
        OutputMessagePool::getInstance()->send(output);
    }
}
