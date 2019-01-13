#ifndef GAME_SERVER_SCORE_H
#define GAME_SERVER_SCORE_H

#include "entities/character.h"
#include "gamecontext.h"

#define NUM_CHECKPOINTS 25

class CPlayerData
{
public:
	CPlayerData()
	{
		Reset();
	}

	void Reset()
	{
		m_BestTime = 0;
		m_CurrentTime = 0;
		for(int i = 0; i < NUM_CHECKPOINTS; i++)
			m_aBestCpTime[i] = 0;
	}

	void Set(float Time, float CpTime[NUM_CHECKPOINTS])
	{
		m_BestTime = Time;
		for(int i = 0; i < NUM_CHECKPOINTS; i++)
			m_aBestCpTime[i] = CpTime[i];
	}

	float m_BestTime;
	float m_CurrentTime;
	float m_aBestCpTime[NUM_CHECKPOINTS];
};

class IScore
{
	CPlayerData m_aPlayerData[MAX_CLIENTS];

public:
	virtual ~IScore() {}

	CPlayerData *PlayerData(int ID) { return &m_aPlayerData[ID]; }

	virtual void MapInfo(int ClientID, const char *pMapName) = 0;
	virtual void MapVote(int ClientID, const char *pMapName) = 0;
	virtual void CheckBirthday(int ClientID) = 0;
	virtual void LoadScore(int ClientID) = 0;
	virtual void SaveScore(int ClientID, float Time, float aCpTime[NUM_CHECKPOINTS], bool NotEligible) = 0;

	virtual void SaveTeamScore(int *pClientIDs, unsigned int Size, float Time) = 0;

	virtual void ShowTop5(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut=1) = 0;
	virtual void ShowRank(int ClientID, const char *pName, bool Search=false) = 0;

	virtual void ShowTeamTop5(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut=1) = 0;
	virtual void ShowTeamRank(int ClientID, const char *pName, bool Search=false) = 0;

	virtual void ShowTopPoints(IConsole::IResult *pResult, int ClientID, void *pUserData, int Debut=1) = 0;
	virtual void ShowPoints(int ClientID, const char *pName, bool Search=false) = 0;

	virtual void RandomMap(int ClientID, int Stars) = 0;
	virtual void RandomUnfinishedMap(int ClientID, int Stars) = 0;

	virtual void SaveTeam(int Team, const char *pCode, int ClientID, const char *pServer) = 0;
	virtual void LoadTeam(const char *pCode, int ClientID) = 0;

	// called when the server is shut down but not on mapchange/reload
	virtual void OnShutdown() = 0;
};

#endif // GAME_SERVER_SCORE_H
