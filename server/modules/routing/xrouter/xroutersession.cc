/*
 * Copyright (c) 2023 MariaDB plc
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-02-21
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
#include "xrouter.hh"
#include "xroutersession.hh"

#include <maxscale/modutil.hh>

// static
std::string_view XRouterSession::state_to_str(State state)
{
    switch (state)
    {
    case State::INIT:
        return "INIT";

    case State::IDLE:
        return "IDLE";

    case State::SOLO:
        return "SOLO";

    case State::WAIT_SOLO:
        return "WAIT_SOLO";

    case State::LOAD_DATA:
        return "LOAD_DATA";

    case State::LOCK_MAIN:
        return "LOCK_MAIN";

    case State::UNLOCK_MAIN:
        return "UNLOCK_MAIN";

    case State::MAIN:
        return "MAIN";

    case State::WAIT_MAIN:
        return "WAIT_MAIN";

    case State::WAIT_SECONDARY:
        return "WAIT_SECONDARY";
    }

    mxb_assert(!true);
    return "UNKNOWN";
}

std::string_view XRouterSession::state_str() const
{
    return state_to_str(m_state);
}

XRouterSession::XRouterSession(MXS_SESSION* session, XRouter& router, SBackends backends,
                               XRouter::Config::ValueRef config)
    : RouterSession(session)
    , m_router(router)
    , m_backends(std::move(backends))
    , m_main(m_backends[0].get())
    , m_solo(m_backends[rand() % m_backends.size()].get())
    , m_config(std::move(config))
{
    for (auto& b : m_backends)
    {
        mxb_assert(b->in_use());
        send_query(b.get(), b.get() == m_main ? m_config->main_sql : m_config->secondary_sql);
    }
}

bool XRouterSession::routeQuery(GWBUF&& packet)
{
    if (!m_main->in_use() || !m_solo->in_use())
    {
        MXB_SINFO("Main node or the single-target node is no longer in use, closing session.");
        return false;
    }

    bool ok = true;

    switch (m_state)
    {
    case State::IDLE:
        if (!check_node_status())
        {
            ok = false;
        }
        else if (is_multi_node(packet))
        {
            // Send the lock query to the main node before doing the DDL. This way the operations are
            // serialized with respect to the main node.
            MXB_SINFO("Multi-node command, locking main node");
            m_state = State::LOCK_MAIN;
            ok = send_query(m_main, m_config->lock_sql);
            m_queue.push_back(std::move(packet));
        }
        else
        {
            // Normal single-node query (DML) that does not need to be sent to the secondary nodes.
            m_state = State::SOLO;
            ok = route_solo(std::move(packet));
        }
        break;

    case State::SOLO:
        // More packets that belong to the single-node command. Keep routing them until we get one that will
        // generate a response.
        ok = route_solo(std::move(packet));
        break;

    case State::LOAD_DATA:
        // Client is uploading data, keep routing it to the solo node until the server responds.
        ok = route_to_one(m_solo, std::move(packet), mxs::Backend::NO_RESPONSE);
        break;

    case State::MAIN:
        // More packets that belong to the multi-node command. Keep routing them until we get one that will
        // generate a response.
        ok = route_main(std::move(packet));
        break;

    case State::LOCK_MAIN:
    case State::UNLOCK_MAIN:
    case State::INIT:
    case State::WAIT_SOLO:
    case State::WAIT_MAIN:
    case State::WAIT_SECONDARY:
        MXB_SINFO("Queuing: " << describe(packet));
        m_queue.push_back(std::move(packet));
        break;
    }

    return ok;
}

bool XRouterSession::route_solo(GWBUF&& packet)
{
    auto type = mxs::Backend::NO_RESPONSE;

    if (protocol_data()->will_respond(packet))
    {
        type = mxs::Backend::EXPECT_RESPONSE;
        m_state = State::WAIT_SOLO;
    }

    return route_to_one(m_solo, std::move(packet), type);
}

bool XRouterSession::route_main(GWBUF&& packet)
{
    auto type = mxs::Backend::NO_RESPONSE;

    if (protocol_data()->will_respond(packet))
    {
        type = mxs::Backend::IGNORE_RESPONSE;
        m_state = State::WAIT_MAIN;
    }

    m_packets.push_back(packet.shallow_clone());
    return route_to_one(m_main, std::move(packet), type);
}

bool XRouterSession::route_secondary()
{
    bool ok = true;
    MXB_SINFO("Routing to secondary backends");

    for (auto& b : m_backends)
    {
        if (b->in_use() && b.get() != m_main)
        {
            for (const auto& packet : m_packets)
            {
                auto type = protocol_data()->will_respond(packet) ?
                    mxs::Backend::IGNORE_RESPONSE : mxs::Backend::NO_RESPONSE;

                if (!route_to_one(b.get(), packet.shallow_clone(), type))
                {
                    ok = false;
                }
            }
        }
    }

    return ok;
}

bool XRouterSession::route_to_one(mxs::Backend* backend, GWBUF&& packet, mxs::Backend::response_type type)
{
    MXB_SINFO("Route to '" << backend->name() << "': " << describe(packet));
    mxb_assert(backend->in_use());
    return backend->write(std::move(packet), type);
}

bool XRouterSession::clientReply(GWBUF&& packet, const mxs::ReplyRoute& down, const mxs::Reply& reply)
{
    mxs::Backend* backend = static_cast<mxs::Backend*>(down.back()->get_userdata());
    bool rv = true;
    bool route = backend->is_expected_response();
    bool complete = reply.is_complete();

    if (complete)
    {
        backend->ack_write();
        MXB_SINFO("Reply complete from " << backend->name() << ". " << reply.describe());
    }
    else
    {
        MXB_SINFO("Partial reply from " << backend->name());
    }

    switch (m_state)
    {
    case State::INIT:
        if (all_backends_idle())
        {
            // All initialization queries complete, proceed with normal routing
            m_state = State::IDLE;
        }
        break;

    case State::SOLO:
        // This might be an error condition in MaxScale but technically it is possible for the server to
        // send a partial response before we expect it.
        mxb_assert_message(!complete, "Result should not be complete");
        [[fallthrough]];

    case State::LOAD_DATA:
    case State::WAIT_SOLO:
        if (complete)
        {
            // We just routed the final response to the query, route queued queries
            mxb_assert(route);
            mxb_assert(all_backends_idle());
            m_state = State::IDLE;
        }
        else if (reply.state() == mxs::ReplyState::LOAD_DATA)
        {
            MXB_SINFO("Data load starting, waiting for more data from the client.");

            // It's possible that the current state is already LOAD_DATA. In this case the client executed a
            // query starts multiple data loads. For example, in MariaDB multiple LOAD DATA LOCAL INFILE
            // commands separated by a semicolons would result in this.
            m_state = State::LOAD_DATA;
            rv = route_queued();
        }
        break;

    case State::LOCK_MAIN:
        if (complete)
        {
            MXB_SINFO("Main node locked, routing query to main node.");
            mxb_assert(!route);
            m_state = State::MAIN;
            rv = route_queued();
        }
        break;

    case State::UNLOCK_MAIN:
        if (complete)
        {
            MXB_SINFO("Main node unlocked, returning to normal routing.");
            mxb_assert(!route);
            m_state = State::IDLE;
        }
        break;

    case State::MAIN:
        // This might also be an error condition in MaxScale but we should still handle it.
        mxb_assert_message(!complete, "Result should not be complete");
        [[fallthrough]];

    case State::WAIT_MAIN:
        mxb_assert(!route);
        m_response.append(packet);
        packet.clear();

        if (complete)
        {
            mxb_assert(all_backends_idle());

            if (reply.error())
            {
                // The command failed, don't propagate the change
                MXB_SINFO("Multi-node command failed: " << reply.describe());
                route = true;
                packet = finish_multinode();
            }
            else
            {
                m_state = State::WAIT_SECONDARY;
                rv = route_secondary();
            }
        }
        break;

    case State::WAIT_SECONDARY:
        mxb_assert_message(!route, "No response expected from '%s'", backend->name());
        mxb_assert_message(backend != m_main, "Main backend should not respond");
        mxb_assert_message(m_main->is_idle(), "Main backend should be idle");

        if (complete)
        {
            if (reply.error())
            {
                MXB_SINFO("Command failed on '" << backend->name() << "': " << reply.describe());
                fence_bad_node(backend);
            }

            if (all_backends_idle())
            {
                // All backends have responded with something, clear out the packets and route the response.
                MXB_SINFO("Multi-node command complete");
                route = true;
                packet = finish_multinode();
            }
        }
        break;

    default:
        MXB_SWARNING("Unexpected response: " << reply.describe());
        m_pSession->kill();
        mxb_assert(!true);
        rv = false;
        break;
    }

    if (rv && route)
    {
        mxb_assert(packet);
        rv = mxs::RouterSession::clientReply(std::move(packet), down, reply);
    }

    if (rv && complete && m_state == State::IDLE)
    {
        rv = route_queued();
    }

    return rv;
}

bool XRouterSession::handleError(mxs::ErrorType type, const std::string& message,
                                 mxs::Endpoint* pProblem, const mxs::Reply& reply)
{
    mxs::Backend* backend = static_cast<mxs::Backend*>(pProblem->get_userdata());

    if (backend != m_main)
    {
        fence_bad_node(backend);
    }

    return false;
}

bool XRouterSession::route_queued()
{
    bool ok = true;
    bool again = true;

    while (!m_queue.empty() && ok && again)
    {
        ok = routeQuery(std::move(m_queue.front()));
        m_queue.pop_front();

        switch (m_state)
        {
        case State::UNLOCK_MAIN:
        case State::LOCK_MAIN:
        case State::WAIT_SOLO:
        case State::WAIT_MAIN:
        case State::WAIT_SECONDARY:
            again = false;
            break;

        default:
            break;
        }
    }

    if (!ok)
    {
        MXB_SINFO("Failed to route queued queries");
        m_pSession->kill();
    }

    return ok;
}

bool XRouterSession::all_backends_idle() const
{
    return std::all_of(m_backends.begin(), m_backends.end(), [](const auto& b){
        return b->is_idle();
    });
}

std::string XRouterSession::describe(const GWBUF& buffer)
{
    return m_pSession->protocol()->describe(buffer);
}

bool XRouterSession::send_query(mxs::Backend* backend, std::string_view sql)
{
    return route_to_one(backend, m_pSession->protocol()->make_query(sql), mxs::Backend::IGNORE_RESPONSE);
}

void XRouterSession::fence_bad_node(mxs::Backend* backend)
{
    if (!backend->target()->is_in_maint())
    {
        auto servers = m_router.service().reachable_servers();

        if (auto it = std::find(servers.begin(), servers.end(), backend->target()); it != servers.end())
        {
            SERVER* srv = *it;
            MXB_SWARNING("Server '" << srv->name() << "' has failed. "
                                    << "The node has been excluded from routing and "
                                    << "is now in maintenance mode.");
            srv->set_maintenance();
        }
    }

    backend->close(mxs::Backend::CLOSE_FATAL);
}

bool XRouterSession::check_node_status()
{
    for (auto& b : m_backends)
    {
        if (b->in_use() && !b->can_connect())
        {
            b->close();
        }
    }

    return m_main->in_use() && m_solo->in_use();
}

GWBUF XRouterSession::finish_multinode()
{
    GWBUF packet = std::move(m_response);
    m_response.clear();
    m_packets.clear();
    m_state = State::UNLOCK_MAIN;
    MXB_SINFO("Unlocking main backend.");

    if (!send_query(m_main, m_config->unlock_sql))
    {
        MXB_SINFO("Failed to unlock main backend, next query will close the session.");
        m_main->close(mxs::Backend::CLOSE_FATAL);
    }

    return packet;
}

bool XRouterSession::is_multi_node(GWBUF& buffer) const
{
    using namespace mxs::sql;
    bool is_multi = false;

    if (!mxs::Parser::type_mask_contains(parser().get_type_mask(buffer), TYPE_CREATE_TMP_TABLE))
    {
        switch (parser().get_operation(buffer))
        {
        // TODO: Update with the parser changes when merging
        case OP_CREATE:
        case OP_CREATE_TABLE:
        case OP_DROP:
        case OP_DROP_TABLE:
        case OP_ALTER:
        case OP_ALTER_TABLE:
        case OP_GRANT:
        case OP_REVOKE:
            is_multi = true;
            break;

        default:
            break;
        }
    }

    return is_multi;
}
