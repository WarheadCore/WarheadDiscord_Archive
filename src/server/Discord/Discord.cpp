/*
 * This file is part of the WarheadCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Discord.h"
#include "AsyncCallbackMgr.h"
#include "AccountMgr.h"
#include "DatabaseEnv.h"
#include "DiscordBot.h"
#include "DiscordConfig.h"
#include "DiscordSession.h"
#include "DiscordSharedDefines.h"
#include "Errors.h"
#include "GameTime.h"
#include "Log.h"
#include "Opcodes.h"
#include "StopWatch.h"
#include "UpdateTime.h"
#include <boost/asio/ip/address.hpp>

std::atomic<bool> Discord::_stopEvent = false;
uint8 Discord::_exitCode = SHUTDOWN_EXIT_CODE;
uint32 Discord::_loopCounter = 0;

/// Discord constructor
Discord::Discord()
{
    _shutdownTimer = 0s;
    m_maxActiveSessionCount = 0;
    _sessionCount = 0;
    _maxSessionCount = 0;
    m_isClosed = false;
}

/// Discord destructor
Discord::~Discord()
{
    ///- Empty the kicked session set
    for (auto& [accountId, session] : _sessions)
        delete session;

    _sessions.clear();
}

/*static*/ Discord* Discord::instance()
{
    static Discord instance;
    return &instance;
}

bool Discord::IsClosed() const
{
    return m_isClosed;
}

void Discord::SetClosed(bool val)
{
    m_isClosed = val;
}

/// Find a session by its id
DiscordSession* Discord::FindSession(uint32 id) const
{
    auto const& itr = _sessions.find(id);
    if (itr != _sessions.end())
        return itr->second; // also can return nullptr for kicked session

    return nullptr;
}

/// Remove a given session
void Discord::KickSession(uint32 id)
{
    ///- Find the session, kick the user, but we can't delete session at this moment to prevent iterator invalidation
    auto const& itr = _sessions.find(id);

    if (itr != _sessions.end() && itr->second)
        itr->second->KickSession("KickSession", false);
}

void Discord::AddSession(DiscordSession* session)
{
    ASSERT(session);

    // kick existing session with same account (if any)
    // if character on old session is being loaded, then return
    KickSession(session->GetAccountId());

    auto const& old = _sessions.find(session->GetAccountId());
    if (old != _sessions.end())
    {
        delete old->second;
        _sessions.erase(session->GetAccountId());
    }

    _sessions.emplace(session->GetAccountId(), session);
    UpdateMaxSessionCounters();
    session->SendAuthResponse(DiscordAuthResponseCodes::Ok);    
}

/// Initialize config values
void Discord::LoadConfigSettings()
{
}

/// Initialize the Discord
void Discord::SetInitialDiscordSettings()
{
    ///- Server startup begin
    StopWatch sw;

    ///- Initialize the random number generator
    srand((unsigned int)GameTime::GetGameTime().count());

    ///- Initialize config settings
    LoadConfigSettings();

    GameTime::UpdateGameTimers();
    opcodeTable.Initialize();

    _scheduler.Schedule(30min, [](TaskContext context)
    {
        LOG_DEBUG("sql.driver", "Ping MySQL to keep connection alive");
        DiscordDatabase.KeepAlive();
        context.Repeat();
    });

    _scheduler.Schedule(15s, [this](TaskContext context)
    {
        LOG_INFO("time.diff", "> Update time diff. Last {} ms, Avg {} ms. Online {} sessions",
            sDiscordUpdateTime.GetLastUpdateTime().count(), sDiscordUpdateTime.GetAverageUpdateTime().count(), GetActiveSessionCount());
        context.Repeat(5min);
    });

    sAccountMgr->Initialize();

    // Start discord bot
    sDiscordBot->Start();
    sDiscordBot->Test();

    std::string startupDuration = Warhead::Time::ToTimeString(sw.Elapsed(), sw.GetOutCount());

    LOG_INFO("server.loading", "Server initialized in {}", startupDuration);
    LOG_INFO("server.loading", " ");
}

void Discord::Update(Milliseconds diff)
{
    ///- Update the game time and check for shutdown time
    _UpdateGameTime();

    sDiscordUpdateTime.Update(diff);

    {
        sAsyncCallbackMgr->ProcessReadyCallbacks();
    }

    {
        UpdateSessions();
    }

    sAccountMgr->Update();
    sDiscordBot->Update(diff);

    _scheduler.Update(diff);
}

/// Kick (and save) all players
void Discord::KickAll()
{
    for (auto const& [accID, session] : _sessions)
        session->KickSession("KickAll sessions");

    _sessions.clear();
}

/// Update the game time
void Discord::_UpdateGameTime()
{
    ///- update the time
    Seconds lastGameTime = GameTime::GetGameTime();
    GameTime::UpdateGameTimers();

    Seconds elapsed = GameTime::GetGameTime() - lastGameTime;

    ///- if there is a shutdown timer
    if (!IsStopped() && _shutdownTimer > 0s && elapsed > 0s)
    {
        ///- ... and it is overdue, stop the world (set m_stopEvent)
        if (_shutdownTimer <= elapsed)
        {
            _stopEvent = true; // exist code already set
        }
        ///- ... else decrease it and if necessary display a shutdown countdown to the users
        else
        {
            _shutdownTimer -= elapsed;

            ShutdownMsg();
        }
    }
}

/// Shutdown the server
void Discord::ShutdownServ(Seconds time, uint8 exitcode, const std::string_view reason)
{
    // ignore if server shutdown at next tick
    if (IsStopped())
        return;

    _exitCode = exitcode;

    if (time < 2s && GetActiveSessionCount())
    {
        // Set time to 5s for save all players
        time = 2s;
    }

    LOG_WARN("server", "> Time left until shutdown/restart: {}", Warhead::Time::ToTimeString(time));

    //sDiscord->SendDefaultMessage("Рестарт сервера", Warhead::StringFormat("Рестарт сервера через {}", strTime), DiscordMessageColor::Yellow);

    ///- If the shutdown time is 0, set m_stopEvent (except if shutdown is 'idle' with remaining sessions)
    if (time == 0s)
    {
        _stopEvent = true; // exist code already set
    }
    ///- Else set the shutdown timer and warn users
    else
    {
        _shutdownTimer = time;
        ShutdownMsg(true, reason);
    }
}

/// Display a shutdown message to the user(s)
void Discord::ShutdownMsg(bool show, const std::string_view reason)
{
    ///- Display a message every 12 hours, hours, 5 minutes, minute, 5 seconds and finally seconds
    if (show ||
            (_shutdownTimer < 5min && (_shutdownTimer % 15) == 0s) || // < 5 min; every 15 sec
            (_shutdownTimer < 15min && (_shutdownTimer % 1min) == 0s) || // < 15 min ; every 1 min
            (_shutdownTimer < 30min && (_shutdownTimer % 5min) == 0s) || // < 30 min ; every 5 min
            (_shutdownTimer < 12h && (_shutdownTimer % 1h) == 0s) || // < 12 h ; every 1 h
            (_shutdownTimer > 12h && (_shutdownTimer % 12h) == 0s)) // > 12 h ; every 12 h
    {
        std::string str = Warhead::Time::ToTimeString(_shutdownTimer);

        LOG_WARN("server", "Server is restart. Time to left '{}'. Reason '{}'", Warhead::Time::ToTimeString(_shutdownTimer), reason);
    }
}

/// Cancel a planned server shutdown
void Discord::ShutdownCancel()
{
    // nothing cancel or too later
    if (_shutdownTimer == 0s || _stopEvent)
        return;

    _shutdownTimer = 0s;
    _exitCode = SHUTDOWN_EXIT_CODE; // to default value

    LOG_WARN("server", "Server restart cancelled.");
}

void Discord::UpdateSessions()
{
    for (std::unordered_map<uint32, DiscordSession*>::const_iterator itr = _sessions.begin(), next; itr != _sessions.end(); itr = next)
    {
        next = itr;
        ++next;

        auto session = itr->second;

        if (!session->Update())
        {
            _sessions.erase(itr->first);
            delete session;
        }
    }
}

void Discord::UpdateMaxSessionCounters()
{
    m_maxActiveSessionCount = std::max(m_maxActiveSessionCount, _sessions.size());
}
