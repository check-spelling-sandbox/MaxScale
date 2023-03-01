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

#pragma once

#include "postgresprotocol.hh"
#include <maxscale/protocol2.hh>
#include <maxscale/session.hh>

class PgClientConnection : public mxs::ClientConnectionBase
{
public:
    PgClientConnection(MXS_SESSION* pSession, mxs::Component* pComponent);

    // DCBHandler
    void ready_for_reading(DCB* dcb) override;
    void write_ready(DCB* dcb) override;
    void error(DCB* dcb) override;
    void hangup(DCB* dcb) override;

    // mxs::ProtocolConnection
    bool write(GWBUF&& buffer) override;

    // mxs::ClientConnection
    bool init_connection() override;
    void finish_connection() override;
    bool clientReply(GWBUF&& buffer, mxs::ReplyRoute& down, const mxs::Reply& reply) override;
    bool safe_to_restart() const override;

    // mxs::ClientConnectionBase
    size_t sizeof_buffers() const override;

private:
    MXS_SESSION*    m_session;
    mxs::Component* m_down;
};