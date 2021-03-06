/* (c) unsigned char*. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at https://github.com/CytraL/HClient */
#include <base/math.h>
#include <base/system.h>
#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <game/version.h>
#include <game/client/components/menus.h>

#if defined(CONF_FAMILY_UNIX)
	#include <unistd.h>
#elif defined(CONF_FAMILY_WINDOWS)
	#include <windows.h>
#endif

#include <cstring>
#include <ctime>
#include <cstdio> // vsnprintf
#include <cstdarg>
#include <algorithm>

#include "irc.h"

static NETSOCKET invalid_socket = {NETTYPE_INVALID, -1, -1};

CIRC::CIRC()
{
	m_IRCComs.clear();
	m_Hooks.clear();
	m_pGraphics = 0x0;
	m_State = STATE_DISCONNECTED;
	m_Socket = invalid_socket;
	m_Nick = "";
	m_StartTime = 0;
	mem_zero(m_CmdToken, sizeof(m_CmdToken));
	SetActiveCom(-1);
}

void CIRC::RegisterCallback(const char* pMsgID, int (*func)(ReplyData*, void*, void*), void *pUser) // pData, pUser
{
	IRCHook h;
	h.messageID = pMsgID;
	h.function = func;
	h.user = pUser;
	m_Hooks.add(h);
	dbg_msg("engine/IRC", "registered callback for '%s'", pMsgID);
}

void CIRC::CallHooks(const char *pMsgID, ReplyData* pReplyData)
{
	for(int i = 0; i < m_Hooks.size(); i++)
	{
		if(m_Hooks[i].messageID == std::string(pMsgID))
		{
			int ret = (*(m_Hooks[i].function))(pReplyData, m_Hooks[i].user, this);
			if(ret != 0)
			{
				char aError[128];
				str_format(aError, sizeof(aError), "IRC callback returned != 0 (ret=%i, i=%i, callback=%s, func=%p)",
						ret, i, m_Hooks[i].messageID.c_str(), m_Hooks[i].function);
				dbg_assert(ret == 0, aError);
			}
		}
	}
}

void CIRC::Init()
{
	m_pGraphics = Kernel()->RequestInterface<IGraphics>();
	m_pGameClient = Kernel()->RequestInterface<IGameClient>();
	m_pClient = Kernel()->RequestInterface<IClient>();
}

void CIRC::SetActiveCom(int index)
{
	if (index < 0 || index >= (int)m_IRCComs.size())
		index = 0;

	m_ActiveCom = index;
	CIRCCom *pCom = GetCom(index);
	if (pCom)
		pCom->m_NumUnreadMsg = 0;
}

void CIRC::SetActiveCom(CIRCCom *pCom)
{
	if(!pCom)
		return;
	
	std::list<CIRCCom*>::iterator it = std::find(m_IRCComs.begin(), m_IRCComs.end(), pCom);
	if (it != m_IRCComs.end())
	{
		pCom->m_NumUnreadMsg = 0;
		m_ActiveCom = std::distance(m_IRCComs.begin(), it);
	}
}

CIRCCom* CIRC::GetActiveCom()
{
	if (m_ActiveCom < 0 || m_ActiveCom >= (int)m_IRCComs.size())
		return 0x0;

	std::list<CIRCCom*>::iterator it = m_IRCComs.begin();
	std::advance(it, m_ActiveCom);
	return (*it);
}

CIRCCom* CIRC::GetCom(size_t index)
{
	if (index < 0 || index >= m_IRCComs.size())
		return 0x0;

	std::list<CIRCCom*>::iterator it = m_IRCComs.begin();
	std::advance(it, index);
	return (*it);
}
CIRCCom* CIRC::GetCom(std::string name)
{
	std::list<CIRCCom*>::iterator it = m_IRCComs.begin();

	while (it != m_IRCComs.end())
	{
		if ((*it)->GetType() == CIRCCom::TYPE_CHANNEL)
		{
			CComChan *pChan = static_cast<CComChan*>((*it));
			if (str_comp_nocase(name.c_str(), pChan->m_Name) == 0)
				return (*it);
		}
		else if ((*it)->GetType() == CIRCCom::TYPE_QUERY)
		{
			CComQuery *pQuery = static_cast<CComQuery*>((*it));
			if (str_comp_nocase(name.c_str(), pQuery->m_Name) == 0)
				return (*it);
		}

		++it;
	}

	return 0x0;
}

bool CIRC::CanCloseCom(CIRCCom *pCom)
{
	if(!pCom)
		return false;

	if(GetNumComs() <= 2 || str_comp_nocase(((CComChan*)pCom)->m_Name, "#AllTheHaxx") == 0 ||
			str_comp_nocase(((CComQuery*)pCom)->m_Name, "@status") == 0)
		return false;

	return true;
}

void CIRC::StartConnection() // call this from a thread only!
{
	NETADDR BindAddr;
	mem_zero(&m_HostAddress, sizeof(m_HostAddress));
	mem_zero(&BindAddr, sizeof(BindAddr));
	char aNetBuff[2048];

	m_State = STATE_CONNECTING;
	// lookup
	int connectionType = NETTYPE_IPV6;
	//if(net_host_lookup("irc.ipv6.quakenet.org:6667", &m_HostAddress, connectionType) != 0)
	{
		connectionType = NETTYPE_IPV4;
		if(net_host_lookup("irc.quakenet.org:6667", &m_HostAddress, connectionType) != 0)
		{
			dbg_msg("IRC","ERROR: Can't lookup irc.quakenet.org, neither on IPv6 nor IPv4");
			m_State = STATE_DISCONNECTED;
			return;
		}
	}

	m_HostAddress.port = 6667;

	// connect
	BindAddr.type = connectionType;
	m_Socket = net_tcp_create(BindAddr);
	if(net_tcp_connect(m_Socket, &m_HostAddress) != 0)
	{
		net_tcp_close(m_Socket);
		char aBuf[64];
		net_addr_str(&m_HostAddress, aBuf, sizeof(aBuf), 0);
		dbg_msg("IRC","ERROR: Can't connect to '%s:%d'...", aBuf, m_HostAddress.port);
		m_State = STATE_DISCONNECTED;
		return;
	}

	if(str_length(g_Config.m_ClIRCNick) == 0 || str_comp(g_Config.m_ClIRCNick, "haxxless tee") == 0)
		str_copy(g_Config.m_ClIRCNick, g_Config.m_PlayerName, sizeof(g_Config.m_ClIRCNick));

	m_Nick = g_Config.m_ClIRCNick;

	// send request
	SendRaw("CAP LS");
	SendRaw("NICK %s", m_Nick.c_str());
	SendRaw("USER %s 0 * :%s", g_Config.m_ClIRCUser, g_Config.m_PlayerName);

	// status tab
	OpenCom<CComQuery>("@Status");
	SetActiveCom(-1);

	m_StartTime = time_get();
	m_State = STATE_CONNECTED;

	std::string NetData;
	//int TotalRecv = 0;
	//int TotalBytes = 0;
	int CurrentRecv = 0;
	char LastPong[255]={0};
	while ((CurrentRecv = net_tcp_recv(m_Socket, aNetBuff, sizeof(aNetBuff))) >= 0 && m_State == STATE_CONNECTED)
	{
		ReplyData reply;

		//dbg_msg(".", "%s", aNetBuff);// XXX: here
		for (int i=0; i < CurrentRecv; i++)
		{
			if (aNetBuff[i]=='\r' || aNetBuff[i]=='\t')
				 continue;

			if (aNetBuff[i]=='\n')
			{
				size_t del = NetData.find_first_of(":");
				size_t ldel = 0;
				if (del > 0)
				{ //NeT Message
					std::string aMsgID = NetData.substr(0, del-1);
					std::string aMsgText = NetData.substr(del+1, NetData.length()-del-1);
					if (aMsgID.compare("PING") == 0)
					{
						SendRaw("PONG %s :%s", LastPong, aMsgText.c_str());
						if(g_Config.m_Debug)
							dbg_msg("engine/IRC", "Ping? Pong!");
						LastPong[0]=0;
					}
					else if (aMsgID.compare("PONG") == 0)
						str_copy(LastPong, aMsgText.c_str(), sizeof(LastPong));
					else
					{
						CIRCCom *pCom = GetCom("@Status");
						pCom->m_Buffer.push_back(aMsgText);
						reply.channel = "@Status";
						reply.from = "quakenet.org";
						reply.params = aMsgText;
					}
				} else
				{ //raw message
					del = NetData.find_first_of(" ");
					std::string aMsgFServer = NetData.substr(1, del);
					ldel = del;
					del = NetData.find_first_of(" ",del+1);
					std::string aMsgID = NetData.substr(ldel+1, del-ldel-1);

					//dbg_msg("IRC", "Raw MSG [%s]: %s",aMsgID.c_str(), aMsgFServer.c_str());
					//dbg_msg("IRC-RAW", NetData.c_str());

					if (aMsgID.compare("001") == 0)
					{
						// set the user's wanted modes
						SetMode(g_Config.m_ClIRCModes, m_Nick.c_str());

						// send Q auth
						if(g_Config.m_ClIRCQAuthName[0] != '\0' && g_Config.m_ClIRCQAuthPass[0] != '\0')
							SendRaw("PRIVMSG Q@CServe.quakenet.org :AUTH %s %s", g_Config.m_ClIRCQAuthName, g_Config.m_ClIRCQAuthPass);

						// auto-join
						JoinTo("#AllTheHaxx");

						reply.channel = "@Status";
						reply.from = "quakenet.org";
					}
					else if (aMsgID.compare("332") == 0) // topic
					{
						del = NetData.find_first_of(" ",del+1);
						ldel = NetData.find_first_of(" ",del+1);
						std::string aMsgChan = NetData.substr(del+1,ldel-del-1);

						del = NetData.find_first_of(":", 1);
						std::string aMsgTopic = NetData.substr(del+1, NetData.length()-del-1);

						CComChan *pChan = static_cast<CComChan*>(GetCom(aMsgChan));
						if (pChan)
							pChan->m_Topic = aMsgTopic;

						reply.channel = aMsgChan;
						reply.params = aMsgTopic;
					}
					else if (aMsgID.compare("353") == 0) // NAMREPLY
					{
						del = NetData.find_first_of("=");
						ldel = NetData.find_first_of(" ",del+2);

						std::string aMsgChan = NetData.substr(del+2, ldel-del-2);
						del = NetData.find_first_of(":",1);
						std::string aMsgUsers = NetData.substr(del+1, NetData.length()-del-1);

						CComChan *pChan = static_cast<CComChan*>(GetCom(aMsgChan));
						if (pChan)
						{
							size_t del=0, ldel=0;
							do{
								del = aMsgUsers.find_first_of(" ",del+1);
								pChan->m_Users.push_back(aMsgUsers.substr(ldel, del-ldel));
								ldel=del+1;
							} while (del != std::string::npos);
						}
						reply.channel = aMsgChan;
						reply.params = aMsgUsers;
					}
					else if (aMsgID.compare("366") == 0) // ENDOFNAMES
					{
						del = NetData.find_first_of(" ",del+1);
						ldel = NetData.find_first_of(" ",del+1);
						std::string aMsgChan = NetData.substr(del+1,ldel-del-1);

						CComChan *pChan = static_cast<CComChan*>(GetCom(aMsgChan));
						if (pChan)
							pChan->m_Users.sort();

						reply.channel = aMsgChan;

					}
					else if (aMsgID.compare("401") == 0) // NOSUCHNICK
					{
						del = NetData.find_first_of(" ",del+1);
						ldel = NetData.find_first_of(" ",del+1);
						std::string aMsgFrom = NetData.substr(del+1,ldel-del-1);
						del = NetData.find_first_of(":",1);
						std::string aMsgText = NetData.substr(del+1, NetData.length()-del-1);

						CIRCCom *pCom = GetCom(aMsgFrom);
						if(!pCom)
							pCom = GetCom("@Status");

						pCom->AddMessage("*** '%s' %s", aMsgFrom.c_str(), aMsgText.c_str());

						reply.from = aMsgFrom;
						reply.params = aMsgText;

					}
					else if (aMsgID.compare("421") == 0) // UNKNOWNCOMMAND
					{
						del = NetData.find_first_of(" ",del+1);
						ldel = NetData.find_first_of(" ",del+1);
						std::string aMsgCmd = NetData.substr(del+1,ldel-del-1);
						del = NetData.find_first_of(":",1);
						std::string aMsgText = NetData.substr(del+1, NetData.length()-del-1);

						CIRCCom *pCom = GetCom("@Status");
						pCom->AddMessage("'%s' %s", aMsgCmd.c_str(), aMsgText.c_str());

						reply.from = aMsgCmd;
						reply.params = aMsgText;

					}
					else if (aMsgID.compare("JOIN") == 0)
					{
						std::string aMsgChannel = NetData.substr(del+1, NetData.length()-del-1);
						del = aMsgFServer.find_first_of("!");
						std::string aMsgFrom = aMsgFServer.substr(0,del);

						if (aMsgFrom == m_Nick) // we (were) joined a channel
						{
							OpenCom<CComChan>(aMsgChannel.c_str());
						}
						else if(aMsgFrom != "circleci-bot") // ignore the ci bot
						{
							CComChan *pChan = static_cast<CComChan*>(GetCom(aMsgChannel));
							if(pChan)
							{
								pChan->m_Users.push_back(aMsgFrom);
								pChan->m_Users.sort();
								pChan->AddMessage("*** '%s' has joined %s", aMsgFrom.c_str(), aMsgChannel.c_str());
							}
						}

						reply.channel = aMsgChannel;
						reply.from = aMsgFrom;
					}
					else if (aMsgID.compare("PART") == 0)
					{
						std::string aMsgChannel = NetData.substr(del+1, NetData.length()-del-1);
						del = aMsgFServer.find_first_of("!");
						std::string aMsgFrom = aMsgFServer.substr(0,del);
						char aBuff[255];

						if (aMsgFrom == m_Nick) // we (were) left a channel
						{
							CIRCCom *pCom = GetCom(aMsgChannel);
							if (pCom)
							{
								m_IRCComs.remove(pCom);
								mem_free(pCom);
								pCom=0x0;
								SetActiveCom(m_IRCComs.size()-1);
							}
						}
						else if(aMsgFrom != "circleci-bot") // ignore the ci bot
						{
							CComChan *pChan = static_cast<CComChan*>(GetCom(aMsgChannel));
							if (pChan)
							{
								pChan->m_Users.remove(aMsgFrom);
								str_format(aBuff, sizeof(aBuff), "@%s", aMsgFrom.c_str());
								pChan->m_Users.remove(std::string(aBuff));
								str_format(aBuff, sizeof(aBuff), "+%s", aMsgFrom.c_str());
								pChan->m_Users.remove(std::string(aBuff));

								pChan->AddMessage("*** '%s' left %s", aMsgFrom.c_str(), aMsgChannel.c_str());
							}
						}

					   reply.channel = aMsgChannel;
					   reply.from = aMsgFrom;
					}
					else if (aMsgID.compare("QUIT") == 0)
					{
						del = NetData.find_first_of(":",1);
						std::string aMsgText = NetData.substr(del+1, NetData.length()-del-1);
						del = aMsgFServer.find_first_of("!");
						std::string aMsgFrom = aMsgFServer.substr(0,del);
						char aBuff[255];

						if (aMsgFrom != m_Nick) // this applies only for channels, not for queries
						{
							std::list<CIRCCom*>::iterator it = m_IRCComs.begin();
							while (it != m_IRCComs.end())
							{
								if (!(*it) || (*it)->GetType() != CIRCCom::TYPE_CHANNEL)
								{
									++it;
									continue;
								}

								CComChan *pChan = static_cast<CComChan*>((*it));
								if (!pChan)
								{
									++it;
									continue;
								}

								pChan->m_Users.remove(aMsgFrom);
								str_format(aBuff, sizeof(aBuff), "@%s", aMsgFrom.c_str());
								pChan->m_Users.remove(std::string(aBuff));
								str_format(aBuff, sizeof(aBuff), "+%s", aMsgFrom.c_str());
								pChan->m_Users.remove(std::string(aBuff));

								if(aMsgFrom != "circleci-bot") // ignore the ci bot
									pChan->AddMessage("*** '%s' has quit (%s)", aMsgFrom.c_str(), aMsgText.c_str());

								++it;
							}
						}

						reply.channel = "IRC";
						reply.params = aMsgText;
						reply.from = aMsgFrom;
					}
					else if (aMsgID.compare("TOPIC") == 0)
					{
						ldel = NetData.find_first_of(" ",del+1);
						std::string aMsgChan = NetData.substr(del+1, ldel-del-1);
						del = NetData.find_first_of(":",1);
						std::string aMsgText = NetData.substr(del+1, NetData.length()-del-1);
						del = aMsgFServer.find_first_of("!");
						std::string aMsgFrom = aMsgFServer.substr(0,del);

						CComChan *pChan = static_cast<CComChan*>(GetCom(aMsgChan));
						if (pChan)
						{
							pChan->m_Topic = aMsgText;
							pChan->AddMessage("*** '%s' changed topic to '%s'", aMsgFrom.c_str(), aMsgText.c_str());
						}

						reply.channel = aMsgChan;
						reply.from = aMsgFrom;
						reply.params = aMsgText;
					}
					else if (aMsgID.compare("PRIVMSG") == 0)
					{
						char aBuff[1024];
						ldel = NetData.find_first_of(" ", del + 1);
						std::string aMsgChan = NetData.substr(del + 1, ldel - del - 1);

						del = NetData.find_first_of(":", 1);
						std::string aMsgText = NetData.substr(del + 1, NetData.length() - del - 1);
						int MsgType = GetMsgType(aMsgText.c_str());

						del = aMsgFServer.find_first_of("!");
						std::string aMsgFrom = aMsgFServer.substr(0, del);

						if(MsgType == MSG_TYPE_TWSERVER) // somebody wants to know our server
						{
							if(aMsgChan == m_Nick)
							{
								del = aMsgText.find_first_of(" ");
								ldel = aMsgText.find_last_of(" ");
								if(del != std::string::npos && del != ldel)
								{
									char aAddr[32];
									mem_zero(aAddr, sizeof(aAddr));
									std::string CleanMsg = aMsgText.substr(10);
									CleanMsg = CleanMsg.substr(0, CleanMsg.length() - 1);
									size_t pc = CleanMsg.find_first_of(" ");
									if(pc != std::string::npos)
									{
										str_copy(aAddr, CleanMsg.substr(pc + 1).c_str(), sizeof(aAddr));
										if(m_CmdToken[0] != 0
												&& str_comp(CleanMsg.substr(0, pc).c_str(), m_CmdToken) == 0
												&& aAddr[0] != 0)
										{
											if(aAddr[0] != 0 && str_comp_nocase(aAddr, "NONE") != 0 && str_comp_nocase(aAddr, "FORBIDDEN") != 0)
											{
												m_pClient->Connect(aAddr);
											}
											else
											{
												CIRCCom *pCom = GetActiveCom();
												if(pCom)
												{
													char aBuf[128];
													if(str_comp_nocase(aAddr, "FORBIDDEN") == 0)
														str_format(aBuf, sizeof(aBuf),
																"*** '%s' forbids joining his server!",
																aMsgFrom.c_str());
													else if(str_comp_nocase(aAddr, "NONE") == 0)
														str_format(aBuf, sizeof(aBuf),
																"*** '%s' isn't playing on a server!",
																aMsgFrom.c_str());
													else
														str_format(aBuf, sizeof(aBuf),
																"*** '%s' sent invalid information!", aMsgFrom.c_str());
													pCom->m_Buffer.push_back(aBuf);
												}
											}

											mem_zero(m_CmdToken, sizeof(m_CmdToken));
										}
									}
								}
							}
						}
						else if(MsgType == MSG_TYPE_GET_TWSERVER) // somebody sent us his server
						{
							if(aMsgChan == m_Nick)
							{
								std::string CleanMsg = aMsgText.substr(13);
								CleanMsg = CleanMsg.substr(0, CleanMsg.length() - 1);

								if(!CleanMsg.empty())
									SendServer(aMsgFrom.c_str(), CleanMsg.c_str());
							}
						}
						else if(MsgType == MSG_TYPE_CTCP) // custom ctcp message
						{
							array<std::string> CmdListParams;
							char aBuf[512]; char *Ptr;
							str_copy(aBuf, aMsgText.c_str(), sizeof(aBuf));
							Ptr = aBuf+1;
							str_replace_char(Ptr, sizeof(aBuf)-1, '\1', '\0');
							dbg_msg("IRC", "got a CTCP '%s' from '%s'", Ptr, aMsgFrom.c_str());

							for (char *p = strtok(Ptr, " "); p != NULL; p = strtok(NULL, " "))
								CmdListParams.add(p);

							if(str_comp_nocase(CmdListParams[0].c_str(), "version") == 0)
								SendVersion(aMsgFrom.c_str());
							else if(str_comp_nocase(CmdListParams[0].c_str(), "time") == 0)
							{
								char aTime[32];
								time_t rawtime;
								struct tm *timeinfo;
								time(&rawtime);
								timeinfo = localtime(&rawtime);
								str_format(aTime, sizeof(aTime), "%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
								SendRaw("NOTICE %s :TIME %s", aMsgFrom.c_str(), aTime);
							}
							else if(str_comp_nocase(CmdListParams[0].c_str(), "playerinfo") == 0)
							{
								str_format(aBuf, sizeof(aBuf), "NOTICE %s :PLAYERINFO", aMsgFrom.c_str());
								CmdListParams.remove_index(0);
								while(CmdListParams.size())
								{
									str_append(aBuf, " '", sizeof(aBuf));
									if(str_comp_nocase("name", CmdListParams[0].c_str()) == 0)
										str_append(aBuf, g_Config.m_PlayerName, sizeof(aBuf));
									else if(str_comp_nocase("clan", CmdListParams[0].c_str()) == 0)
										str_append(aBuf, g_Config.m_PlayerClan, sizeof(aBuf));
									else if(str_comp_nocase("skin", CmdListParams[0].c_str()) == 0)
										str_append(aBuf, g_Config.m_ClPlayerSkin, sizeof(aBuf));
									else if(str_comp_nocase("dummy", CmdListParams[0].c_str()) == 0)
										str_append(aBuf, g_Config.m_ClDummyName, sizeof(aBuf));
									else
										str_append(aBuf, "\\\\¶¶_error:arg\\\\", sizeof(aBuf));
									str_append(aBuf, "' ", sizeof(aBuf));

									CmdListParams.remove_index(0);
								}
								SendRaw(aBuf);
							}
						}
						else // normal message
						{
							reply.channel = aMsgChan;
							reply.from = aMsgFrom;
							reply.params = aMsgText;


							{
								CIRCCom *pCom;
								if(aMsgChan == m_Nick) // this is the case for private chats ("Query"s)
								{
									pCom = GetCom(aMsgFrom);
									if(!pCom)
										pCom = OpenCom<CComQuery>(aMsgFrom.c_str(), false);
								}
								else
								{
									pCom = GetCom(aMsgChan);
									if(!pCom)
										pCom = OpenCom<CComChan>(aMsgChan.c_str(), false);
								}

								if(pCom)
								{
									if(pCom != GetActiveCom())
										pCom->m_NumUnreadMsg++;

									if(MsgType == MSG_TYPE_ACTION)
									{
										str_format(aBuff, sizeof(aBuff), "* %s %s", aMsgFrom.c_str(),
												aMsgText.substr(8, -1).c_str());
										str_replace_char(aBuff, sizeof(aBuff), '\1', '\0');
									}
									else
										str_format(aBuff, sizeof(aBuff), "<%s> %s", aMsgFrom.c_str(),
												aMsgText.c_str());
									pCom->AddMessage(aBuff);
								}

								if(pCom == GetActiveCom())
								{
									aMsgChan.insert(0, "[");
									aMsgChan.append("] ");
									aMsgFrom.insert(0, "<");
									aMsgFrom.append("> ");
									//aMsgFrom.insert(0, aTime);
									m_pGameClient->OnMessageIRC(aMsgChan.c_str(), aMsgFrom.c_str(), aMsgText.c_str());
								}
							}
						}
					}
					else if (aMsgID.compare("NOTICE") == 0)
					{
						char aBuff[1024];
						ldel = NetData.find_first_of(" ", del + 1);
						std::string aMsgChan = NetData.substr(del + 1, ldel - del - 1);

						del = NetData.find_first_of(":", 1);
						std::string aMsgText = NetData.substr(del + 1, NetData.length() - del - 1);
						int MsgType = GetMsgType(aMsgText.c_str());

						del = aMsgFServer.find_first_of("!");
						std::string aMsgFrom = aMsgFServer.substr(0, del);
						{
							reply.channel = aMsgChan;
							reply.from = aMsgFrom;
							reply.params = aMsgText;

							if(aMsgChan == m_Nick) // TODO refractor: sending ourselves a NOTICE??
							{
								CIRCCom *pCom = GetCom(aMsgFrom);
								std::replace(aMsgText.begin(), aMsgText.end(), '\1', ' ');
								if(!pCom)
									pCom = OpenCom<CComQuery>(aMsgFrom.c_str(), false);

								if(pCom)
								{
									if(pCom != GetActiveCom())
										pCom->m_NumUnreadMsg++;

									if(MsgType == MSG_TYPE_ACTION)
										pCom->AddMessage(aMsgText.substr(8, -1).c_str());
									else
										pCom->AddMessage(aMsgText.c_str());
								}
							}
							else
							{
								CIRCCom *pCom = GetCom(aMsgChan);
								if(pCom)
								{
									if(pCom != GetActiveCom())
										pCom->m_NumUnreadMsg++;

									if(MsgType == MSG_TYPE_ACTION)
										str_format(aBuff, sizeof(aBuff), "* %s %s", aMsgFrom.c_str(), aMsgText.substr(8, -1).c_str());
									else
										str_format(aBuff, sizeof(aBuff), "<%s> %s", aMsgFrom.c_str(), aMsgText.c_str());
									pCom->AddMessage(aBuff);
								}

								if(pCom == GetActiveCom())
								{
									aMsgChan.insert(0, "[");
									aMsgChan.append("] ");
									aMsgFrom.insert(0, "<");
									aMsgFrom.append("> ");
									//aMsgFrom.insert(0, aTime);
									m_pGameClient->OnMessageIRC(aMsgChan.c_str(), aMsgFrom.c_str(), aMsgText.c_str());
								}
							}
						}
					}
					else if (aMsgID.compare("NICK") == 0)
					{
						del = aMsgFServer.find_first_of("!");
						std::string aMsgOldNick = aMsgFServer.substr(0,del);

						del = NetData.find_first_of(":", 1);
						std::string aMsgNewNick = NetData.substr(del+1, NetData.length()-del-1);

						if(aMsgOldNick == m_Nick) // our name has changed
							m_Nick = aMsgNewNick;

						std::list<CIRCCom*>::iterator it = m_IRCComs.begin();
						while (it != m_IRCComs.end())
						{
							if ((*it)->GetType() == CIRCCom::TYPE_QUERY)
							{
								CComQuery *pQuery = static_cast<CComQuery*>((*it));
								if (str_comp_nocase(pQuery->m_Name, aMsgOldNick.c_str()) == 0)
								{
									str_copy(pQuery->m_Name, aMsgNewNick.c_str(), sizeof(pQuery->m_Name));
									pQuery->AddMessage( "*** '%s' changed their nick to '%s'", aMsgOldNick.c_str(), aMsgNewNick.c_str());
								}
							}
							else if ((*it)->GetType() == CIRCCom::TYPE_CHANNEL)
							{
								CComChan *pChan = static_cast<CComChan*>((*it));
								std::list<std::string>::iterator itU = pChan->m_Users.begin();
								while (itU != pChan->m_Users.end())
								{
									std::string NickOper = aMsgOldNick; NickOper.insert(0, "@");
									std::string NickVoice = aMsgOldNick; NickVoice.insert(0, "+");

									bool got = false;
									if (str_comp_nocase((*itU).c_str(), aMsgOldNick.c_str()) == 0)
									{
										(*itU) = aMsgNewNick;
										got = true;
									}
									else if (str_comp_nocase((*itU).c_str(), NickOper.c_str()) == 0)
									{
										(*itU) = aMsgNewNick;
										(*itU).insert(0, "@");
										got = true;
									}
									else if (str_comp_nocase((*itU).c_str(), NickVoice.c_str()) == 0)
									{
										(*itU) = aMsgNewNick;
										(*itU).insert(0, "+");
										got = true;
									}

									if(got)
									{
										pChan->AddMessage("*** '%s' changed their nick to '%s'", aMsgOldNick.c_str(), aMsgNewNick.c_str());
										pChan->m_Users.sort();
										break;
									}

									++itU;
								}
							}

							++it;
						}

						reply.from = aMsgOldNick;
						reply.to = aMsgNewNick;
					}
					else if (aMsgID.compare("MODE") == 0)
					{
						del = aMsgFServer.find_first_of("!");
						std::string aNickFrom = aMsgFServer.substr(0,del);

						del = NetData.find_first_of(" ");
						ldel = NetData.find_first_of(" ", del+1);
						del = NetData.find_first_of(" ", ldel+1);
						std::string aChannel = NetData.substr(ldel+1, (del)-(ldel+1));

						ldel = NetData.find_first_of(" ", del+1);
						std::string aMode = NetData.substr(del+1, ldel-(del+1));

						ldel = NetData.find_first_of(" ", del+1);
						del = NetData.find_first_of(" ", ldel+1);
						std::string aNickTo = NetData.substr(ldel+1, del-(ldel+1));


						CIRCCom *pCom = GetCom(aChannel);
						if (pCom && pCom->GetType() == CIRCCom::TYPE_CHANNEL)
						{
							CComChan *pChan = static_cast<CComChan*>(pCom);
							if (pChan)
							{
								char aGenericTerm[32] = {0};
								str_format(aGenericTerm, sizeof(aGenericTerm), "%s %s %s",
										aMode.c_str()[0] == '+' ? "gives" : "removes",
											aMode.c_str()[1] == 'o' ? "operator" :
											aMode.c_str()[1] == 'v' ? "voice" :
											aMode.c_str()[1] == 'b' ? "a ban" : "unknown",
										aMode.c_str()[0] == '+' ? "to" : "from");
								pChan->AddMessage("*** '%s' %s '%s' (mode%s)", aNickFrom.c_str(), aGenericTerm, aNickTo.c_str(), aMode.c_str());

								std::string aNewNick = aNickTo;
								std::string aNickToVoz = aNickTo; aNickToVoz.insert(0, "+");

								if (aMode[0] == '+')
								{
									if (aMode[1] == 'o')
										aNewNick.insert(0, "@");
									else if (aMode[1] == 'v')
										aNewNick.insert(0, "+");
								}
								else if (aMode[0] == '-')
								{
									if (aMode[1] == 'o')
										aNickTo.insert(0, "@");
								}

								std::list<std::string>::iterator it = pChan->m_Users.begin();
								while (it != pChan->m_Users.end())
								{
									if ((*it).compare(aNickTo) == 0 || (*it).compare(aNickToVoz) == 0)
									{
										(*it) = aNewNick;
										break;
									}
									++it;
								}

								pChan->m_Users.sort();
							}
						}
						reply.channel = aChannel;
						reply.from = aNickFrom;
						reply.to = aNickTo;
						reply.params = aMode;
					}
					else if (aMsgID.compare("KICK") == 0)
					{
						del = aMsgFServer.find_first_of("!");
						std::string aNickFrom = aMsgFServer.substr(0,del);

						del = NetData.find_first_of(" ");
						ldel = NetData.find_first_of(" ", del+1);
						del = NetData.find_first_of(" ", ldel+1);
						std::string aChannel = NetData.substr(ldel+1, (del)-(ldel+1));

						ldel = NetData.find_first_of(" ", del+1);
						std::string aNickTo = NetData.substr(del+1, ldel-(del+1));

						ldel = NetData.find_first_of(":", 1);
						std::string aKickReason = NetData.substr(ldel+1);


						CIRCCom *pCom = GetCom(aChannel);
						if (pCom && pCom->GetType() == CIRCCom::TYPE_CHANNEL)
						{
							if (aNickTo == m_Nick) // we got kicked
							{
								m_IRCComs.remove(pCom);
								mem_free(pCom);
								pCom=0x0;
								SetActiveCom(0);
								GetActiveCom()->AddMessage("You got kicked from channel '%s', Reason: '%s'", aChannel.c_str(), aKickReason.c_str());
							}
							else
							{
								CComChan *pChan = static_cast<CComChan*>(pCom);

								pChan->AddMessage("*** '%s' kicked '%s' (Reason: %s)", aNickFrom.c_str(), aNickTo.c_str(), aKickReason.c_str());

								pChan->m_Users.remove(aNickTo);
								aNickTo.insert(0, "@");
								pChan->m_Users.remove(aNickTo);
								aNickTo[0]='+';
								pChan->m_Users.remove(aNickTo);
							}
						}
						reply.channel = aChannel;
						reply.from = aNickFrom;
						reply.to = aNickTo;
						reply.params = aKickReason;
					}
					else
					{
						char aBuff[1024];
						ldel = NetData.find_first_of(" ", del+1);
						del = ldel;
						ldel = NetData.find_first_of(" ", del+1);
						std::string aMsgData = NetData.substr(del+1, ldel-del-1);
						del = NetData.find_first_of(":", 1);
						std::string aMsgText = NetData.substr(del+1, NetData.length()-del-1);

						if (ldel < del && ldel != std::string::npos)
							str_format(aBuff, sizeof(aBuff), "%s %s", aMsgData.c_str(), aMsgText.c_str());
						else
							str_format(aBuff, sizeof(aBuff), "%s", aMsgText.c_str());

						GetCom("@Status")->m_Buffer.push_back(aBuff);
					}

					CallHooks(aMsgID.c_str(), &reply);
				}

				NetData.clear();
				continue;
			}
			NetData += aNetBuff[i];
		}
	}

	// finish
	net_tcp_close(m_Socket);
	m_State = STATE_DISCONNECTED;

	Disconnect();
}

void CIRC::NextRoom()
{
	/* -- dunno if i misunderstood this...? -Henritees
	if (m_ActiveCom >= (int)m_IRCComs.size()-1)
		SetActiveCom(((int)m_IRCComs.size()>1)?1:0);
	else if (m_ActiveCom <= 0)
		SetActiveCom((int)m_IRCComs.size()-1);
	else
		SetActiveCom(m_ActiveCom+1);*/
	if (m_ActiveCom >= (int)m_IRCComs.size()-1)
		SetActiveCom(0);
	else
		SetActiveCom(m_ActiveCom+1);
}

void CIRC::PrevRoom()
{
	if (m_ActiveCom <= 0)
		SetActiveCom((int)m_IRCComs.size()-1);
	else
		SetActiveCom(m_ActiveCom-1);
}

void CIRC::OpenQuery(const char *to)
{
	char SanNick[25] = {0};
	str_copy(SanNick, (to[0] == '@' || to[0] == '+')?to+1:to, sizeof(SanNick));
	
	CIRCCom *pCom = GetCom(SanNick);
	if (pCom)
		SetActiveCom(pCom);
	else
		OpenCom<CComQuery>((to[0] == '@' || to[0] == '+')?to+1:to);
}

template<class TCOM> // returns a new CComChan or CComQuery
TCOM* CIRC::OpenCom(const char *pName, bool SwitchTo, int UnreadMessages)
{
	// XXX: hack to suppress opening of the useless tabs at startup
	if(!g_Config.m_ClIRCGetStartupMsgs &&
			time_get() < m_StartTime+time_freq()*2 && // give it a couple of seconds; should be enough
			str_comp_nocase(pName, "#AllTheHaxx") != 0 &&
			str_comp_nocase(pName, "@Status") != 0)
		return 0;

	TCOM *pNewCom = new(mem_alloc(sizeof(TCOM), 0)) TCOM();
	pNewCom->m_NumUnreadMsg = UnreadMessages;
	str_copy(pNewCom->m_Name, pName, sizeof(pNewCom->m_Name));
	m_IRCComs.push_back(pNewCom);

	if(SwitchTo)
		SetActiveCom(m_IRCComs.size()-1);

	return pNewCom;
}

void CIRCCom::AddMessage(const char *fmt, ...)
{
	if(!fmt || fmt[0] == 0)
		return;

	char aTime[32];
	time_t rawtime;
	struct tm *timeinfo;
	time(&rawtime);
	timeinfo = localtime(&rawtime);
	str_format(aTime, sizeof(aTime), "[%02d:%02d:%02d] ", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);

	va_list args;
	char aMsg[768];

	va_start(args, fmt);
	#if defined(CONF_FAMILY_WINDOWS)
		_vsnprintf(aMsg, sizeof(aMsg), fmt, args);
	#else
		vsnprintf(aMsg, sizeof(aMsg), fmt, args);
	#endif
	va_end(args);

	m_Buffer.push_back(std::string(aTime) + std::string(aMsg));
}

void CIRC::JoinTo(const char *to, const char *pass)
{
	SendRaw("JOIN %s %s", to, pass);
}

void CIRC::SetMode(const char *mode, const char *to)
{
	CIRCCom *pCom = GetActiveCom();
	if (!pCom || pCom->GetType() == CIRCCom::TYPE_QUERY)
		return;

	CComChan *pChan = static_cast<CComChan*>(pCom);
	if (!pChan)
		return;

	if (!to || to[0] == 0)
		SendRaw("MODE %s %s %s", pChan->m_Name, mode, m_Nick.c_str());
	else
		SendRaw("MODE %s %s %s", pChan->m_Name, mode, to);
}

void CIRC::SetTopic(const char *topic)
{
	CIRCCom *pCom = GetActiveCom();
	if (!pCom || pCom->GetType() != CIRCCom::TYPE_CHANNEL)
		return;

	CComChan *pChan = static_cast<CComChan*>(pCom);
	SendRaw("TOPIC %s :%s", pChan->m_Name, topic);
}

void CIRC::Part(const char *pReason, CIRCCom *pCom)
{
	if(!pCom)
		pCom = GetCom(m_ActiveCom);

	if (!pCom)
		return;

	if (pCom->GetType() == CIRCCom::TYPE_CHANNEL)
	{
		CComChan *pChan = static_cast<CComChan*>(pCom);
		if(pReason && pReason[0])
			SendRaw("PART %s :%s", pChan->m_Name, pReason);
		else
			SendRaw("PART %s %", pChan->m_Name);

		m_IRCComs.remove(pCom);
		mem_free(pCom);
		pCom=0x0;
		SetActiveCom(m_IRCComs.size()-1);
	}
	else if (pCom->GetType() == CIRCCom::TYPE_QUERY)
	{
		CComQuery *pQuery = static_cast<CComQuery*>(pCom);
		if (str_comp_nocase(pQuery->m_Name, "@Status") == 0)
			return;

		m_IRCComs.remove(pCom);
		mem_free(pCom);
		pCom=0x0;
		SetActiveCom(m_IRCComs.size()-1);
	}
}

void CIRC::Disconnect(const char *pReason)
{
	if (m_State != STATE_DISCONNECTED)
	{
		if(pReason && pReason[0])
			SendRaw("QUIT :%s", pReason);
		else
			SendRaw("QUIT");
		m_State = STATE_DISCONNECTED;
	}

	std::list<CIRCCom*>::iterator it = m_IRCComs.begin();
	while (it != m_IRCComs.end())
	{
		mem_free((*it));
		it = m_IRCComs.erase(it);
	}

	mem_zero(m_CmdToken, sizeof(m_CmdToken));
}

void CIRC::SendMsg(const char *to, const char *msg, int type)
{ //TODO: Rework this! duplicate code :P
	if (GetState() == STATE_DISCONNECTED || !msg || msg[0] == 0)
		return;

	char aBuff[1024];
	char aDest[25];

	// Search Destination
	if (!to || to[0] == 0)
	{
		if (m_ActiveCom == -1)
			return;

		std::list<CIRCCom*>::iterator it = m_IRCComs.begin();
		std::advance(it, m_ActiveCom);
		if ((*it)->GetType() == CIRCCom::TYPE_CHANNEL)
		{
			CComChan *pChan = static_cast<CComChan*>((*it));
			str_copy(aDest, pChan->m_Name, sizeof(aDest));
		}
		else if ((*it)->GetType() == CIRCCom::TYPE_QUERY)
		{
			CComQuery *pQuery = static_cast<CComQuery*>((*it));
			if (str_comp_nocase(pQuery->m_Name, "@Status") == 0)
			{
				pQuery->AddMessage("*** You can't send messages to '@Status'!", GetNick(), msg);
				return;
			}

			str_copy(aDest, pQuery->m_Name, sizeof(aDest));
		}
		else
			return;
	}
	else
		str_copy(aDest, to, sizeof(aDest));


	// Send
	if (type == MSG_TYPE_ACTION)
		str_format(aBuff, sizeof(aBuff), "\1ACTION %s\1", msg);
	else
		str_copy(aBuff, msg, sizeof(aBuff));

	SendRaw("PRIVMSG %s :%s", aDest, aBuff);
	CIRCCom *pCom = GetCom(aDest);
	if (pCom)
	{
		if (type == MSG_TYPE_ACTION)
			str_format(aBuff, sizeof(aBuff),"* %s %s", GetNick(), msg); // XXX: This seems to be useless...? (broken)
		else
			str_format(aBuff, sizeof(aBuff),"<%s> %s", GetNick(), msg);
		pCom->AddMessage(aBuff);
	}
}

void CIRC::SendRaw(const char *fmt, ...)
{
	if (!fmt || fmt[0] == 0)
		return;

	va_list args;
	char msg[1024*4];

	va_start(args, fmt);
	#if defined(CONF_FAMILY_WINDOWS)
		_vsnprintf(msg, sizeof(msg), fmt, args);
	#else
		vsnprintf(msg, sizeof(msg), fmt, args);
	#endif
	va_end(args);

	str_append(msg, "\r\n", sizeof(msg));
	net_tcp_send(m_Socket, msg, str_length(msg));
}

void CIRC::SetNick(const char *nick)
{
	if (m_State == STATE_CONNECTED)
		SendRaw("NICK %s", nick);

	m_Nick = nick;
}

void CIRC::SetAway(bool state, const char *msg)
{
	if (state)
		SendRaw("AWAY :%s", msg);
	else
		SendRaw("AWAY");
}

int CIRC::GetMsgType(const char *msg)
{
	int len = str_length(msg);
	if (len > 0 && msg[0] == '\1' && msg[len-1] == '\1')
	{
		char aCmd[12];
		mem_zero(aCmd, sizeof(aCmd));
		for(int i = 1, e = 0; i < len && msg[i] != ' ' && e < (int)sizeof(aCmd); i++, e++)
			aCmd[e] = msg[i];

		if (str_comp_nocase(aCmd, "ACTION") == 0)
			return MSG_TYPE_ACTION;
		else if (str_comp_nocase(aCmd, "TWSERVER") == 0)
			return MSG_TYPE_TWSERVER;
		else if (str_comp_nocase(aCmd, "GETTWSERVER") == 0)
			return MSG_TYPE_GET_TWSERVER;

		return MSG_TYPE_CTCP;
	}

	return MSG_TYPE_NORMAL;
}

void CIRC::SendServer(const char *to, const char *Token)
{
	const char *curAddr = m_pClient->GetCurrentServerAddress();
	SendRaw("PRIVMSG %s :\1TWSERVER %s %s\1", to, Token, g_Config.m_ClIRCAllowJoin ? ((curAddr&&curAddr[0]!=0)?curAddr:"NONE") : "FORBIDDEN");
}

void CIRC::SendGetServer(const char *to)
{
	str_format(m_CmdToken, sizeof(m_CmdToken), "%ld", time_get());
	SendRaw("PRIVMSG %s :\1GETTWSERVER %s\1", to, m_CmdToken);
}

void CIRC::SendVersion(const char *to)
{
	SendRaw("NOTICE %s :VERSION AllTheHaxx %s; DDNet v%i; Teeworlds %s (%s); built on %s", to,
			ALLTHEHAXX_VERSION, CLIENT_VERSIONNR, GAME_VERSION, GAME_NETVERSION, BUILD_DATE);
}

void CIRC::ExecuteCommand(const char *cmd, char *params)
{
	array<std::string> CmdListParams;
	for (char *p = strtok(params, " "); p != NULL; p = strtok(NULL, " "))
		CmdListParams.add(p);

	if (str_comp_nocase(cmd, "join") == 0 || str_comp_nocase(cmd, "j") == 0)
	{
		if (CmdListParams.size() == 0)
			return;

		JoinTo(CmdListParams[0].c_str(), (CmdListParams.size() > 1)?CmdListParams[1].c_str():"");
	}
	else if (str_comp_nocase(cmd, "query") == 0 || str_comp_nocase(cmd, "q") == 0)
	{
		if (CmdListParams.size() == 0)
			return;

		OpenQuery(CmdListParams[0].c_str());
	}
	else if (str_comp_nocase(cmd, "squery") == 0 || str_comp_nocase(cmd, "sq") == 0)
	{
		if (CmdListParams.size() == 0)
			return;

		SendGetServer(CmdListParams[0].c_str());
	}
	else if (str_comp_nocase(cmd, "topic") == 0 || str_comp_nocase(cmd, "t") == 0)
	{
		if (CmdListParams.size() == 0)
			return;

		SetTopic(params);
	}
	else if (str_comp_nocase(cmd, "part") == 0 || str_comp_nocase(cmd, "p") == 0)
	{
		Part();
	}
	else if (str_comp_nocase(cmd, "nick") == 0)
	{
		if (CmdListParams.size() == 0)
			return;

		SetNick(CmdListParams[0].c_str());
	}
	else if (str_comp_nocase(cmd, "op") == 0)
	{
		if (CmdListParams.size() > 0)
			SetMode("+o", CmdListParams[0].c_str());
		else
			SetMode("+o", 0x0);
	}
	else if (str_comp_nocase(cmd, "deop") == 0)
	{
		if (CmdListParams.size() > 0)
			SetMode("-o", CmdListParams[0].c_str());
		else
			SetMode("-o", 0x0);
	}
	else if (str_comp_nocase(cmd, "voz") == 0)
	{
		if (CmdListParams.size() > 0)
			SetMode("+v", CmdListParams[0].c_str());
		else
			SetMode("+v", 0x0);
	}
	else if (str_comp_nocase(cmd, "devoz") == 0)
	{
		if (CmdListParams.size() > 0)
			SetMode("-v", CmdListParams[0].c_str());
		else
			SetMode("-v", 0x0);
	}
	else if (str_comp_nocase(cmd, "clear") == 0)
	{
		CIRCCom *pCom = GetActiveCom();
		if (pCom)
			pCom->m_Buffer.clear();
	}
	else if (str_comp_nocase(cmd, "msg") == 0)
	{
		char aBuf[1024] = {0};
		if (CmdListParams.size() >= 2)
		{
			str_format(aBuf, sizeof(aBuf), "PRIVMSG %s :%s",
					CmdListParams[0].c_str(), CmdListParams[1].c_str()); // to & what
			CmdListParams.remove_index(0); // pop twice
			CmdListParams.remove_index(0); //   -> the first two arguments
			while(CmdListParams.size() > 0) // add all other arguments
			{
				str_append(aBuf, " ", sizeof(aBuf));
				str_append(aBuf, CmdListParams[0].c_str(), sizeof(aBuf));
				CmdListParams.remove_index(0);
			}
			SendRaw(aBuf);
		}
	}
	else if (str_comp_nocase(cmd, "ctcp") == 0)
	{
		char aBuf[1024] = {0};
		if (CmdListParams.size() >= 2)
		{
			str_format(aBuf, sizeof(aBuf), "PRIVMSG %s :\1%s",
					CmdListParams[0].c_str(), CmdListParams[1].c_str()); // to & what
			CmdListParams.remove_index(0); // pop twice
			CmdListParams.remove_index(0); //   -> the first two arguments
			while(CmdListParams.size() > 0) // add all other arguments
			{
				str_append(aBuf, " ", sizeof(aBuf));
				str_append(aBuf, CmdListParams[0].c_str(), sizeof(aBuf));
				CmdListParams.remove_index(0);
			}
			aBuf[str_length(aBuf)] = '\1';
			SendRaw(aBuf);
		}
	}
	else if(str_comp_nocase(cmd, "me") == 0)
	{
		char aBuf[1024] = {0};
		char aMsg[768] = {0};
		if (CmdListParams.size() >= 1)
		{
			str_format(aMsg, sizeof(aMsg), "%s", CmdListParams[0].c_str()); // first word
			CmdListParams.remove_index(0); // pop
			while(CmdListParams.size() > 0) // add all other arguments to the message
			{
				str_append(aMsg, " ", sizeof(aMsg));
				str_append(aMsg, CmdListParams[0].c_str(), sizeof(aMsg));
				CmdListParams.remove_index(0);
			}

			str_format(aBuf, sizeof(aBuf), "PRIVMSG %s :\1ACTION %s",
					GetActiveCom()->GetType() == CIRCCom::TYPE_QUERY ?
							((CComQuery*)GetActiveCom())->m_Name : ((CComChan*)GetActiveCom())->m_Name,
					aMsg); // message text


			aBuf[str_length(aBuf)] = '\1';
			SendRaw(aBuf);
			GetActiveCom()->AddMessage("* %s %s", m_Nick.c_str(), aMsg);
		}
	}
	else
		SendRaw("%s %s", cmd, params);
}

int CIRC::NumUnreadMessages(int *pArray)
{
	int NumChan = 0, NumQuery = 0;
	for(int i = 0; i < GetNumComs(); i++)
	{
		CIRCCom *pCom = GetCom(i);
		if(pCom->GetType() == CIRCCom::TYPE_CHANNEL)
			NumChan += ((CComChan *)pCom)->m_NumUnreadMsg;
		else if(pCom->GetType() == CIRCCom::TYPE_QUERY)
			NumQuery += ((CComQuery *)pCom)->m_NumUnreadMsg;
	}

	if(pArray)
	{
		pArray[0] = NumChan;
		pArray[1] = NumQuery;
	}

	return NumChan + NumQuery;
}
