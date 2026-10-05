// license:GPLv3+

#pragma once

#include "MsgPlugin.h"

#include <stdint.h>

// Scores of the players of the running game, shown by a plugin on behalf of the table script (UltraDMD scoreboard, ...).
// VPX listens to it to record the scores of the games (see ScoreTracker).

#define SCOREPI_NAMESPACE "Scores"

// Broadcasted on the main thread each time a plugin shows the scores of the players, message data is a pointer to a ScoreboardEvent
#define SCOREPI_EVT_ON_SCOREBOARD "OnScoreboard:1"

#define SCOREPI_MAX_PLAYERS 8

typedef struct ScoreboardEvent
{
   const char* source; // Short name of what shows the scores, for logs and statistics (e.g. "ultradmd")
   int32_t nPlayers; // Players of the game, 0 if unknown
   int32_t currentPlayer; // Player up, from 1, 0 if unknown
   int32_t nScores; // Valid entries of scores
   int64_t scores[SCOREPI_MAX_PLAYERS]; // Score of each player, the first one being player 1
} ScoreboardEvent;
