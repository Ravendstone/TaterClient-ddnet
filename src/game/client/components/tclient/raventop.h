// (c) Custom Raven Top minimal component
#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_RAVENTOP_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_RAVENTOP_H

#include <game/client/component.h>
#include <engine/console.h>
#include <engine/shared/http.h>
#include <engine/shared/protocol.h>
#include <memory>

struct SRavenTopRecord
{
    char m_aPlayerName[MAX_NAME_LENGTH];
    char m_aTime[32];
    int m_Rank;
    SRavenTopRecord()
    {
        m_aPlayerName[0] = '\0';
        m_aTime[0] = '\0';
        m_Rank = 0;
    }
};

class CRavenTop : public CComponent
{
public:
    enum
    {
        MAX_TOP_RECORDS = 50,
        STATE_IDLE,
        STATE_LOADING,
        STATE_DONE,
        STATE_ERROR
    };

    int Sizeof() const override { return sizeof(*this); }

    void OnConsoleInit() override;
    void OnReset() override;
    void OnRender() override;
    void OnMapLoad() override;

    static void ConShowTop(IConsole::IResult *pResult, void *pUserData);

private:
    SRavenTopRecord m_aTopRecords[MAX_TOP_RECORDS];
    int m_NumRecords = 0;
    int m_State = STATE_IDLE;
    std::shared_ptr<CHttpRequest> m_pRequest;
    char m_aCurrentMap[64] = {0};
    int64_t m_LastRequestTime = 0;
    bool m_PendingChatDisplay = false;
    int m_RequestedCount = 10;

    void RequestMapTimes(const char *pMapName);
    void ParseResponse(const char *pJson);
    bool HasValidData() const { return m_State == STATE_DONE && m_NumRecords > 0; }
    void ShowTopInChat(int Count);
    void FormatTime(char *pBuf, int BufSize, const char *pTimeString);
};

#endif // GAME_CLIENT_COMPONENTS_TCLIENT_RAVENTOP_H
