/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <algorithm> // sort  TODO: remove this

#include <base/math.h>
#include <base/system.h>

#include <engine/external/md5/md5.h>
#include <engine/shared/config.h>
#include <engine/shared/memheap.h>
#include <engine/shared/network.h>
#include <engine/shared/protocol.h>

#include <engine/config.h>
#include <engine/console.h>
#include <engine/friends.h>
#include <engine/masterserver.h>
#include <engine/storage.h>

#include <mastersrv/mastersrv.h>

#include <engine/external/json-parser/json.h>

#include "serverbrowser.h"
class SortWrap
{
	typedef bool (CServerBrowser::*SortFunc)(int, int) const;
	SortFunc m_pfnSort;
	CServerBrowser *m_pThis;
public:
	SortWrap(CServerBrowser *t, SortFunc f) : m_pfnSort(f), m_pThis(t) {}
	bool operator()(int a, int b) { return (g_Config.m_BrSortOrder ? (m_pThis->*m_pfnSort)(b, a) : (m_pThis->*m_pfnSort)(a, b)); }
};

CServerBrowser::CServerBrowser()
{
	m_pMasterServer = 0;
	m_ppServerlist = 0;
	m_pSortedServerlist = 0;

	m_NumFavoriteServers = 0;

	mem_zero(m_aServerlistIp, sizeof(m_aServerlistIp));

	m_pFirstReqServer = 0; // request list
	m_pLastReqServer = 0;
	m_NumRequests = 0;

	m_NeedRefresh = 0;

	m_NumSortedServers = 0;
	m_NumSortedServersCapacity = 0;
	m_NumServers = 0;
	m_NumServerCapacity = 0;

	m_Sorthash = 0;
	m_aFilterString[0] = 0;
	m_aFilterGametypeString[0] = 0;

	m_ServerlistType = 0;
	m_BroadcastTime = 0;
	secure_random_fill(m_aTokenSeed, sizeof(m_aTokenSeed));
	m_RequestNumber = 0;

	m_pDDNetInfo = 0;
}

CServerBrowser::~CServerBrowser()
{
	if(m_pDDNetInfo)
		json_value_free(m_pDDNetInfo);
}

void CServerBrowser::SetBaseInfo(class CNetClient *pClient, const char *pNetVersion)
{
	m_pNetClient = pClient;
	str_copy(m_aNetVersion, pNetVersion, sizeof(m_aNetVersion));
	m_pMasterServer = Kernel()->RequestInterface<IMasterServer>();
	m_pConsole = Kernel()->RequestInterface<IConsole>();
	m_pFriends = Kernel()->RequestInterface<IFriends>();
	IConfig *pConfig = Kernel()->RequestInterface<IConfig>();
	if(pConfig)
		pConfig->RegisterCallback(ConfigSaveCallback, this);
}

const CServerInfo *CServerBrowser::SortedGet(int Index) const
{
	if(Index < 0 || Index >= m_NumSortedServers)
		return 0;
	return &m_ppServerlist[m_pSortedServerlist[Index]]->m_Info;
}

int CServerBrowser::GenerateToken(const NETADDR &Addr) const
{
	md5_state_t Md5;
	md5_byte_t aDigest[16];

	md5_init(&Md5);
	md5_append(&Md5, m_aTokenSeed, sizeof(m_aTokenSeed));
	md5_append(&Md5, (unsigned char *)&Addr, sizeof(Addr));
	md5_finish(&Md5, aDigest);

	return (aDigest[0] << 16) | (aDigest[1] << 8) | aDigest[2];
}

int CServerBrowser::GetBasicToken(int Token)
{
	return Token & 0xff;
}

int CServerBrowser::GetExtraToken(int Token)
{
	return Token >> 8;
}

bool CServerBrowser::SortCompareName(int Index1, int Index2) const
{
	CServerEntry *a = m_ppServerlist[Index1];
	CServerEntry *b = m_ppServerlist[Index2];
	//	make sure empty entries are listed last
	return (a->m_GotInfo && b->m_GotInfo) || (!a->m_GotInfo && !b->m_GotInfo) ? str_comp(a->m_Info.m_aName, b->m_Info.m_aName) < 0 :
			a->m_GotInfo ? true : false;
}

bool CServerBrowser::SortCompareMap(int Index1, int Index2) const
{
	CServerEntry *a = m_ppServerlist[Index1];
	CServerEntry *b = m_ppServerlist[Index2];
	return str_comp(a->m_Info.m_aMap, b->m_Info.m_aMap) < 0;
}

bool CServerBrowser::SortComparePing(int Index1, int Index2) const
{
	CServerEntry *a = m_ppServerlist[Index1];
	CServerEntry *b = m_ppServerlist[Index2];
	return a->m_Info.m_Latency < b->m_Info.m_Latency;
}

bool CServerBrowser::SortCompareGametype(int Index1, int Index2) const
{
	CServerEntry *a = m_ppServerlist[Index1];
	CServerEntry *b = m_ppServerlist[Index2];
	return str_comp(a->m_Info.m_aGameType, b->m_Info.m_aGameType) < 0;
}

bool CServerBrowser::SortCompareNumPlayers(int Index1, int Index2) const
{
	CServerEntry *a = m_ppServerlist[Index1];
	CServerEntry *b = m_ppServerlist[Index2];
	return a->m_Info.m_NumFilteredPlayers < b->m_Info.m_NumFilteredPlayers;
}

bool CServerBrowser::SortCompareNumClients(int Index1, int Index2) const
{
	CServerEntry *a = m_ppServerlist[Index1];
	CServerEntry *b = m_ppServerlist[Index2];
	return a->m_Info.m_NumClients < b->m_Info.m_NumClients;
}

void CServerBrowser::Filter()
{
	int i = 0, p = 0;
	m_NumSortedServers = 0;

	// allocate the sorted list
	if(m_NumSortedServersCapacity < m_NumServers)
	{
		if(m_pSortedServerlist)
			free(m_pSortedServerlist);
		m_NumSortedServersCapacity = m_NumServers;
		m_pSortedServerlist = (int *)calloc(m_NumSortedServersCapacity, sizeof(int));
	}

	// filter the servers
	for(i = 0; i < m_NumServers; i++)
	{
		int Filtered = 0;

		if(g_Config.m_BrFilterEmpty && m_ppServerlist[i]->m_Info.m_NumFilteredPlayers == 0)
			Filtered = 1;
		else if(g_Config.m_BrFilterFull && Players(m_ppServerlist[i]->m_Info) == Max(m_ppServerlist[i]->m_Info))
			Filtered = 1;
		else if(g_Config.m_BrFilterPw && m_ppServerlist[i]->m_Info.m_Flags&SERVER_FLAG_PASSWORD)
			Filtered = 1;
		else if(g_Config.m_BrFilterPure &&
			(str_comp(m_ppServerlist[i]->m_Info.m_aGameType, "DM") != 0 &&
			str_comp(m_ppServerlist[i]->m_Info.m_aGameType, "TDM") != 0 &&
			str_comp(m_ppServerlist[i]->m_Info.m_aGameType, "CTF") != 0))
		{
			Filtered = 1;
		}
		else if(g_Config.m_BrFilterPureMap &&
			!(str_comp(m_ppServerlist[i]->m_Info.m_aMap, "dm1") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "dm2") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "dm6") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "dm7") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "dm8") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "dm9") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf1") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf2") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf3") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf4") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf5") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf6") == 0 ||
			str_comp(m_ppServerlist[i]->m_Info.m_aMap, "ctf7") == 0)
		)
		{
			Filtered = 1;
		}
		else if(g_Config.m_BrFilterPing < m_ppServerlist[i]->m_Info.m_Latency)
			Filtered = 1;
		else if(g_Config.m_BrFilterCompatversion && str_comp_num(m_ppServerlist[i]->m_Info.m_aVersion, m_aNetVersion, 3) != 0)
			Filtered = 1;
		else if(g_Config.m_BrFilterServerAddress[0] && !str_find_nocase(m_ppServerlist[i]->m_Info.m_aAddress, g_Config.m_BrFilterServerAddress))
			Filtered = 1;
		else if(g_Config.m_BrFilterGametypeStrict && g_Config.m_BrFilterGametype[0] && str_comp_nocase(m_ppServerlist[i]->m_Info.m_aGameType, g_Config.m_BrFilterGametype))
			Filtered = 1;
		else if(!g_Config.m_BrFilterGametypeStrict && g_Config.m_BrFilterGametype[0] && !str_find_nocase(m_ppServerlist[i]->m_Info.m_aGameType, g_Config.m_BrFilterGametype))
			Filtered = 1;
		else if(g_Config.m_BrFilterUnfinishedMap && m_ppServerlist[i]->m_Info.m_HasRank != 0)
			Filtered = 1;
		else
		{
			if(g_Config.m_BrFilterCountry)
			{
				Filtered = 1;
				// match against player country
				for(p = 0; p < m_ppServerlist[i]->m_Info.m_NumClients; p++)
				{
					if(m_ppServerlist[i]->m_Info.m_aClients[p].m_Country == g_Config.m_BrFilterCountryIndex)
					{
						Filtered = 0;
						break;
					}
				}
			}

			if(!Filtered && g_Config.m_BrFilterString[0] != 0)
			{
				int MatchFound = 0;

				m_ppServerlist[i]->m_Info.m_QuickSearchHit = 0;

				// match against server name
				if(str_find_nocase(m_ppServerlist[i]->m_Info.m_aName, g_Config.m_BrFilterString))
				{
					MatchFound = 1;
					m_ppServerlist[i]->m_Info.m_QuickSearchHit |= IServerBrowser::QUICK_SERVERNAME;
				}

				// match against players
				for(p = 0; p < m_ppServerlist[i]->m_Info.m_NumClients; p++)
				{
					if(str_find_nocase(m_ppServerlist[i]->m_Info.m_aClients[p].m_aName, g_Config.m_BrFilterString) ||
						str_find_nocase(m_ppServerlist[i]->m_Info.m_aClients[p].m_aClan, g_Config.m_BrFilterString))
					{
						MatchFound = 1;
						m_ppServerlist[i]->m_Info.m_QuickSearchHit |= IServerBrowser::QUICK_PLAYER;
						break;
					}
				}

				// match against map
				if(str_find_nocase(m_ppServerlist[i]->m_Info.m_aMap, g_Config.m_BrFilterString))
				{
					MatchFound = 1;
					m_ppServerlist[i]->m_Info.m_QuickSearchHit |= IServerBrowser::QUICK_MAPNAME;
				}

				if(!MatchFound)
					Filtered = 1;
			}

			if(!Filtered && g_Config.m_BrExcludeString[0] != 0)
			{
				int MatchFound = 0;

				// match against server name
				if(str_find_nocase(m_ppServerlist[i]->m_Info.m_aName, g_Config.m_BrExcludeString))
				{
					MatchFound = 1;
				}

				// match against map
				if(str_find_nocase(m_ppServerlist[i]->m_Info.m_aMap, g_Config.m_BrExcludeString))
				{
					MatchFound = 1;
				}

				// match against gametype
				if(str_find_nocase(m_ppServerlist[i]->m_Info.m_aGameType, g_Config.m_BrExcludeString))
				{
					MatchFound = 1;
				}

				if(MatchFound)
					Filtered = 1;
			}
		}

		if(Filtered == 0)
		{
			// check for friend
			m_ppServerlist[i]->m_Info.m_FriendState = IFriends::FRIEND_NO;
			for(p = 0; p < m_ppServerlist[i]->m_Info.m_NumClients; p++)
			{
				m_ppServerlist[i]->m_Info.m_aClients[p].m_FriendState = m_pFriends->GetFriendState(m_ppServerlist[i]->m_Info.m_aClients[p].m_aName,
					m_ppServerlist[i]->m_Info.m_aClients[p].m_aClan);
				m_ppServerlist[i]->m_Info.m_FriendState = max(m_ppServerlist[i]->m_Info.m_FriendState, m_ppServerlist[i]->m_Info.m_aClients[p].m_FriendState);
			}

			if(!g_Config.m_BrFilterFriends || m_ppServerlist[i]->m_Info.m_FriendState != IFriends::FRIEND_NO)
				m_pSortedServerlist[m_NumSortedServers++] = i;
		}
	}
}

int CServerBrowser::SortHash() const
{
	int i = g_Config.m_BrSort&0xff;
	i |= g_Config.m_BrFilterEmpty<<4;
	i |= g_Config.m_BrFilterFull<<5;
	i |= g_Config.m_BrFilterSpectators<<6;
	i |= g_Config.m_BrFilterFriends<<7;
	i |= g_Config.m_BrFilterPw<<8;
	i |= g_Config.m_BrSortOrder<<9;
	i |= g_Config.m_BrFilterCompatversion<<10;
	i |= g_Config.m_BrFilterPure<<11;
	i |= g_Config.m_BrFilterPureMap<<12;
	i |= g_Config.m_BrFilterGametypeStrict<<13;
	i |= g_Config.m_BrFilterUnfinishedMap<<14;
	i |= g_Config.m_BrFilterCountry<<15;
	i |= g_Config.m_BrFilterConnectingPlayers<<16;
	return i;
}

void SetFilteredPlayers(const CServerInfo &Item)
{
	Item.m_NumFilteredPlayers = 0;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		const CServerInfo::CClient &Client = Item.m_aClients[i];

		if(Client.m_aName[0] == '\0')
			continue;

		if(g_Config.m_BrFilterSpectators && !Client.m_Player)
			continue;

		if(g_Config.m_BrFilterConnectingPlayers && str_comp(Client.m_aName, "(connecting)") == 0 && Client.m_aClan[0] == '\0' && Client.m_Country == -1 && Client.m_Score == 0)
			continue;

		Item.m_NumFilteredPlayers++;
	}
}

void CServerBrowser::Sort()
{
	int i;

	// fill m_NumFilteredPlayers
	for(i = 0; i < m_NumServers; i++)
	{
		SetFilteredPlayers(m_ppServerlist[i]->m_Info);
	}

	// create filtered list
	Filter();

	// sort
	if(g_Config.m_BrSort == IServerBrowser::SORT_NAME)
		std::stable_sort(m_pSortedServerlist, m_pSortedServerlist+m_NumSortedServers, SortWrap(this, &CServerBrowser::SortCompareName));
	else if(g_Config.m_BrSort == IServerBrowser::SORT_PING)
		std::stable_sort(m_pSortedServerlist, m_pSortedServerlist+m_NumSortedServers, SortWrap(this, &CServerBrowser::SortComparePing));
	else if(g_Config.m_BrSort == IServerBrowser::SORT_MAP)
		std::stable_sort(m_pSortedServerlist, m_pSortedServerlist+m_NumSortedServers, SortWrap(this, &CServerBrowser::SortCompareMap));
	else if(g_Config.m_BrSort == IServerBrowser::SORT_NUMPLAYERS)
		std::stable_sort(m_pSortedServerlist, m_pSortedServerlist+m_NumSortedServers, SortWrap(this, &CServerBrowser::SortCompareNumPlayers));
	else if(g_Config.m_BrSort == IServerBrowser::SORT_GAMETYPE)
		std::stable_sort(m_pSortedServerlist, m_pSortedServerlist+m_NumSortedServers, SortWrap(this, &CServerBrowser::SortCompareGametype));

	// set indexes
	for(i = 0; i < m_NumSortedServers; i++)
		m_ppServerlist[m_pSortedServerlist[i]]->m_Info.m_SortedIndex = i;

	str_copy(m_aFilterGametypeString, g_Config.m_BrFilterGametype, sizeof(m_aFilterGametypeString));
	str_copy(m_aFilterString, g_Config.m_BrFilterString, sizeof(m_aFilterString));
	m_Sorthash = SortHash();
}

void CServerBrowser::RemoveRequest(CServerEntry *pEntry)
{
	if(pEntry->m_pPrevReq || pEntry->m_pNextReq || m_pFirstReqServer == pEntry)
	{
		if(pEntry->m_pPrevReq)
			pEntry->m_pPrevReq->m_pNextReq = pEntry->m_pNextReq;
		else
			m_pFirstReqServer = pEntry->m_pNextReq;

		if(pEntry->m_pNextReq)
			pEntry->m_pNextReq->m_pPrevReq = pEntry->m_pPrevReq;
		else
			m_pLastReqServer = pEntry->m_pPrevReq;

		pEntry->m_pPrevReq = 0;
		pEntry->m_pNextReq = 0;
		m_NumRequests--;
	}
}

CServerBrowser::CServerEntry *CServerBrowser::Find(const NETADDR &Addr)
{
	CServerEntry *pEntry = m_aServerlistIp[Addr.ip[0]];

	for(; pEntry; pEntry = pEntry->m_pNextIp)
	{
		if(net_addr_comp(&pEntry->m_Addr, &Addr) == 0)
			return pEntry;
	}
	return (CServerEntry*)0;
}

void CServerBrowser::QueueRequest(CServerEntry *pEntry)
{
	// add it to the list of servers that we should request info from
	pEntry->m_pPrevReq = m_pLastReqServer;
	if(m_pLastReqServer)
		m_pLastReqServer->m_pNextReq = pEntry;
	else
		m_pFirstReqServer = pEntry;
	m_pLastReqServer = pEntry;
	pEntry->m_pNextReq = 0;
	m_NumRequests++;
}

void CServerBrowser::SetInfo(CServerEntry *pEntry, const CServerInfo &Info)
{
	int Fav = pEntry->m_Info.m_Favorite;
	pEntry->m_Info = Info;
	pEntry->m_Info.m_Favorite = Fav;
	pEntry->m_Info.m_NetAddr = pEntry->m_Addr;

	// all these are just for nice compatibility
	if(pEntry->m_Info.m_aGameType[0] == '0' && pEntry->m_Info.m_aGameType[1] == 0)
		str_copy(pEntry->m_Info.m_aGameType, "DM", sizeof(pEntry->m_Info.m_aGameType));
	else if(pEntry->m_Info.m_aGameType[0] == '1' && pEntry->m_Info.m_aGameType[1] == 0)
		str_copy(pEntry->m_Info.m_aGameType, "TDM", sizeof(pEntry->m_Info.m_aGameType));
	else if(pEntry->m_Info.m_aGameType[0] == '2' && pEntry->m_Info.m_aGameType[1] == 0)
		str_copy(pEntry->m_Info.m_aGameType, "CTF", sizeof(pEntry->m_Info.m_aGameType));

	/*if(!request)
	{
		pEntry->m_Info.latency = (time_get()-pEntry->request_time)*1000/time_freq();
		RemoveRequest(pEntry);
	}*/

	pEntry->m_GotInfo = 1;
}

CServerBrowser::CServerEntry *CServerBrowser::Add(const NETADDR &Addr)
{
	int Hash = Addr.ip[0];
	CServerEntry *pEntry = 0;
	int i;

	// create new pEntry
	pEntry = (CServerEntry *)m_ServerlistHeap.Allocate(sizeof(CServerEntry));
	mem_zero(pEntry, sizeof(CServerEntry));

	// set the info
	pEntry->m_Addr = Addr;
	pEntry->m_Info.m_NetAddr = Addr;

	pEntry->m_Info.m_Latency = 999;
	pEntry->m_Info.m_HasRank = -1;
	net_addr_str(&Addr, pEntry->m_Info.m_aAddress, sizeof(pEntry->m_Info.m_aAddress), true);
	str_copy(pEntry->m_Info.m_aName, pEntry->m_Info.m_aAddress, sizeof(pEntry->m_Info.m_aName));

	// check if it's a favorite
	for(i = 0; i < m_NumFavoriteServers; i++)
	{
		if(net_addr_comp(&Addr, &m_aFavoriteServers[i]) == 0)
			pEntry->m_Info.m_Favorite = 1;
	}

	// add to the hash list
	pEntry->m_pNextIp = m_aServerlistIp[Hash];
	m_aServerlistIp[Hash] = pEntry;

	if(m_NumServers == m_NumServerCapacity)
	{
		CServerEntry **ppNewlist;
		m_NumServerCapacity += 100;
		ppNewlist = (CServerEntry **)calloc(m_NumServerCapacity, sizeof(CServerEntry *));
		mem_copy(ppNewlist, m_ppServerlist, m_NumServers*sizeof(CServerEntry*));
		free(m_ppServerlist);
		m_ppServerlist = ppNewlist;
	}

	// add to list
	m_ppServerlist[m_NumServers] = pEntry;
	pEntry->m_Info.m_ServerIndex = m_NumServers;
	m_NumServers++;

	return pEntry;
}

void CServerBrowser::Set(const NETADDR &Addr, int Type, int Token, const CServerInfo *pInfo)
{
	CServerEntry *pEntry = 0;
	if(Type == IServerBrowser::SET_MASTER_ADD)
	{
		if(m_ServerlistType != IServerBrowser::TYPE_INTERNET)
			return;
		m_LastPacketTick = 0;
		if(!Find(Addr))
		{
			pEntry = Add(Addr);
			QueueRequest(pEntry);
		}
	}
	else if(Type == IServerBrowser::SET_FAV_ADD)
	{
		if(m_ServerlistType != IServerBrowser::TYPE_FAVORITES)
			return;

		if(!Find(Addr))
		{
			pEntry = Add(Addr);
			QueueRequest(pEntry);
		}
	}
	else if(Type == IServerBrowser::SET_DDNET_ADD)
	{
		if(m_ServerlistType != IServerBrowser::TYPE_DDNET)
			return;

		if(!Find(Addr))
		{
			pEntry = Add(Addr);
			QueueRequest(pEntry);
		}
	}
	else if(Type == IServerBrowser::SET_TOKEN)
	{
		int BasicToken = Token;
		int ExtraToken = 0;
		if(pInfo->m_Type == SERVERINFO_EXTENDED)
		{
			BasicToken = Token & 0xff;
			ExtraToken = Token >> 8;
		}

		pEntry = Find(Addr);
		if(m_ServerlistType != IServerBrowser::TYPE_LAN)
		{
			if(!pEntry)
			{
				return;
			}
			int Token = GenerateToken(Addr);
			bool Drop = false;
			Drop = Drop || BasicToken != GetBasicToken(Token);
			Drop = Drop || (pInfo->m_Type == SERVERINFO_EXTENDED && ExtraToken != GetExtraToken(Token));
			if(Drop)
			{
				return;
			}
		}
		if(!pEntry)
			pEntry = Add(Addr);
		if(pEntry)
		{
			if(m_ServerlistType == IServerBrowser::TYPE_LAN)
			{
				NETADDR Broadcast;
				mem_zero(&Broadcast, sizeof(Broadcast));
				Broadcast.type = m_pNetClient->NetType()|NETTYPE_LINK_BROADCAST;
				int Token = GenerateToken(Broadcast);
				bool Drop = false;
				Drop = Drop || BasicToken != GetBasicToken(Token);
				Drop = Drop || (pInfo->m_Type == SERVERINFO_EXTENDED && ExtraToken != GetExtraToken(Token));
				if(Drop)
				{
					return;
				}
			}
			SetInfo(pEntry, *pInfo);
			if (m_ServerlistType == IServerBrowser::TYPE_LAN)
				pEntry->m_Info.m_Latency = min(static_cast<int>((time_get()-m_BroadcastTime)*1000/time_freq()), 999);
			else if (pEntry->m_RequestTime > 0)
			{
				pEntry->m_Info.m_Latency = min(static_cast<int>((time_get()-pEntry->m_RequestTime)*1000/time_freq()), 999);
				pEntry->m_RequestTime = -1; // Request has been answered
			}
			RemoveRequest(pEntry);
		}
	}

	Sort();
}

void CServerBrowser::Refresh(int Type)
{
	// clear out everything
	m_ServerlistHeap.Reset();
	m_NumServers = 0;
	m_NumSortedServers = 0;
	mem_zero(m_aServerlistIp, sizeof(m_aServerlistIp));
	m_pFirstReqServer = 0;
	m_pLastReqServer = 0;
	m_NumRequests = 0;
	m_CurrentMaxRequests = g_Config.m_BrMaxRequests;
	m_RequestNumber++;

	m_ServerlistType = Type;

	if(Type == IServerBrowser::TYPE_LAN)
	{
		unsigned char Buffer[sizeof(SERVERBROWSE_GETINFO)+1];
		CNetChunk Packet;
		int i;

		/* do the broadcast version */
		Packet.m_ClientID = -1;
		mem_zero(&Packet, sizeof(Packet));
		Packet.m_Address.type = m_pNetClient->NetType()|NETTYPE_LINK_BROADCAST;
		Packet.m_Flags = NETSENDFLAG_CONNLESS|NETSENDFLAG_EXTENDED;
		Packet.m_DataSize = sizeof(Buffer);
		Packet.m_pData = Buffer;
		mem_zero(&Packet.m_aExtraData, sizeof(Packet.m_aExtraData));

		int Token = GenerateToken(Packet.m_Address);
		mem_copy(Buffer, SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO));
		Buffer[sizeof(SERVERBROWSE_GETINFO)] = GetBasicToken(Token);

		Packet.m_aExtraData[0] = GetExtraToken(Token) >> 8;
		Packet.m_aExtraData[1] = GetExtraToken(Token) & 0xff;

		m_BroadcastTime = time_get();

		for(i = 8303; i <= 8310; i++)
		{
			Packet.m_Address.port = i;
			m_pNetClient->Send(&Packet);
		}

		if(g_Config.m_Debug)
			m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", "broadcasting for servers");
	}
	else if(Type == IServerBrowser::TYPE_INTERNET)
		m_NeedRefresh = 1;
	else if(Type == IServerBrowser::TYPE_FAVORITES)
	{
		for(int i = 0; i < m_NumFavoriteServers; i++)
			Set(m_aFavoriteServers[i], IServerBrowser::SET_FAV_ADD, -1, 0);
	}
	else if(Type == IServerBrowser::TYPE_DDNET)
	{
		// remove unknown elements of exclude list
		DDNetCountryFilterClean();
		DDNetTypeFilterClean();

		for(int i = 0; i < m_NumDDNetCountries; i++)
		{
			CDDNetCountry *pCntr = &m_aDDNetCountries[i];

			// check for filter
			if(DDNetFiltered(g_Config.m_BrFilterExcludeCountries, pCntr->m_aName))
				continue;

			for(int g = 0; g < pCntr->m_NumServers; g++)
			{
				if(!DDNetFiltered(g_Config.m_BrFilterExcludeTypes, pCntr->m_aTypes[g]))
					Set(pCntr->m_aServers[g], IServerBrowser::SET_DDNET_ADD, -1, 0);
			}
		}
	}
}

void CServerBrowser::RequestImpl(const NETADDR &Addr, CServerEntry *pEntry) const
{
	unsigned char Buffer[sizeof(SERVERBROWSE_GETINFO)+1];
	CNetChunk Packet;

	if(g_Config.m_Debug)
	{
		char aAddrStr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aAddrStr, sizeof(aAddrStr), true);
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf),"requesting server info from %s", aAddrStr);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", aBuf);
	}

	int Token = GenerateToken(Addr);

	mem_copy(Buffer, SERVERBROWSE_GETINFO, sizeof(SERVERBROWSE_GETINFO));
	Buffer[sizeof(SERVERBROWSE_GETINFO)] = GetBasicToken(Token);

	Packet.m_ClientID = -1;
	Packet.m_Address = Addr;
	Packet.m_Flags = NETSENDFLAG_CONNLESS|NETSENDFLAG_EXTENDED;
	Packet.m_DataSize = sizeof(Buffer);
	Packet.m_pData = Buffer;
	mem_zero(&Packet.m_aExtraData, sizeof(Packet.m_aExtraData));
	Packet.m_aExtraData[0] = GetExtraToken(Token) >> 8;
	Packet.m_aExtraData[1] = GetExtraToken(Token) & 0xff;

	m_pNetClient->Send(&Packet);

	if(pEntry)
		pEntry->m_RequestTime = time_get();
}

void CServerBrowser::RequestImpl64(const NETADDR &Addr, CServerEntry *pEntry) const
{
	unsigned char Buffer[sizeof(SERVERBROWSE_GETINFO_64_LEGACY)+1];
	CNetChunk Packet;

	if(g_Config.m_Debug)
	{
		char aAddrStr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aAddrStr, sizeof(aAddrStr), true);
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf),"requesting server info 64 from %s", aAddrStr);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", aBuf);
	}

	mem_copy(Buffer, SERVERBROWSE_GETINFO_64_LEGACY, sizeof(SERVERBROWSE_GETINFO_64_LEGACY));
	Buffer[sizeof(SERVERBROWSE_GETINFO_64_LEGACY)] = GetBasicToken(GenerateToken(Addr));

	Packet.m_ClientID = -1;
	Packet.m_Address = Addr;
	Packet.m_Flags = NETSENDFLAG_CONNLESS;
	Packet.m_DataSize = sizeof(Buffer);
	Packet.m_pData = Buffer;

	m_pNetClient->Send(&Packet);

	if(pEntry)
		pEntry->m_RequestTime = time_get();
}

void CServerBrowser::RequestCurrentServer(const NETADDR &Addr) const
{
	RequestImpl(Addr, 0);
}


void CServerBrowser::Update(bool ForceResort)
{
	int64 Timeout = time_freq();
	int64 Now = time_get();
	int Count;
	CServerEntry *pEntry, *pNext;

	// do server list requests
	if(m_NeedRefresh && !m_pMasterServer->IsRefreshing())
	{
		NETADDR Addr;
		CNetChunk Packet;
		int i = 0;

		m_NeedRefresh = 0;
		m_MasterServerCount = -1;
		mem_zero(&Packet, sizeof(Packet));
		Packet.m_ClientID = -1;
		Packet.m_Flags = NETSENDFLAG_CONNLESS;
		Packet.m_DataSize = sizeof(SERVERBROWSE_GETCOUNT);
		Packet.m_pData = SERVERBROWSE_GETCOUNT;

		for(i = 0; i < IMasterServer::MAX_MASTERSERVERS; i++)
		{
			if(!m_pMasterServer->IsValid(i))
				continue;

			Addr = m_pMasterServer->GetAddr(i);
			m_pMasterServer->SetCount(i, -1);
			Packet.m_Address = Addr;
			m_pNetClient->Send(&Packet);
			if(g_Config.m_Debug)
			{
				dbg_msg("client_srvbrowse", "count-request sent to %d", i);
			}
		}
	}

	//Check if all server counts arrived
	if(m_MasterServerCount == -1)
	{
		m_MasterServerCount = 0;
		for(int i = 0; i < IMasterServer::MAX_MASTERSERVERS; i++)
			{
				if(!m_pMasterServer->IsValid(i))
					continue;
				int Count = m_pMasterServer->GetCount(i);
				if(Count == -1)
				{
				/* ignore Server
					m_MasterServerCount = -1;
					return;
					// we don't have the required server information
					*/
				}
				else
					m_MasterServerCount += Count;
			}
		//request Server-List
		NETADDR Addr;
		CNetChunk Packet;
		mem_zero(&Packet, sizeof(Packet));
		Packet.m_ClientID = -1;
		Packet.m_Flags = NETSENDFLAG_CONNLESS;
		Packet.m_DataSize = sizeof(SERVERBROWSE_GETLIST);
		Packet.m_pData = SERVERBROWSE_GETLIST;

		for(int i = 0; i < IMasterServer::MAX_MASTERSERVERS; i++)
		{
			if(!m_pMasterServer->IsValid(i))
				continue;

			Addr = m_pMasterServer->GetAddr(i);
			Packet.m_Address = Addr;
			m_pNetClient->Send(&Packet);
		}
		if(g_Config.m_Debug)
		{
			dbg_msg("client_srvbrowse", "servercount: %d, requesting server list", m_MasterServerCount);
		}
		m_LastPacketTick = 0;
	}
	else if(m_MasterServerCount > -1)
	{
		m_MasterServerCount = 0;
		for(int i = 0; i < IMasterServer::MAX_MASTERSERVERS; i++)
			{
				if(!m_pMasterServer->IsValid(i))
					continue;
				int Count = m_pMasterServer->GetCount(i);
				if(Count == -1)
				{
				/* ignore Server
					m_MasterServerCount = -1;
					return;
					// we don't have the required server information
					*/
				}
				else
					m_MasterServerCount += Count;
			}
			//if(g_Config.m_Debug)
			//{
			//	dbg_msg("client_srvbrowse", "ServerCount2: %d", m_MasterServerCount);
			//}
	}
	if(m_MasterServerCount > m_NumRequests  + m_LastPacketTick)
	{
		++m_LastPacketTick;
		return; //wait for more packets
	}
	pEntry = m_pFirstReqServer;
	Count = 0;
	while(1)
	{
		if(!pEntry) // no more entries
			break;
		if(pEntry->m_RequestTime && pEntry->m_RequestTime+Timeout < Now)
		{
			pEntry = pEntry->m_pNextReq;
			continue;
		}
		// no more then 10 concurrent requests
		if(Count == m_CurrentMaxRequests)
			break;

		if(pEntry->m_RequestTime == 0)
		{
			if (pEntry->m_Request64Legacy)
				RequestImpl64(pEntry->m_Addr, pEntry);
			else
				RequestImpl(pEntry->m_Addr, pEntry);
		}

		Count++;
		pEntry = pEntry->m_pNextReq;
	}

	if(m_pFirstReqServer && Count == 0 && m_CurrentMaxRequests > 1) //NO More current Server Requests
	{
		//reset old ones
		pEntry = m_pFirstReqServer;
		while(1)
		{
			if(!pEntry) // no more entries
				break;
			pEntry->m_RequestTime = 0;
			pEntry = pEntry->m_pNextReq;
		}

		//update max-requests
		m_CurrentMaxRequests = m_CurrentMaxRequests/2;
		if(m_CurrentMaxRequests < 1)
			m_CurrentMaxRequests = 1;
	}
	else if(Count == 0 && m_CurrentMaxRequests == 1) //we reached the limit, just release all left requests. IF a server sends us a packet, a new request will be added automatically, so we can delete all
	{
		pEntry = m_pFirstReqServer;
		while(1)
		{
			if(!pEntry) // no more entries
				break;
			pNext = pEntry->m_pNextReq;
			RemoveRequest(pEntry);	//release request
			pEntry = pNext;
		}
	}

	// check if we need to resort
	if(m_Sorthash != SortHash() || ForceResort)
		Sort();
}


bool CServerBrowser::IsFavorite(const NETADDR &Addr) const
{
	// search for the address
	int i;
	for(i = 0; i < m_NumFavoriteServers; i++)
	{
		if(net_addr_comp(&Addr, &m_aFavoriteServers[i]) == 0)
			return true;
	}
	return false;
}

void CServerBrowser::AddFavorite(const NETADDR &Addr)
{
	CServerEntry *pEntry;

	if(m_NumFavoriteServers == MAX_FAVORITES)
		return;

	// make sure that we don't already have the server in our list
	for(int i = 0; i < m_NumFavoriteServers; i++)
	{
		if(net_addr_comp(&Addr, &m_aFavoriteServers[i]) == 0)
			return;
	}

	// add the server to the list
	m_aFavoriteServers[m_NumFavoriteServers++] = Addr;
	pEntry = Find(Addr);
	if(pEntry)
		pEntry->m_Info.m_Favorite = 1;

	if(g_Config.m_Debug)
	{
		char aAddrStr[NETADDR_MAXSTRSIZE];
		net_addr_str(&Addr, aAddrStr, sizeof(aAddrStr), true);
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "added fav, %s", aAddrStr);
		m_pConsole->Print(IConsole::OUTPUT_LEVEL_DEBUG, "client_srvbrowse", aBuf);
	}
}

void CServerBrowser::RemoveFavorite(const NETADDR &Addr)
{
	int i;
	CServerEntry *pEntry;

	for(i = 0; i < m_NumFavoriteServers; i++)
	{
		if(net_addr_comp(&Addr, &m_aFavoriteServers[i]) == 0)
		{
			mem_move(&m_aFavoriteServers[i], &m_aFavoriteServers[i+1], sizeof(NETADDR)*(m_NumFavoriteServers-(i+1)));
			m_NumFavoriteServers--;

			pEntry = Find(Addr);
			if(pEntry)
				pEntry->m_Info.m_Favorite = 0;

			return;
		}
	}
}

void CServerBrowser::LoadDDNetServers()
{
	if (!m_pDDNetInfo)
		return;

	// parse JSON
	const json_value *pServers = json_object_get(m_pDDNetInfo, "servers");

	if (!pServers || pServers->type != json_array)
		return;

	// reset servers / countries
	m_NumDDNetCountries = 0;
	m_NumDDNetTypes = 0;

	for (int i = 0; i < json_array_length(pServers) && m_NumDDNetCountries < MAX_DDNET_COUNTRIES; i++)
	{
		// pSrv - { name, flagId, servers }
		const json_value *pSrv = json_array_get(pServers, i);
		const json_value *pTypes = json_object_get(pSrv, "servers");
		const json_value *pName = json_object_get(pSrv, "name");
		const json_value *pFlagID = json_object_get(pSrv, "flagId");

		if (pSrv->type != json_object || pTypes->type != json_object || pName->type != json_string || pFlagID->type != json_integer)
		{
			dbg_msg("client_srvbrowse", "invalid attributes");
			continue;
		}

		// build structure
		CDDNetCountry *pCntr = &m_aDDNetCountries[m_NumDDNetCountries];

		pCntr->Reset();

		str_copy(pCntr->m_aName, json_string_get(pName), sizeof(pCntr->m_aName));
		pCntr->m_FlagID = json_int_get(pFlagID);

		// add country
		for (unsigned int t = 0; t < pTypes->u.object.length; t++)
		{
			const char *pType = pTypes->u.object.values[t].name;
			const json_value *pAddrs = pTypes->u.object.values[t].value;

			if (pAddrs->type != json_array)
			{
				dbg_msg("client_srvbrowse", "invalid attributes");
				continue;
			}

			// add type
			if(json_array_length(pAddrs) > 0 && m_NumDDNetTypes < MAX_DDNET_TYPES)
			{
				int Pos;
				for(Pos = 0; Pos < m_NumDDNetTypes; Pos++)
				{
					if(!str_comp(m_aDDNetTypes[Pos], pType))
						break;
				}
				if(Pos == m_NumDDNetTypes)
				{
					str_copy(m_aDDNetTypes[m_NumDDNetTypes], pType, sizeof(m_aDDNetTypes[m_NumDDNetTypes]));
					m_NumDDNetTypes++;
				}
			}

			// add addresses
			for (int g = 0; g < json_array_length(pAddrs); g++, pCntr->m_NumServers++)
			{
				const json_value *pAddr = json_array_get(pAddrs, g);
				if (pAddr->type != json_string)
				{
					dbg_msg("client_srvbrowse", "invalid attributes");
					continue;
				}
				const char *pStr = json_string_get(pAddr);
				net_addr_from_str(&pCntr->m_aServers[pCntr->m_NumServers], pStr);
				str_copy(pCntr->m_aTypes[pCntr->m_NumServers], pType, sizeof(pCntr->m_aTypes[pCntr->m_NumServers]));
			}
		}

		m_NumDDNetCountries++;
	}
}

void CServerBrowser::LoadDDNetRanks()
{
	for(int i = 0; i < m_NumServers; i++)
	{
		if(m_ppServerlist[i]->m_Info.m_aMap[0])
			m_ppServerlist[i]->m_Info.m_HasRank = HasRank(m_ppServerlist[i]->m_Info.m_aMap);
	}
}

int CServerBrowser::HasRank(const char *pMap)
{
	if(m_ServerlistType != IServerBrowser::TYPE_DDNET || !m_pDDNetInfo)
		return -1;

	const json_value *pDDNetRanks = json_object_get(m_pDDNetInfo, "maps");

	if(!pDDNetRanks || pDDNetRanks->type != json_array)
		return -1;

	for (int i = 0; i < json_array_length(pDDNetRanks); i++)
	{
		const json_value *pJson = json_array_get(pDDNetRanks, i);
		if(!pJson || pJson->type != json_string)
			continue;

		const char *pStr = json_string_get(pJson);

		if(str_comp(pMap, pStr) == 0)
			return 1;
	}

	return 0;
}

void CServerBrowser::LoadDDNetInfoJson()
{
	IStorage *pStorage = Kernel()->RequestInterface<IStorage>();
	IOHANDLE File = pStorage->OpenFile(DDNET_INFO, IOFLAG_READ, IStorage::TYPE_SAVE);

	if(!File)
		return;

	const int Length = io_length(File);
	if(Length <= 0)
	{
		io_close(File);
		return;
	}

	char *pBuf = (char *)malloc(Length);
	pBuf[0] = '\0';

	io_read(File, pBuf, Length);
	io_close(File);

	if(m_pDDNetInfo)
		json_value_free(m_pDDNetInfo);

	m_pDDNetInfo = json_parse(pBuf, Length);

	free(pBuf);

	if(m_pDDNetInfo && m_pDDNetInfo->type != json_object)
	{
		json_value_free(m_pDDNetInfo);
		m_pDDNetInfo = 0;
	}
}

const json_value *CServerBrowser::LoadDDNetInfo()
{
	LoadDDNetInfoJson();
	LoadDDNetServers();

	if(m_NumServers == 0)
		Refresh(m_ServerlistType);
	else
		LoadDDNetRanks();

	return m_pDDNetInfo;
}

bool CServerBrowser::IsRefreshing() const
{
	return m_pFirstReqServer != 0;
}

bool CServerBrowser::IsRefreshingMasters() const
{
	return m_pMasterServer->IsRefreshing();
}


int CServerBrowser::LoadingProgression() const
{
	if(m_NumServers == 0)
		return 0;

	int Servers = m_NumServers;
	int Loaded = m_NumServers-m_NumRequests;
	return 100.0f * Loaded/Servers;
}


void CServerBrowser::ConfigSaveCallback(IConfig *pConfig, void *pUserData)
{
	CServerBrowser *pSelf = (CServerBrowser *)pUserData;

	char aAddrStr[128];
	char aBuffer[256];
	for(int i = 0; i < pSelf->m_NumFavoriteServers; i++)
	{
		net_addr_str(&pSelf->m_aFavoriteServers[i], aAddrStr, sizeof(aAddrStr), true);
		str_format(aBuffer, sizeof(aBuffer), "add_favorite %s", aAddrStr);
		pConfig->WriteLine(aBuffer);
	}
}

void CServerBrowser::DDNetFilterAdd(char *pFilter, const char *pName)
{
	if (DDNetFiltered(pFilter, pName))
		return;

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), ",%s", pName);
	str_append(pFilter, aBuf, 128);
}

void CServerBrowser::DDNetFilterRem(char *pFilter, const char *pName)
{
	if (!DDNetFiltered(pFilter, pName))
		return;

	// rewrite exclude/filter list
	char aBuf[128];

	str_copy(aBuf, pFilter, sizeof(aBuf));
	pFilter[0] = '\0';

	char aToken[128];
	for(const char *tok = aBuf; (tok = str_next_token(tok, ",", aToken, sizeof(aToken)));)
	{
		if(str_comp_nocase(pName, aToken) != 0)
		{
			char aBuf2[128];
			str_format(aBuf2, sizeof(aBuf2), ",%s", aToken);
			str_append(pFilter, aBuf2, 128);
		}
	}
}

bool CServerBrowser::DDNetFiltered(char *pFilter, const char *pName)
{
	return str_in_list(pFilter, ",", pName); // country not excluded
}

void CServerBrowser::DDNetCountryFilterClean()
{
	char aNewList[128];
	aNewList[0] = '\0';

	for(int i = 0; i < m_NumDDNetCountries; i++)
	{
		const char *pName = m_aDDNetCountries[i].m_aName;
		if(DDNetFiltered(g_Config.m_BrFilterExcludeCountries, pName))
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), ",%s", pName);
			str_append(aNewList, aBuf, sizeof(aNewList));
		}
	}

	str_copy(g_Config.m_BrFilterExcludeCountries, aNewList, sizeof(g_Config.m_BrFilterExcludeCountries));
}

void CServerBrowser::DDNetTypeFilterClean()
{
	char aNewList[128];
	aNewList[0] = '\0';

	for(int i = 0; i < m_NumDDNetTypes; i++)
	{
		const char *pName = m_aDDNetTypes[i];
		if(DDNetFiltered(g_Config.m_BrFilterExcludeTypes, pName))
		{
			char aBuf[128];
			str_format(aBuf, sizeof(aBuf), ",%s", pName);
			str_append(aNewList, aBuf, sizeof(aNewList));
		}
	}

	str_copy(g_Config.m_BrFilterExcludeTypes, aNewList, sizeof(g_Config.m_BrFilterExcludeTypes));
}
