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

#include "iotournament.h"
#include "database.h"

#include "configmanager.h"
#include "game.h"

extern Game g_game;
extern ConfigManager g_config;

bool IOTournament::load(Tournament* tournament)
{
	Database* db = Database::getInstance();
	DBQuery query;

	query << "SELECT `id`, `last_winner`, `last_date`, `number`, `next_date` FROM `tournaments` WHERE `tournament_id` = " << tournament->getId() << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID) << " LIMIT 1";
	DBResult* result;
	if((result = db->storeQuery(query.str())))
    {
        tournament->setDate(result->getDataInt("next_date"));
        tournament->setNumber(result->getDataInt("number"));
        result->free();
    }
    else
    {
        query.str("");
        query << "INSERT INTO `tournaments` (`tournament_id`, `world_id`, `name`, `min_level`, `max_level`) VALUES ('" <<
        tournament->getId() << "', '" << g_config.getNumber(ConfigManager::WORLD_ID) << "', '" << tournament->getName() <<
        "', '" << tournament->getMinLevel() << "', '" << tournament->getMaxLevel() << "')";
	    return db->executeQuery(query.str());
    }

    //load inscrptions
    query.str("");
	query << "SELECT `player_id` FROM `tournament_inscriptions` WHERE `tournament_id` = " << tournament->getId() << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID) << " LIMIT " << uint32_t(tournament->getCapacity());
	if(!(result = db->storeQuery(query.str())))
		return true;

	do
		tournament->join(result->getDataInt("player_id"), 0, true);
	while(result->next());
	result->free();    
    return true;
}

bool IOTournament::ban(uint32_t playerGUID, uint64_t expires)
{
    Database* db = Database::getInstance();
	DBQuery query;
	query << "INSERT INTO `tournament_bans` (`player_id`, `expires`) VALUES ('" << playerGUID << "', '" << expires << "')";
	return db->executeQuery(query.str());
}

bool IOTournament::join(uint16_t tournamentId, uint32_t playerGUID, uint32_t accountId)
{
    Database* db = Database::getInstance();
	DBQuery query;
	DBResult* result;
	
	query << "SELECT `id` FROM `tournament_inscriptions` WHERE `player_id` = " << playerGUID << " LIMIT 1";
	if((result = db->storeQuery(query.str())))
	{
		result->free();
        return false;
    }
    
    query.str("");
    query << "SELECT `id` FROM `tournament_inscriptions` WHERE `tournament_id` = " << tournamentId << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID) << " AND `account_id` = " << accountId << " LIMIT 1";
	if((result = db->storeQuery(query.str())))
	{
		result->free();
        return false;
    }
    
    query.str("");
    query << "SELECT `id` FROM `tournament_bans` WHERE `player_id` = " << playerGUID << " LIMIT 1";
	if((result = db->storeQuery(query.str())))
	{
		result->free();
        return false;
    }
	
	query.str("");
	query << "INSERT INTO `tournament_inscriptions` (`tournament_id`, `player_id`, `account_id`, `world_id`) VALUES ('" << tournamentId << "', '" << playerGUID << "', '" << accountId << "', '" << g_config.getNumber(ConfigManager::WORLD_ID) << "')";
	return db->executeQuery(query.str());
}

bool IOTournament::leave(uint32_t playerGUID)
{
    Database* db = Database::getInstance();
	DBQuery query;
	query << "DELETE FROM `tournament_inscriptions` WHERE `player_id` = " << playerGUID;
	return db->executeQuery(query.str());
}

bool IOTournament::setNextDate(uint16_t tournamentId, uint64_t nextDate)
{
	Database* db = Database::getInstance();
	DBQuery query;
	query << "UPDATE `tournaments` SET `next_date` = " << nextDate << " WHERE `tournament_id` = " << tournamentId << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID) << db->getUpdateLimiter();
	return db->executeQuery(query.str());
}

bool IOTournament::restart(uint16_t tournamentId, uint32_t lastWinner, uint64_t lastDate, uint16_t number, uint64_t nextDate)
{    
    Database* db = Database::getInstance();
	DBQuery query;

	query << "DELETE FROM `tournament_inscriptions` WHERE `tournament_id` = " << tournamentId << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID);
	if(!db->executeQuery(query.str()))
		return false;

	query.str("");
	query << "UPDATE `tournaments` SET `last_winner` = " << lastWinner << ", `last_date` = " << lastDate << ", `number` = " << number << ", `next_date` = " << nextDate << " WHERE `tournament_id` = " << tournamentId << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID) << " LIMIT 1";
    return db->executeQuery(query.str());
}

bool IOTournament::isInscribed(uint32_t playerGUID)
{
    Database* db = Database::getInstance();
	DBResult* result;

	DBQuery query;
	query << "SELECT `id` FROM `tournament_inscriptions` WHERE `player_id` = " << playerGUID << " LIMIT 1";
	if(!(result = db->storeQuery(query.str())))
		return false;

	result->free();
	return true;
}

uint16_t IOTournament::getPlayerTournamentId(uint32_t playerGUID)
{
    Database* db = Database::getInstance();
	DBResult* result;

	DBQuery query;
	query << "SELECT `tournament_id` FROM `tournament_inscriptions` WHERE `player_id` = " << playerGUID << " LIMIT 1";
	if(!(result = db->storeQuery(query.str())))
		return 0;
		
    uint16_t id = result->getDataInt("tournament_id");
	result->free();
	return id;
}

bool IOTournament::clearBans()
{
    Database* db = Database::getInstance();
	DBQuery query;
    query << "DELETE FROM `tournament_bans` WHERE `expires` < " << time(NULL);
    return db->executeQuery(query.str());
}

uint32_t IOTournament::getTournamentId(uint16_t tournamentRankId) const
{
	Database* db = Database::getInstance();
	DBQuery query;
	query << "SELECT `id` FROM `tournaments` WHERE `tournament_id` = " << tournamentRankId << " AND `world_id` = " << g_config.getNumber(ConfigManager::WORLD_ID) << " LIMIT 1";

	DBResult* result;
	if (!(result = db->storeQuery(query.str()))) {
		return 0;
    }

	const uint32_t id = result->getDataInt("id");
	result->free();
	return id;
}

uint32_t IOTournament::getHighestWeeklyPlayer() const
{
    Database* db = Database::getInstance();
	DBQuery query;
	query << "SELECT `id` FROM `players` WHERE `tournament_weekly_score` > 0 ORDER BY `tournament_weekly_score` DESC LIMIT 1";

	DBResult* result;
	if (!(result = db->storeQuery(query.str()))) {
		return 0;
    }

	const uint32_t id = result->getDataInt("id");
	result->free();
	return id;
}

uint32_t IOTournament::getLastHighestWeeklyPlayer() const
{
    Database* db = Database::getInstance();
	DBQuery query;
	query << "SELECT `player_id` FROM `tournament_weekly_winners` ORDER BY `date` DESC LIMIT 1";

	DBResult* result;
	if (!(result = db->storeQuery(query.str()))) {
		return 0;
    }

	const uint32_t id = result->getDataInt("player_id");
	result->free();
	return id;
}

bool IOTournament::doRegisterWeeklyWinner(uint32_t playerGuid)
{
    Database* db = Database::getInstance();
	DBQuery query;
	query << "INSERT INTO `tournament_weekly_winners` (`player_id`, `date`) VALUES ('" << playerGuid << "', '" << time(NULL) << "')";
	return db->executeQuery(query.str());
}

bool IOTournament::doResetWeeklyScore()
{
    Database* db = Database::getInstance();
	DBQuery query;
	query << "UPDATE `players` SET `tournament_weekly_score` = 0 ";
	return db->executeQuery(query.str());
}
