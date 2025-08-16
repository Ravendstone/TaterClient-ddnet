// Raven top minimal component
#include <algorithm>

#include <base/log.h>
#include <base/system.h>

#include <engine/client.h>
#include <engine/external/json-parser/json.h>

#include <game/client/components/chat.h>
#include <game/client/gameclient.h>

#include "raventop.h"

static void SimpleEscape(char *pDst, int DstSize, const char *pSrc)
{
    int i = 0;
    for(; *pSrc && i < DstSize - 4; ++pSrc)
    {
        if(*pSrc == ' ')
        {
            pDst[i++] = '%';
            pDst[i++] = '2';
            pDst[i++] = '0';
        }
        else
            pDst[i++] = *pSrc;
    }
    pDst[i] = '\0';
}

void CRavenTop::OnConsoleInit()
{
    Console()->Register("show_top", "i[count]", CFGFLAG_CLIENT, ConShowTop, this, "Show top N records for current map (default 10, max 50)");
}

void CRavenTop::OnReset()
{
    m_NumRecords = 0;
    m_State = STATE_IDLE;
    m_aCurrentMap[0] = '\0';
    m_PendingChatDisplay = false;
    m_RequestedCount = 10;
    if(m_pRequest)
    {
        m_pRequest->Abort();
        m_pRequest.reset();
    }
}

void CRavenTop::OnMapLoad()
{
    const char *pMap = Client()->GetCurrentMap();
    if(pMap)
        str_copy(m_aCurrentMap, pMap, sizeof(m_aCurrentMap));
    m_NumRecords = 0;
    m_State = STATE_IDLE;
}

void CRavenTop::OnRender()
{
    if(m_pRequest && m_pRequest->Done())
    {
        if(m_pRequest->State() == EHttpState::DONE && m_pRequest->StatusCode() == 200)
        {
            unsigned char *pResult = nullptr;
            size_t Len = 0;
            m_pRequest->Result(&pResult, &Len);
            if(pResult && Len)
            {
                char *pJson = (char *)malloc(Len + 1);
                mem_copy(pJson, pResult, Len);
                pJson[Len] = '\0';
                ParseResponse(pJson);
                free(pJson);
            }
            else
            {
                m_State = STATE_ERROR;
            }
        }
        else
        {
            log_error("raventop", "HTTP failed status=%d", m_pRequest->StatusCode());
            m_State = STATE_ERROR;
        }
        m_pRequest.reset();
        if(m_PendingChatDisplay)
        {
            m_PendingChatDisplay = false;
            if(HasValidData())
                ShowTopInChat(m_RequestedCount);
            else
                GameClient()->m_Chat.AddLine(TEAM_ALL, 0, "Failed to load map times data.");
        }
    }
}

void CRavenTop::RequestMapTimes(const char *pMapName)
{
    if(!pMapName || !pMapName[0])
        return;
    int64_t Now = time_get();
    if(Now - m_LastRequestTime < time_freq() * 10)
        return;
    m_LastRequestTime = Now;
    m_State = STATE_LOADING;
    m_NumRecords = 0;
    if(m_pRequest)
    {
        m_pRequest->Abort();
        m_pRequest.reset();
    }
    char aEscaped[256];
    SimpleEscape(aEscaped, sizeof(aEscaped), pMapName);
    char aUrl[512];
    str_format(aUrl, sizeof(aUrl), "https://www.ravenkog.com/api/maps?mapName=%s", aEscaped);
    m_pRequest = HttpGet(aUrl);
    m_pRequest->Timeout(CTimeout{10000, 0, 500, 10});
    m_pRequest->WriteToMemory();
    m_pRequest->LogProgress(HTTPLOG::FAILURE);
    GameClient()->Http()->Run(m_pRequest);
    log_debug("raventop", "Requesting map '%s' via %s", pMapName, aUrl);
}

void CRavenTop::ParseResponse(const char *pJson)
{
    json_value *pRoot = json_parse(pJson, str_length(pJson));
    if(!pRoot)
    {
        log_error("raventop", "parse error");
        m_State = STATE_ERROR;
        return;
    }
    m_NumRecords = 0;
    const json_value *pTop100 = nullptr;
    if(pRoot->type == json_object)
    {
        for(unsigned i = 0; i < pRoot->u.object.length; i++)
        {
            if(str_comp(pRoot->u.object.values[i].name, "top100") == 0)
            {
                pTop100 = pRoot->u.object.values[i].value;
                break;
            }
        }
    }
    if(!pTop100 || pTop100->type != json_array)
    {
        log_error("raventop", "no top100 array");
        json_value_free(pRoot);
        m_State = STATE_ERROR;
        return;
    }
    int Limit = (int)std::min<size_t>(pTop100->u.array.length, (size_t)MAX_TOP_RECORDS);
    for(int i = 0; i < Limit; i++)
    {
        const json_value *pRecord = pTop100->u.array.values[i];
        if(!pRecord || pRecord->type != json_object)
            continue;
        SRavenTopRecord &R = m_aTopRecords[m_NumRecords];
        for(unsigned j = 0; j < pRecord->u.object.length; j++)
        {
            const char *pKey = pRecord->u.object.values[j].name;
            const json_value *pVal = pRecord->u.object.values[j].value;
            if(str_comp(pKey, "playerName") == 0 && pVal->type == json_string)
            {
                str_copy(R.m_aPlayerName, pVal->u.string.ptr, sizeof(R.m_aPlayerName));
            }
            else if(str_comp(pKey, "time") == 0 && pVal->type == json_object)
            {
                for(unsigned k = 0; k < pVal->u.object.length; k++)
                {
                    if(str_comp(pVal->u.object.values[k].name, "value") == 0 && pVal->u.object.values[k].value->type == json_string)
                    {
                        str_copy(R.m_aTime, pVal->u.object.values[k].value->u.string.ptr, sizeof(R.m_aTime));
                        break;
                    }
                }
            }
            else if(str_comp(pKey, "rank") == 0 && pVal->type == json_integer)
            {
                R.m_Rank = (int)pVal->u.integer;
            }
        }
        if(R.m_aPlayerName[0] && R.m_aTime[0])
            m_NumRecords++;
    }
    json_value_free(pRoot);
    if(m_NumRecords)
    {
        m_State = STATE_DONE;
        log_debug("raventop", "parsed %d records", m_NumRecords);
    }
    else
    {
        m_State = STATE_ERROR;
        log_error("raventop", "no valid records");
    }
}

void CRavenTop::FormatTime(char *pBuf, int BufSize, const char *pTimeString)
{
    if(!pTimeString || !pTimeString[0])
    {
        pBuf[0] = '\0';
        return;
    }
    const char *pDot = str_find(pTimeString, ".");
    if(!pDot)
    {
        str_copy(pBuf, pTimeString, BufSize);
        return;
    }
    int BaseLen = (int)(pDot - pTimeString);
    int Total = BaseLen + 3;
    int SrcLen = str_length(pTimeString);
    if(Total > SrcLen)
        Total = SrcLen;
    if(Total >= BufSize)
        Total = BufSize - 1;
    mem_copy(pBuf, pTimeString, Total);
    pBuf[Total] = '\0';
}

void CRavenTop::ShowTopInChat(int Count)
{
    if(!HasValidData())
    {
        GameClient()->m_Chat.AddLine(TEAM_ALL, 0, "No map times yet.");
        return;
    }
    int Show = std::min(Count, m_NumRecords);
    char aHeader[128];
    str_format(aHeader, sizeof(aHeader), "=== Top %d Records for %s ===", Show, m_aCurrentMap);
    GameClient()->m_Chat.AddLine(TEAM_ALL, 0, aHeader);
    for(int i = 0; i < Show; i++)
    {
        const SRavenTopRecord &R = m_aTopRecords[i];
        char aFmt[32];
        FormatTime(aFmt, sizeof(aFmt), R.m_aTime);
        char aLine[256];
        str_format(aLine, sizeof(aLine), "%d. %s - %s", i + 1, R.m_aPlayerName, aFmt);
        GameClient()->m_Chat.AddLine(TEAM_ALL, 0, aLine);
    }
}

void CRavenTop::ConShowTop(IConsole::IResult *pResult, void *pUserData)
{
    CRavenTop *pSelf = static_cast<CRavenTop *>(pUserData);
    int Count = 10;
    if(pResult->NumArguments() > 0)
    {
        Count = pResult->GetInteger(0);
        if(Count < 1)
            Count = 1;
        if(Count > 50)
            Count = 50;
    }
    pSelf->m_RequestedCount = Count;
    if(pSelf->HasValidData())
    {
        pSelf->ShowTopInChat(Count);
        return;
    }
    const char *pMap = pSelf->Client()->GetCurrentMap();
    if(pMap && pMap[0])
    {
        str_copy(pSelf->m_aCurrentMap, pMap, sizeof(pSelf->m_aCurrentMap));
        pSelf->GameClient()->m_Chat.AddLine(TEAM_ALL, 0, "Requesting map times data...");
        pSelf->m_PendingChatDisplay = true;
        pSelf->RequestMapTimes(pMap);
    }
    else
    {
        pSelf->GameClient()->m_Chat.AddLine(TEAM_ALL, 0, "No map loaded.");
    }
}
