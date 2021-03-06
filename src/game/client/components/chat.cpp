/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.				*/

#include <base/tl/string.h>

#include <engine/engine.h>
#include <engine/graphics.h>
#include <engine/textrender.h>
#include <engine/keys.h>
#include <engine/shared/config.h>

#include <game/generated/protocol.h>
#include <game/generated/client_data.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>

#include <game/client/components/scoreboard.h>
#include <game/client/components/sounds.h>
#include <game/localization.h>

#ifdef CONF_PLATFORM_MACOSX
#include <osx/notification.h>
#endif

#include "hud.h"
#include "chat.h"
#include "console.h"


CChat::CChat()
{
	OnReset();

	m_pTranslator = new(mem_alloc(sizeof(CTranslator), sizeof(void*))) CTranslator();
	m_pTranslator->Init();

	m_GotKeys = false;
	//m_pKeyPair = GenerateKeyPair(512, 3);
}

CChat::~CChat()
{
	if(m_pTranslator)
		mem_free(m_pTranslator);
	m_pTranslator = 0;
}

void CChat::OnReset()
{
	for(int i = 0; i < MAX_LINES; i++)
	{
		m_aLines[i].m_Time = 0;
		m_aLines[i].m_aText[0] = 0;
		m_aLines[i].m_aName[0] = 0;
	}

	m_ReverseTAB = false;
	m_Mode = MODE_NONE;
	m_Show = false;
	m_InputUpdate = false;
	m_ChatStringOffset = 0;
	m_CompletionChosen = -1;
	m_aCompletionBuffer[0] = 0;
	m_PlaceholderOffset = 0;
	m_PlaceholderLength = 0;
	m_pHistoryEntry = 0x0;
	m_PendingChatCounter = 0;
	m_LastChatSend = 0;

	for(int i = 0; i < CHAT_NUM; ++i)
		m_aLastSoundPlayed[i] = 0;
}

void CChat::OnRelease()
{
	m_Show = false;
}

void CChat::OnStateChange(int NewState, int OldState)
{
	if(OldState <= IClient::STATE_CONNECTING)
	{
		m_Mode = MODE_NONE;
		for(int i = 0; i < MAX_LINES; i++)
		{
			m_aLines[i].m_Time = 0;
		}
		m_CurrentLine = 0;
	}
}

void CChat::ConSay(IConsole::IResult *pResult, void *pUserData)
{
	((CChat*)pUserData)->Say(0, pResult->GetString(0));
}

void CChat::ConSayTeam(IConsole::IResult *pResult, void *pUserData)
{
	((CChat*)pUserData)->Say(1, pResult->GetString(0));
}

void CChat::ConChat(IConsole::IResult *pResult, void *pUserData)
{
	const char *pMode = pResult->GetString(0);
	if(str_comp(pMode, "all") == 0)
		((CChat*)pUserData)->EnableMode(0);
	else if(str_comp(pMode, "team") == 0)
		((CChat*)pUserData)->EnableMode(1);
	else if(str_comp(pMode, "hidden") == 0)
		((CChat*)pUserData)->EnableMode(2);
	else if(str_comp(pMode, "crypt") == 0)
		((CChat*)pUserData)->EnableMode(3);
	else
		((CChat*)pUserData)->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "console", "expected all, team, hidden or crypt as mode");

	if(pResult->GetString(1)[0] || g_Config.m_ClChatReset)
		((CChat*)pUserData)->m_Input.Set(pResult->GetString(1));
}

void CChat::ConShowChat(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->m_Show = pResult->GetInteger(0) != 0;
}

void CChat::ConGenKeys(IConsole::IResult *pResult, void *pUserData)
{
	if(pResult->GetInteger(1) % 2 == 0)
	{
		((CChat*)pUserData)->Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "console", "please use a odd number as exponent.");
		return;
	}
	((CChat *)pUserData)->GenerateKeyPair(pResult->GetInteger(0), pResult->GetInteger(1));
}

void CChat::ConSaveKeys(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->SaveKeys(((CChat *)pUserData)->m_pKeyPair, pResult->GetString(0));
}

void CChat::ConLoadKeys(IConsole::IResult *pResult, void *pUserData)
{
	((CChat *)pUserData)->LoadKeys(pResult->GetString(0));
}

void CChat::OnConsoleInit()
{
	Console()->Register("say", "r[message]", CFGFLAG_CLIENT, ConSay, this, "Say in chat");
	Console()->Register("say_team", "r[message]", CFGFLAG_CLIENT, ConSayTeam, this, "Say in team chat");
	Console()->Register("chat", "s['all'|'team'|'hidden'|'crypt'] ?r[message]", CFGFLAG_CLIENT, ConChat, this, "Enable chat with all/team mode");
	Console()->Register("+show_chat", "", CFGFLAG_CLIENT, ConShowChat, this, "Show chat");

	Console()->Register("generate_rsa_keys", "i[Bytes] i[Exponent]", CFGFLAG_CLIENT, ConGenKeys, this, "Usually bytes = 256 and exponent = 3");
	Console()->Register("save_rsa_keys", "s[keyname]", CFGFLAG_CLIENT, ConSaveKeys, this, "Save RSA keys for chat crypt");
	Console()->Register("load_rsa_keys", "s[keyname]", CFGFLAG_CLIENT, ConLoadKeys, this, "Load RSA keys for chat crypt");
}

bool CChat::OnInput(IInput::CEvent Event)
{
	if(m_Mode == MODE_NONE)
		return false;

	if(Input()->KeyIsPressed(KEY_LCTRL) && Input()->KeyPress(KEY_V)) // copy
	{
		const char *Text = Input()->GetClipboardText();
		if(Text)
		{
			// if the text has more than one line, we send all lines except the last one
			// the last one is set as in the text field
			char Line[256];
			int i, Begin = 0;
			for(i = 0; i < str_length(Text); i++)
			{
				if(Text[i] == '\n')
				{
					int max = min(i - Begin + 1, (int)sizeof(Line));
					str_copy(Line, Text + Begin, max);
					Begin = i+1;
					SayChat(Line);
					while(Text[i] == '\n') i++;
				}
			}
			int max = min(i - Begin + 1, (int)sizeof(Line));
			str_copy(Line, Text + Begin, max);
			Begin = i+1;
			m_Input.Add(Line);
		}
	}

	if(Input()->KeyIsPressed(KEY_LCTRL) && Input()->KeyPress(KEY_C)) // paste
	{
		Input()->SetClipboardText(m_Input.GetString());
	}

	if(Input()->KeyIsPressed(KEY_LCTRL)) // jump to spaces and special ASCII characters
	{
		int SearchDirection = 0;
		if(Input()->KeyPress(KEY_LEFT))
			SearchDirection = -1;
		else if(Input()->KeyPress(KEY_RIGHT))
			SearchDirection = 1;

		if(SearchDirection != 0)
		{
			int FoundAt = SearchDirection > 0 ? m_Input.GetLength()-1 : 0;
			for(int i = m_Input.GetCursorOffset()+SearchDirection; SearchDirection > 0 ? i < m_Input.GetLength()-1 : i > 0; i+=SearchDirection)
			{
				int next = i+SearchDirection;
				if(	(m_Input.GetString()[next] == ' ') ||
					(m_Input.GetString()[next] >= 32 && m_Input.GetString()[next] <= 47) ||
					(m_Input.GetString()[next] >= 58 && m_Input.GetString()[next] <= 64) ||
					(m_Input.GetString()[next] >= 91 && m_Input.GetString()[next] <= 96) )
				{
					FoundAt = i;
					if(SearchDirection < 0)
						FoundAt++;
					break;
				}
			}
			m_Input.SetCursorOffset(FoundAt);
		}
	}

	if(Event.m_Flags&IInput::FLAG_PRESS && Event.m_Key == KEY_ESCAPE)
	{
		m_Mode = MODE_NONE;
		m_pClient->OnRelease();
		if(g_Config.m_ClChatReset)
			m_Input.Clear();
	}
	else if(Event.m_Flags&IInput::FLAG_PRESS && (Event.m_Key == KEY_RETURN || Event.m_Key == KEY_KP_ENTER))
	{
		if(m_Input.GetString()[0])
		{
			bool AddEntry = false;

			if(m_LastChatSend+time_freq() < time_get())
			{
				if(m_Mode == MODE_HIDDEN)
					m_CryptSendQueue = std::string(m_Input.GetString());
				else if(m_Mode == MODE_CRYPT)
				{
					char *pEncrypted = EncryptMsg(m_Input.GetString());
					if(pEncrypted)
					{
						Say(0, pEncrypted);
						delete[] pEncrypted;
					}

				}
				else
					Say(m_Mode == MODE_ALL ? 0 : 1, m_Input.GetString());
				AddEntry = true;
			}
			else if(m_PendingChatCounter < 3)
			{
				++m_PendingChatCounter;
				AddEntry = true;
			}

			if(AddEntry)
			{
				CHistoryEntry *pEntry = m_History.Allocate(sizeof(CHistoryEntry)+m_Input.GetLength());
				pEntry->m_Team = m_Mode == MODE_TEAM ? 1 : 0;
				mem_copy(pEntry->m_aText, m_Input.GetString(), m_Input.GetLength()+1);
			}
		}
		m_pHistoryEntry = 0x0;
		m_Mode = MODE_NONE;
		m_pClient->OnRelease();
		m_Input.Clear();
	}
	if(Event.m_Flags&IInput::FLAG_PRESS && Event.m_Key == KEY_TAB)
	{
		// fill the completion buffer
		if(m_CompletionChosen < 0)
		{
			const char *pCursor = m_Input.GetString()+m_Input.GetCursorOffset();
			for(int Count = 0; Count < m_Input.GetCursorOffset() && *(pCursor-1) != ' '; --pCursor, ++Count);
			m_PlaceholderOffset = pCursor-m_Input.GetString();

			for(m_PlaceholderLength = 0; *pCursor && *pCursor != ' '; ++pCursor)
				++m_PlaceholderLength;

			str_copy(m_aCompletionBuffer, m_Input.GetString()+m_PlaceholderOffset, min(static_cast<int>(sizeof(m_aCompletionBuffer)), m_PlaceholderLength+1));
		}

		// find next possible name
		const char *pCompletionString = 0;

			if(m_ReverseTAB)
				m_CompletionChosen = (m_CompletionChosen-1 + 2*MAX_CLIENTS)%(2*MAX_CLIENTS);
			else
				m_CompletionChosen = (m_CompletionChosen+1)%(2*MAX_CLIENTS);

		for(int i = 0; i < 2*MAX_CLIENTS; ++i)
		{
			int SearchType;
			int Index;

			if(m_ReverseTAB)
			{
				SearchType = ((m_CompletionChosen-i +2*MAX_CLIENTS)%(2*MAX_CLIENTS))/MAX_CLIENTS;
				Index = (m_CompletionChosen-i + MAX_CLIENTS )%MAX_CLIENTS;
			}
			else
			{
				SearchType = ((m_CompletionChosen+i)%(2*MAX_CLIENTS))/MAX_CLIENTS;
				Index = (m_CompletionChosen+i)%MAX_CLIENTS;
			}


			if(!m_pClient->m_Snap.m_paInfoByName[Index])
				continue;

			int Index2 = m_pClient->m_Snap.m_paInfoByName[Index]->m_ClientID;

			bool Found = false;
			if(SearchType == 1)
			{
				if(str_comp_nocase_num(m_pClient->m_aClients[Index2].m_aName, m_aCompletionBuffer, str_length(m_aCompletionBuffer)) &&
					str_find_nocase(m_pClient->m_aClients[Index2].m_aName, m_aCompletionBuffer))
					Found = true;
			}
			else if(!str_comp_nocase_num(m_pClient->m_aClients[Index2].m_aName, m_aCompletionBuffer, str_length(m_aCompletionBuffer)))
				Found = true;

			if(Found)
			{
				pCompletionString = m_pClient->m_aClients[Index2].m_aName;
				m_CompletionChosen = Index+SearchType*MAX_CLIENTS;
				break;
			}
		}

		// insert the name
		if(pCompletionString)
		{
			char aBuf[256];
			// add part before the name
			str_copy(aBuf, m_Input.GetString(), min(static_cast<int>(sizeof(aBuf)), m_PlaceholderOffset+1));

			// add the name
			str_append(aBuf, pCompletionString, sizeof(aBuf));

			// add seperator
			const char *pSeparator = "";
			if(*(m_Input.GetString()+m_PlaceholderOffset+m_PlaceholderLength) != ' ')
				pSeparator = m_PlaceholderOffset == 0 ? ": " : " ";
			else if(m_PlaceholderOffset == 0)
				pSeparator = ":";
			if(*pSeparator)
				str_append(aBuf, pSeparator, sizeof(aBuf));

			// add part after the name
			str_append(aBuf, m_Input.GetString()+m_PlaceholderOffset+m_PlaceholderLength, sizeof(aBuf));

			m_PlaceholderLength = str_length(pSeparator)+str_length(pCompletionString);
			m_OldChatStringLength = m_Input.GetLength();
			m_Input.Set(aBuf); // TODO: Use Add instead
			m_Input.SetCursorOffset(m_PlaceholderOffset+m_PlaceholderLength);
			m_InputUpdate = true;
		}
	}
	else
	{
		// reset name completion process
		if(Event.m_Flags&IInput::FLAG_PRESS && Event.m_Key != KEY_TAB)
			if(Event.m_Key != KEY_LSHIFT)
				m_CompletionChosen = -1;

		m_OldChatStringLength = m_Input.GetLength();
		m_Input.ProcessInput(Event);
		m_InputUpdate = true;
	}
	if(Event.m_Flags&IInput::FLAG_PRESS && Event.m_Key == KEY_LSHIFT)
	{
		m_ReverseTAB = true;
	}
	else if(Event.m_Flags&IInput::FLAG_RELEASE && Event.m_Key == KEY_LSHIFT)
	{
		m_ReverseTAB = false;
	}
	if(Event.m_Flags&IInput::FLAG_PRESS && Event.m_Key == KEY_UP)
	{
		if(m_pHistoryEntry)
		{
			CHistoryEntry *pTest = m_History.Prev(m_pHistoryEntry);

			if(pTest)
				m_pHistoryEntry = pTest;
		}
		else
			m_pHistoryEntry = m_History.Last();

		if(m_pHistoryEntry)
			m_Input.Set(m_pHistoryEntry->m_aText);
	}
	else if (Event.m_Flags&IInput::FLAG_PRESS && Event.m_Key == KEY_DOWN)
	{
		if(m_pHistoryEntry)
			m_pHistoryEntry = m_History.Next(m_pHistoryEntry);

		if (m_pHistoryEntry)
			m_Input.Set(m_pHistoryEntry->m_aText);
		else
			m_Input.Clear();
	}

	return true;
}


void CChat::EnableMode(int Team)
{
	if(Client()->State() == IClient::STATE_DEMOPLAYBACK)
		return;

	if(m_Mode == MODE_NONE)
	{
		if(Team == 3)
			m_Mode = MODE_CRYPT;
		else if(Team == 2)
			m_Mode = MODE_HIDDEN;
		else if(Team == 1)
			m_Mode = MODE_TEAM;
		else
			m_Mode = MODE_ALL;

		Input()->Clear();
		m_CompletionChosen = -1;
		UI()->AndroidShowTextInput("", Team ? Localize("Team chat") : Localize("Chat"));
	}
}

void CChat::OnMessage(int MsgType, void *pRawMsg)
{
	static const char *apNotificationMsgs[] = {
			"You are now in a solo part.",
			"You are now out of the solo part.",
			"Rescue is not enabled on this server",
			"You aren't freezed!",
			"You are not freezed!",
			"Please join a team before you start",
			"Server admin requires you to be in a team and with other tees to start",
			"You have a jetpack gun",
			"You lost your jetpack gun",
			"You can't hook others",
			"You can hook others",
			"You can jump",
			"You have unlimited air jumps",
			"You don't have unlimited air jumps",
			"You can collide with others",
			"You can't collide with others",
			"Endless hook has been activated",
			"Endless hook has been deactivated",
			"You can hit others",
			"You can't hit others",
			"You can hammer hit others",
			"You can't hammer hit others",
			"You can shoot others with shotgun",
			"You can't shoot others with shotgun",
			"You can shoot others with grenade",
			"You can't shoot others with grenade",
			"You can shoot others with rifle",
			"You can't shoot others with rifle",
	};

	if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
		bool HideChat = false;

		// EVENT CALL
		{
			for(int ijdfg = 0; ijdfg < Client()->Lua()->GetLuaFiles().size(); ijdfg++)
			{
				if(Client()->Lua()->GetLuaFiles()[ijdfg]->State() != CLuaFile::LUAFILE_STATE_LOADED)
					continue;
				LuaRef lfunc = Client()->Lua()->GetLuaFiles()[ijdfg]->GetFunc("OnChat");
				if(lfunc) try { HideChat |= lfunc(pMsg->m_ClientID, pMsg->m_Team, std::string(pMsg->m_pMessage)).cast<bool>(); } catch(std::exception &e) { Client()->Lua()->HandleException(e, Client()->Lua()->GetLuaFiles()[ijdfg]); }
			}
			LuaRef confunc = getGlobal(CGameConsole::m_pStatLuaConsole->m_LuaHandler.m_pLuaState, "OnChat");
			if(confunc) try { confunc(pMsg->m_ClientID, pMsg->m_Team, std::string(pMsg->m_pMessage)); } catch(std::exception &e) { printf("LUA EXCEPTION: %s\n", e.what()); }
		}


		if(g_Config.m_ClNotifications)
		{
			for(int i = 0; i < 28; i++) // don't forget to increment 28 if you add more messages!
			{
				if(str_comp_nocase(pMsg->m_pMessage, apNotificationMsgs[i]) == 0)
				{
					m_pClient->m_pHud->PushNotification(pMsg->m_pMessage);
					HideChat = true;
				}
			}
		}

		if(g_Config.m_ClChatDennisProtection && m_LastDennisTrigger + time_freq() * 30 < time_get() &&
			(str_find_nocase(pMsg->m_pMessage, "'Dennis' entered and joined the game") ||
			str_find_nocase(pMsg->m_pMessage, "'deen' entered and joined the game") ||
			str_find_nocase(pMsg->m_pMessage, "'Dune' entered and joined the game")))
		{
			Say(0, "DENNIS!");
			m_LastDennisTrigger = time_get();
		}

		NETADDR Addr;
		if(net_addr_from_str(&Addr, pMsg->m_pMessage) == 0)
		{
			// such dennis
			if(g_Config.m_ClChatDennisProtection && m_LastDennisTrigger + time_freq() * 30 < time_get() && 
				Addr.port != 1337)
			{
				Say(0, "DENNIS!");
				m_LastDennisTrigger = time_get();
			}

			m_pClient->m_aClients[pMsg->m_ClientID].m_Spoofable = true; // presume the player is spoofable as he gave us his IP

			// thx 4 u ip nab ICKSDEHHHH
			str_copy(m_pClient->m_aClients[pMsg->m_ClientID].m_Addr, pMsg->m_pMessage, sizeof(m_pClient->m_aClients[pMsg->m_ClientID].m_Addr));
			if(!g_Config.m_ClChatShowIPs)
				HideChat = true;
		}

		if(!HideChat)
			AddLine(pMsg->m_ClientID, pMsg->m_Team, pMsg->m_pMessage);

		// try to decrypt everything we can
		if(pMsg->m_ClientID != -1)
		{
			char *pDecrypted = DecryptMsg(pMsg->m_pMessage);
			if(pDecrypted)
			{
				AddLine(pMsg->m_ClientID, 0, pDecrypted, true);
				delete[] pDecrypted;
			}
		}

		if(g_Config.m_ClTransIn &&
			str_length(pMsg->m_pMessage) > 4 &&
			pMsg->m_ClientID != m_pClient->m_Snap.m_LocalClientID &&
			pMsg->m_ClientID != -1)
			m_pTranslator->RequestTranslation(g_Config.m_ClTransInSrc, g_Config.m_ClTransInDst, pMsg->m_pMessage, true);
	}
}

bool CChat::LineShouldHighlight(const char *pLine, const char *pName)
{
	const char *pHL = str_find_nocase(pLine, pName);

	if (pHL)
	{
		int Length = str_length(pName);

		if((pLine == pHL || pHL[-1] == ' ') && (pHL[Length] == 0 || pHL[Length] == ' ' || pHL[Length] == '.' || pHL[Length] == '!' || pHL[Length] == ',' || pHL[Length] == '?' || pHL[Length] == ':'))
			return true;

	}

	return false;
}

void CChat::AddLine(int ClientID, int Team, const char *pLine, bool Hidden)
{
	if(*pLine == 0 || (ClientID != -1 && ClientID != -1337 && (m_pClient->m_aClients[ClientID].m_aName[0] == '\0' || // unknown client
		m_pClient->m_aClients[ClientID].m_ChatIgnore ||
		(m_pClient->m_Snap.m_LocalClientID != ClientID && g_Config.m_ClShowChatFriends && !m_pClient->m_aClients[ClientID].m_Friend) ||
		(m_pClient->m_Snap.m_LocalClientID != ClientID && m_pClient->m_aClients[ClientID].m_Foe))))
		return;

	// trim right and set maximum length to 256 utf8-characters
	int Length = 0;
	const char *pStr = pLine;
	const char *pEnd = 0;
	while(*pStr)
	{
		const char *pStrOld = pStr;
		int Code = str_utf8_decode(&pStr);

		// check if unicode is not empty
		if(str_utf8_isspace(Code))
		{
			pEnd = 0;
		}
		else if(pEnd == 0)
			pEnd = pStrOld;

		if(++Length >= 256)
		{
			*(const_cast<char *>(pStr)) = 0;
			break;
		}
	}
	if(pEnd != 0)
		*(const_cast<char *>(pEnd)) = 0;

	bool Highlighted = false;
	char *p = const_cast<char*>(pLine);
	while(*p)
	{
		Highlighted = false;
		pLine = p;
		// find line seperator and strip multiline
		while(*p)
		{
			if(*p++ == '\n')
			{
				*(p-1) = 0;
				break;
			}
		}

		m_CurrentLine = (m_CurrentLine+1)%MAX_LINES;
		m_aLines[m_CurrentLine].m_Time = time_get();
		m_aLines[m_CurrentLine].m_YOffset[0] = -1.0f;
		m_aLines[m_CurrentLine].m_YOffset[1] = -1.0f;
		m_aLines[m_CurrentLine].m_ClientID = ClientID;
		m_aLines[m_CurrentLine].m_Team = Team;
		m_aLines[m_CurrentLine].m_NameColor = -2;

		// check for highlighted name
		if (Client()->State() != IClient::STATE_DEMOPLAYBACK)
		{
			if(ClientID != m_pClient->Client()->m_LocalIDs[0])
			{
				// main character
				if (LineShouldHighlight(pLine, m_pClient->m_aClients[m_pClient->Client()->m_LocalIDs[0]].m_aName))
					Highlighted = true;
				// dummy
				if(m_pClient->Client()->DummyConnected() && LineShouldHighlight(pLine, m_pClient->m_aClients[m_pClient->Client()->m_LocalIDs[1]].m_aName))
					Highlighted = true;
			}
		}
		else
		{
			// on demo playback use local id from snap directly,
			// since m_LocalIDs isn't valid there
			if (LineShouldHighlight(pLine, m_pClient->m_aClients[m_pClient->m_Snap.m_LocalClientID].m_aName))
				Highlighted = true;
		}

		m_aLines[m_CurrentLine].m_Hidden = false;
		if(!Hidden)
			m_aLines[m_CurrentLine].m_Highlighted = Highlighted;
		else
			m_aLines[m_CurrentLine].m_Hidden = true;

		if(ClientID == -1) // server message
		{
			str_copy(m_aLines[m_CurrentLine].m_aName, "*** ", sizeof(m_aLines[m_CurrentLine].m_aName));
			str_format(m_aLines[m_CurrentLine].m_aText, sizeof(m_aLines[m_CurrentLine].m_aText), "%s", pLine);
		}
		else if(ClientID == -1337)
		{
			str_copy(m_aLines[m_CurrentLine].m_aName, "[*Translator*]: ", sizeof(m_aLines[m_CurrentLine].m_aName));
			str_format(m_aLines[m_CurrentLine].m_aText, sizeof(m_aLines[m_CurrentLine].m_aText), "%s", pLine);
		}
		else
		{
			if(m_pClient->m_aClients[ClientID].m_Team == TEAM_SPECTATORS)
				m_aLines[m_CurrentLine].m_NameColor = TEAM_SPECTATORS;

			if(m_pClient->m_Snap.m_pGameInfoObj && m_pClient->m_Snap.m_pGameInfoObj->m_GameFlags&GAMEFLAG_TEAMS)
			{
				if(m_pClient->m_aClients[ClientID].m_Team == TEAM_RED)
					m_aLines[m_CurrentLine].m_NameColor = TEAM_RED;
				else if(m_pClient->m_aClients[ClientID].m_Team == TEAM_BLUE)
					m_aLines[m_CurrentLine].m_NameColor = TEAM_BLUE;
			}

			if (Team == 2) // whisper send
			{
				str_format(m_aLines[m_CurrentLine].m_aName, sizeof(m_aLines[m_CurrentLine].m_aName), "→ %s", m_pClient->m_aClients[ClientID].m_aName);
				m_aLines[m_CurrentLine].m_NameColor = TEAM_BLUE;
				m_aLines[m_CurrentLine].m_Highlighted = false;
				m_aLines[m_CurrentLine].m_Team = 0;
				Highlighted = false;
			}
			else if (Team == 3) // whisper recv
			{
				str_format(m_aLines[m_CurrentLine].m_aName, sizeof(m_aLines[m_CurrentLine].m_aName), "← %s", m_pClient->m_aClients[ClientID].m_aName);
				m_aLines[m_CurrentLine].m_NameColor = TEAM_RED;
				m_aLines[m_CurrentLine].m_Highlighted = true;
				m_aLines[m_CurrentLine].m_Team = 0;
				Highlighted = true;
			}
			else
			{
				str_copy(m_aLines[m_CurrentLine].m_aName, m_pClient->m_aClients[ClientID].m_aName, sizeof(m_aLines[m_CurrentLine].m_aName));
			}

			str_format(m_aLines[m_CurrentLine].m_aText, sizeof(m_aLines[m_CurrentLine].m_aText), ": %s", pLine);
		}

		char aBuf[1024];
		str_format(aBuf, sizeof(aBuf), "%s%s", m_aLines[m_CurrentLine].m_aName, m_aLines[m_CurrentLine].m_aText);
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, ClientID == -1?"serv":Team >= 2?"whisper":(m_aLines[m_CurrentLine].m_Team?"teamchat":"chat"), aBuf, Highlighted);
	}

	// play sound
	int64 Now = time_get();
	if(ClientID == -1)
	{
		if(Now-m_aLastSoundPlayed[CHAT_SERVER] >= time_freq()*3/10)
		{
			if(g_Config.m_SndServerMessage)
			{
				m_pClient->m_pSounds->Play(CSounds::CHN_GUI, SOUND_CHAT_SERVER, 0);
				m_aLastSoundPlayed[CHAT_SERVER] = Now;
			}
		}
	}
	else if(Highlighted)
	{
		if(Now-m_aLastSoundPlayed[CHAT_HIGHLIGHT] >= time_freq()*3/10)
		{
#ifdef CONF_PLATFORM_MACOSX
			char aBuf[1024];
			str_format(aBuf, sizeof(aBuf), "%s%s", m_aLines[m_CurrentLine].m_aName, m_aLines[m_CurrentLine].m_aText);
			CNotification::notify("AllTheHaxx-Chat", aBuf);
#else
			Graphics()->NotifyWindow();
#endif
			if(g_Config.m_SndHighlight)
			{
				m_pClient->m_pSounds->Play(CSounds::CHN_GUI, SOUND_CHAT_HIGHLIGHT, 0);
				m_aLastSoundPlayed[CHAT_HIGHLIGHT] = Now;
			}
		}
	}
	else if(Team != 2)
	{
		if(Now-m_aLastSoundPlayed[CHAT_CLIENT] >= time_freq()*3/10)
		{
			if ((g_Config.m_SndTeamChat || !m_aLines[m_CurrentLine].m_Team)
				&& (g_Config.m_SndChat || m_aLines[m_CurrentLine].m_Team))
			{
				m_pClient->m_pSounds->Play(CSounds::CHN_GUI, SOUND_CHAT_CLIENT, 0);
				m_aLastSoundPlayed[CHAT_CLIENT] = Now;
			}
		}
	}
}

void CChat::OnRender()
{
	if(m_pTranslator->GetTranslation())
	{
		char aBuf[512];
		if(m_pTranslator->GetTranslation()->m_In)
		{
			str_format(aBuf, sizeof(aBuf), "'%s' (%s → %s)", m_pTranslator->GetTranslation()->m_Text, m_pTranslator->GetTranslation()->m_SrcLang, m_pTranslator->GetTranslation()->m_DstLang);
			AddLine(-1337, 0, aBuf);
		}
		else
			Say(0, m_pTranslator->GetTranslation()->m_Text, true);

		m_pTranslator->RemoveTranslation();
	}

	if (!g_Config.m_ClShowChat)
		return;

	// send pending chat messages
	if(m_PendingChatCounter > 0 && m_LastChatSend+time_freq() < time_get())
	{
		CHistoryEntry *pEntry = m_History.Last();
		for(int i = m_PendingChatCounter-1; pEntry; --i, pEntry = m_History.Prev(pEntry))
		{
			if(i == 0)
			{
				Say(pEntry->m_Team, pEntry->m_aText);
				break;
			}
		}
		--m_PendingChatCounter;
	}

	float Width = 300.0f*Graphics()->ScreenAspect();
	Graphics()->MapScreen(0.0f, 0.0f, Width, 300.0f);
	float x = 5.0f;
	float y = 300.0f-20.0f;
	if(m_Mode != MODE_NONE)
	{
		// render chat input
		CTextCursor Cursor;
		TextRender()->SetCursor(&Cursor, x, y, 8.0f, TEXTFLAG_RENDER);
		Cursor.m_LineWidth = m_pClient->m_pScoreboard->Active() ? 90.0f : 200.0f;
		Cursor.m_MaxLines = 2;

		if(m_Mode == MODE_ALL)
			TextRender()->TextEx(&Cursor, Localize("All"), -1);
		else if(m_Mode == MODE_TEAM)
			TextRender()->TextEx(&Cursor, Localize("Team"), -1);
		else if(m_Mode == MODE_HIDDEN)
			TextRender()->TextEx(&Cursor, Localize("Hidden"), -1);
		else if(m_Mode == MODE_CRYPT)
			TextRender()->TextEx(&Cursor, Localize("Crypt"), -1);
		else
			TextRender()->TextEx(&Cursor, Localize("Chat"), -1);

		TextRender()->TextEx(&Cursor, ": ", -1);

		// check if the visible text has to be moved
		if(m_InputUpdate)
		{
			if(m_ChatStringOffset > 0 && m_Input.GetLength() < m_OldChatStringLength)
				m_ChatStringOffset = max(0, m_ChatStringOffset-(m_OldChatStringLength-m_Input.GetLength()));

			if(m_ChatStringOffset > m_Input.GetCursorOffset())
				m_ChatStringOffset -= m_ChatStringOffset-m_Input.GetCursorOffset();
			else
			{
				CTextCursor Temp = Cursor;
				Temp.m_Flags = 0;
				TextRender()->TextEx(&Temp, m_Input.GetString()+m_ChatStringOffset, m_Input.GetCursorOffset()-m_ChatStringOffset);
				TextRender()->TextEx(&Temp, "|", -1);
				while(Temp.m_LineCount > 1)
				{
					++m_ChatStringOffset;
					Temp = Cursor;
					Temp.m_Flags = 0;
					TextRender()->TextEx(&Temp, m_Input.GetString()+m_ChatStringOffset, m_Input.GetCursorOffset()-m_ChatStringOffset);
					TextRender()->TextEx(&Temp, "|", -1);
				}
			}
			m_InputUpdate = false;
		}

		TextRender()->TextEx(&Cursor, m_Input.GetString()+m_ChatStringOffset, m_Input.GetCursorOffset()-m_ChatStringOffset);
		static float MarkerOffset = TextRender()->TextWidth(0, 8.0f, "|", -1)/3;
		CTextCursor Marker = Cursor;
		Marker.m_X -= MarkerOffset;
		TextRender()->TextEx(&Marker, "|", -1);
		TextRender()->TextEx(&Cursor, m_Input.GetString()+m_Input.GetCursorOffset(), -1);
	}

	y -= 8.0f;
#if defined(__ANDROID__)
	x += 120.0f;
#endif

	int64 Now = time_get();
	float LineWidth = m_pClient->m_pScoreboard->Active() ? 90.0f : 200.0f;
	float HeightLimit = m_pClient->m_pScoreboard->Active() ? 230.0f : m_Show ? 50.0f : 200.0f;
	float Begin = x;
#if defined(__ANDROID__)
	float FontSize = 10.0f;
#else
	float FontSize = 6.0f;
#endif
	CTextCursor Cursor;
	int OffsetType = m_pClient->m_pScoreboard->Active() ? 1 : 0;
	for(int i = 0; i < MAX_LINES; i++)
	{
		int r = ((m_CurrentLine-i)+MAX_LINES)%MAX_LINES;
		if(Now > m_aLines[r].m_Time+16*time_freq() && !m_Show)
			break;

		char aName[64] = "";
		if(g_Config.m_ClShowIDsChat && m_aLines[r].m_ClientID != -1 && m_aLines[r].m_aName[0] != '\0')
		{
			if (m_aLines[r].m_ClientID >= 10)
				str_format(aName, sizeof(aName),"%d: ", m_aLines[r].m_ClientID);
			else
				str_format(aName, sizeof(aName)," %d: ", m_aLines[r].m_ClientID);
			str_append(aName, m_aLines[r].m_aName,sizeof(aName));
		}
		else
		{
			str_copy(aName, m_aLines[r].m_aName, sizeof(aName));
		}

		// get the y offset (calculate it if we haven't done that yet)
		if(m_aLines[r].m_YOffset[OffsetType] < 0.0f)
		{
			TextRender()->SetCursor(&Cursor, Begin, 0.0f, FontSize, 0);
			Cursor.m_LineWidth = LineWidth;
			TextRender()->TextEx(&Cursor, aName, -1);
			TextRender()->TextEx(&Cursor, m_aLines[r].m_aText, -1);
			m_aLines[r].m_YOffset[OffsetType] = Cursor.m_Y + Cursor.m_FontSize;
		}
		y -= m_aLines[r].m_YOffset[OffsetType];

		// cut off if msgs waste too much space
		if(y < HeightLimit)
			break;

		float Blend = Now > m_aLines[r].m_Time+14*time_freq() && !m_Show ? 1.0f-(Now-m_aLines[r].m_Time-14*time_freq())/(2.0f*time_freq()) : 1.0f;

		// reset the cursor
		TextRender()->SetCursor(&Cursor, Begin + (g_Config.m_ClChatAvatar ? 3.0f : 0.0f), y, FontSize, TEXTFLAG_RENDER);
		Cursor.m_LineWidth = LineWidth;

		// render name
		if (m_aLines[r].m_ClientID == -1)
		{
			//TextRender()->TextColor(1.0f, 1.0f, 0.5f, Blend); // system
			vec3 rgb = HslToRgb(vec3(g_Config.m_ClMessageSystemHue / 255.0f, g_Config.m_ClMessageSystemSat / 255.0f, g_Config.m_ClMessageSystemLht / 255.0f));
			TextRender()->TextColor(rgb.r, rgb.g, rgb.b, Blend);
		}
		else if (m_aLines[r].m_Team)
			TextRender()->TextColor(0.45f, 0.9f, 0.45f, Blend); // team message
		else if(m_aLines[r].m_NameColor == TEAM_RED)
			TextRender()->TextColor(1.0f, 0.5f, 0.5f, Blend); // red
		else if(m_aLines[r].m_NameColor == TEAM_BLUE)
			TextRender()->TextColor(0.7f, 0.7f, 1.0f, Blend); // blue
		else if(m_aLines[r].m_NameColor == TEAM_SPECTATORS)
			TextRender()->TextColor(0.75f, 0.5f, 0.75f, Blend); // spectator
		else if(m_aLines[r].m_ClientID >= 0 && g_Config.m_ClChatTeamColors && m_pClient->m_Teams.Team(m_aLines[r].m_ClientID))
		{
			vec3 rgb = HslToRgb(vec3(m_pClient->m_Teams.Team(m_aLines[r].m_ClientID) / 64.0f, 1.0f, 0.75f));
			TextRender()->TextColor(rgb.r, rgb.g, rgb.b, Blend);
		}
		else
			TextRender()->TextColor(0.8f, 0.8f, 0.8f, Blend);

		// friends always in green // TODO: settings pls!
		if(m_aLines[r].m_ClientID > 0 && m_aLines[r].m_ClientID < MAX_CLIENTS && g_Config.m_ClColorfulClient && m_pClient->Friends()->IsFriend(m_pClient->m_aClients[m_aLines[r].m_ClientID].m_aName, m_pClient->m_aClients[m_aLines[r].m_ClientID].m_aClan, true))
			TextRender()->TextColor(0,0.7f,0,Blend);
		else if (m_aLines[r].m_ClientID == -1337) // translator in blue
			TextRender()->TextColor(0.2f,0.2f,0.7f,Blend);
		TextRender()->TextEx(&Cursor, aName, -1);

		// render line
		vec3 rgb;
		if (m_aLines[r].m_ClientID == -1)
			rgb = HslToRgb(vec3(g_Config.m_ClMessageSystemHue / 255.0f, g_Config.m_ClMessageSystemSat / 255.0f, g_Config.m_ClMessageSystemLht / 255.0f));
		else if (m_aLines[r].m_ClientID == -1337)
			rgb = vec3(0.45f, 0.45f, 1.0f); //TextRender()->TextColor(0.45f,0.45f,1.0f, Blend);
		else if (m_aLines[r].m_Highlighted)
			rgb = HslToRgb(vec3(g_Config.m_ClMessageHighlightHue / 255.0f, g_Config.m_ClMessageHighlightSat / 255.0f, g_Config.m_ClMessageHighlightLht / 255.0f));
		else if (m_aLines[r].m_Team)
			rgb = HslToRgb(vec3(g_Config.m_ClMessageTeamHue / 255.0f, g_Config.m_ClMessageTeamSat / 255.0f, g_Config.m_ClMessageTeamLht / 255.0f));
		else if(m_aLines[r].m_Hidden)
			rgb = vec3(1.0f, 0.7f, 0.0f);
		else
			rgb = HslToRgb(vec3(g_Config.m_ClMessageHue / 255.0f, g_Config.m_ClMessageSat / 255.0f, g_Config.m_ClMessageLht / 255.0f));

        if(g_Config.m_ClChatAvatar && m_aLines[r].m_ClientID != -1)
        {
            CGameClient::CClientData *pClientData = &m_pClient->m_aClients[m_aLines[r].m_ClientID];
            CTeeRenderInfo RenderInfo = pClientData->m_RenderInfo;
            RenderInfo.m_Size = 8.0f;
            RenderInfo.m_ColorBody.a = RenderInfo.m_ColorFeet.a = Blend;

            RenderTools()->RenderTee(CAnimState::GetIdle(), &RenderInfo, 0, vec2(-1.0f, 0.0f), vec2(Begin, y+FontSize-1.5f));
        }

		TextRender()->TextColor(rgb.r, rgb.g, rgb.b, Blend);
		TextRender()->TextEx(&Cursor, m_aLines[r].m_aText, -1);
	}

	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 1.0f);

#if defined(__ANDROID__)
	static int deferEvent = 0;
	if( UI()->AndroidTextInputShown() )
	{
		if(m_Mode == MODE_NONE)
		{
			deferEvent++;
			if( deferEvent > 2 )
				EnableMode(0);
		}
		else
			deferEvent = 0;
	}
	else
	{
		if(m_Mode != MODE_NONE)
		{
			deferEvent++;
			if( deferEvent > 2 )
			{
				IInput::CEvent Event;
				Event.m_Flags = IInput::FLAG_PRESS;
				Event.m_Key = KEY_RETURN;
				OnInput(Event);
			}
		}
		else
			deferEvent = 0;
	}
#endif
}

bool CChat::HandleTCommands(const char *pMsg)
{
	if(g_Config.m_ClTransChatCmds && pMsg[0] == '$')
	{
		char aCmd[512][256] = {{0}};
		mem_zero(&aCmd, sizeof(aCmd));
		int Cmd = 0;
		int Char = 0;

		for(int i = 1; i < str_length(pMsg); i++)
		{
			if(pMsg[i] == ' ')
			{
				Cmd++;
				Char = 0;
				continue;
			}

			aCmd[Cmd][Char] = pMsg[i];
			Char++;
		}

		if(!str_comp_nocase(aCmd[0], "cmdlist"))
		{
			AddLine(-1337, 0, "~~~~ Commands ~~~~");
			AddLine(-1337, 0, "'$tout <dst> <message>': Translate a message");
			AddLine(-1337, 0, "'$tin <src> <ID>': Translate message in the chat ($tin 0 to translate the last message)");
			return true;
		}
		else if(!str_comp_nocase(aCmd[0], "tout"))
		{
			if(!aCmd[1][0] || !aCmd[2][0])
			{
				AddLine(-1337, 0, "Please use '$tout <dst> <message>'");
				return true;
			}

			int i = 2;
			char aBuf[256];
			char RawMsg[512];

			mem_zero(&RawMsg, sizeof(RawMsg));

			while(aCmd[i][0])
			{
				str_format(aBuf, sizeof(aBuf), "%s ", aCmd[i]);
				str_append(RawMsg, aBuf, sizeof(RawMsg));
				i++;
			}

			m_pTranslator->RequestTranslation(g_Config.m_ClTransOutSrc, aCmd[1], RawMsg, false);
			return true;
		}
		else if(!str_comp_nocase(aCmd[0], "tin"))
		{
			if(!aCmd[1][0] || !aCmd[2][0])
			{
				AddLine(-1337, 0, "Please use '$tin <src> <ID>'");
				return true;
			}

			int MsgID = str_toint(aCmd[2]);

			m_pTranslator->RequestTranslation(aCmd[1], g_Config.m_ClTransInDst, m_aLines[m_CurrentLine-MsgID].m_aText, true);
			return true;
		}

		AddLine(-1337, 0, "Unknown command. Try '$cmdlist'!");
		return true;
	}

	return false;
}

void CChat::Say(int Team, const char *pLine, bool NoTrans)
{
	m_LastChatSend = time_get();

	char aMessage[1024];
	str_copy(aMessage, pLine, sizeof(aMessage));

	if(HandleTCommands(pLine))
		return;

	if(g_Config.m_ClTransOut && str_length(aMessage) > 4 && aMessage[0] != '/' && !NoTrans)
	{
		m_pTranslator->RequestTranslation(g_Config.m_ClTransOutSrc, g_Config.m_ClTransOutDst, aMessage, false);
		return;
	}

	//LUA_FIRE_EVENT("OnChatSend", Team, pLine);
	{
		bool DiscardChat = false;
		for(int ijdfg = 0; ijdfg < Client()->Lua()->GetLuaFiles().size(); ijdfg++)
		{
			if(Client()->Lua()->GetLuaFiles()[ijdfg]->State() != CLuaFile::LUAFILE_STATE_LOADED)
				continue;
			LuaRef lfunc = Client()->Lua()->GetLuaFiles()[ijdfg]->GetFunc("OnChatSend");
			if(lfunc) try { if(lfunc(Team, pLine)) DiscardChat = true; } catch(std::exception &e) { printf("LUA EXCEPTION: %s\n", e.what()); }
		}
		LuaRef confunc = getGlobal(CGameConsole::m_pStatLuaConsole->m_LuaHandler.m_pLuaState, "OnChatSend");
		if(confunc) try { if(confunc(Team, pLine)) DiscardChat = true; } catch(std::exception &e) { printf("LUA EXCEPTION: %s\n", e.what()); }

		if(DiscardChat)
			return;
	}


	// send chat message
	CNetMsg_Cl_Say Msg;
	Msg.m_Team = Team;
	Msg.m_pMessage = aMessage;
	Client()->SendPackMsg(&Msg, MSGFLAG_VITAL);
}

////////////////////////////
// chat crypt stuff below //
////////////////////////////

void CChat::GenerateKeyPair(int Bytes, int Exp) // let's dont go ham and do like 512 bytes and Exp = 3
{
	m_pKeyPair = RSA_generate_key(Bytes, Exp, NULL, NULL);
	m_GotKeys = true;
}

char *CChat::ReadPubKey(RSA *pKeyPair)
{
	BIO *pBio = BIO_new(BIO_s_mem());
	PEM_write_bio_RSAPublicKey(pBio, pKeyPair);

	char *PEMKey = new char[512];
	int KeyLen = BIO_pending(pBio);
	BIO_read(pBio, PEMKey, KeyLen);
	
	return PEMKey;
}

char *CChat::ReadPrivKey(RSA *pKeyPair)
{
	BIO *pBio = BIO_new(BIO_s_mem());
	PEM_write_bio_RSAPrivateKey(pBio, pKeyPair, NULL, NULL, 0, NULL, NULL);

	char *PEMKey = new char[512];
	int KeyLen = BIO_pending(pBio);
	BIO_read(pBio, PEMKey, KeyLen);
	
	return PEMKey;
}

char *CChat::EncryptMsg(const char *pMsg)
{
	if(!m_GotKeys)
	{
		m_pClient->m_pHud->PushNotification("Generate or load keys first!");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "crypto", "generate or load keys first");
		return 0;
	}

	char *pHex = new char[512];

	unsigned char aEncrypted[512] = {};
	if(RSA_public_encrypt(str_length(pMsg)+1, (unsigned char*)pMsg, (unsigned char*)aEncrypted, m_pKeyPair, RSA_PKCS1_PADDING) == -1)
	{
		dbg_msg("chatcrypt", "failed to public encrypt message");
	}

	mem_zero(pHex, sizeof(pHex));
	for(int i = 0; aEncrypted[i]; i++)
	{
		char aBuf[3];
		str_format(aBuf, sizeof(aBuf), "%02x", aEncrypted[i]);
		str_append(pHex, aBuf, 512);
	}

	return pHex;
}

char *CChat::DecryptMsg(const char *pMsg)
{
	if(!m_GotKeys)
		return 0;

	char *pHex = new char[512];
	char *pClear = new char[512];

	unsigned char aEncrypted[1024] = {0};
	unsigned char aDecrypted[1024] = {};
	for(int i = 0, j = 0; pMsg[j]; i++, j+=2)
	{
		char aBuf[3];
		str_copy(aBuf, &pMsg[j], sizeof(aBuf));
		aEncrypted[i] = strtol(aBuf, 0, 16);
	}

	for(int i = 0; aEncrypted[i]; i++)
	{
		char aBuf[3];
		str_format(aBuf, sizeof(aBuf), "%02x", aEncrypted[i]);
		str_append(pHex, aBuf, 512);
	}

	if(RSA_private_decrypt(str_length((char*)aEncrypted), aEncrypted, aDecrypted, m_pKeyPair, RSA_PKCS1_PADDING) != -1)
	{
		str_copy(pClear, (char *)aDecrypted, 512);
		return pClear; 
	}
	else
		return 0;
}

void CChat::SaveKeys(RSA *pKeyPair, const char *pKeyName)
{
	if(!m_GotKeys)
	{
		m_pClient->m_pHud->PushNotification("No keys to save!");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "crypto", "no keys to save");
		return;
	}

	char aPubKey[256];
	char aPrivKey[256];
	str_format(aPubKey, sizeof(aPubKey), "rsa/%s_pub.key", pKeyName);
	str_format(aPrivKey, sizeof(aPrivKey), "rsa/%s_priv.key", pKeyName);

	FILE *pPubFile;
	pPubFile = fopen(aPubKey, "w");
	if(pPubFile != NULL)
	{
		char *pPupKey = ReadPubKey(pKeyPair);
		fputs(pPupKey, pPubFile);
		fclose(pPubFile);
		delete[] pPupKey;
	}
	else
	{
		m_pClient->m_pHud->PushNotification("Couldn't save public key!");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "crypto", "couldn't save public key");
	}

	FILE *pPrivFile;
	pPrivFile = fopen(aPrivKey, "w");
	if(pPrivFile != NULL)
	{
		char *pPrivKey = ReadPrivKey(pKeyPair);
		fputs(pPrivKey, pPrivFile);
		fclose(pPrivFile);
		delete[] pPrivKey;
	}
	else
	{
		m_pClient->m_pHud->PushNotification("Couldn't save private key!");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "crypto", "couldn't save private key");
	}
}

void CChat::LoadKeys(const char *pKeyName)
{
	m_pKeyPair = RSA_new();

	char aPubKey[256];
	char aPrivKey[256];
	str_format(aPubKey, sizeof(aPubKey), "rsa/%s_pub.key", pKeyName);
	str_format(aPrivKey, sizeof(aPrivKey), "rsa/%s_priv.key", pKeyName);

	FILE *pPubFile = fopen(aPubKey, "rb");
	if(pPubFile)
		PEM_read_RSAPublicKey(pPubFile, &m_pKeyPair, NULL, NULL);
	else
	{
		m_pClient->m_pHud->PushNotification("Couldn't load public key!");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "crypto", "couldn't load public key");
		m_GotKeys = false;
		return;
	}

	FILE *pPrivFile = fopen(aPrivKey, "rb");
	if(pPrivFile)
		PEM_read_RSAPrivateKey(pPrivFile, &m_pKeyPair, NULL, NULL);
	else
	{
		m_pClient->m_pHud->PushNotification("Couldn't load private key!");
		Console()->Print(IConsole::OUTPUT_LEVEL_STANDARD, "crypto", "couldn't load private key");
		m_GotKeys = false;
		return;
	}

	m_GotKeys = true;
}

/*  ++++ PADDINGS ++++
	RSA_PKCS1_PADDING - most commonly used
	RSA_PKCS1_OAEP_PADDING
	RSA_SSLV23_PADDING
	RSA_NO_PADDING - raw RSA crypto
*/

void CChat::SayChat(const char *pLine)
{
	if(!pLine || str_length(pLine) < 1)
		return;

	bool AddEntry = false;

	if(m_LastChatSend+time_freq() < time_get())
	{
		Say(m_Mode == MODE_ALL ? 0 : 1, pLine);
		AddEntry = true;
	}
	else if(m_PendingChatCounter < 3)
	{
		++m_PendingChatCounter;
		AddEntry = true;
	}

	if(AddEntry)
	{
		CHistoryEntry *pEntry = m_History.Allocate(sizeof(CHistoryEntry)+str_length(pLine)-1);
		pEntry->m_Team = m_Mode == MODE_ALL ? 0 : 1;
		mem_copy(pEntry->m_aText, pLine, str_length(pLine));
	}
}
