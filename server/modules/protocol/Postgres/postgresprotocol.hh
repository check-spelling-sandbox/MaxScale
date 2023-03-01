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

#define MXB_MODULE_NAME "postgresprotocol"

#include <maxscale/ccdefs.hh>
#include <maxbase/assert.hh>
#include <maxscale/log.hh>

namespace postgres
{
//
// Message types: https://www.postgresql.org/docs/current/protocol-message-formats.html
//

// Backend messages
enum BackendCommand : uint8_t
{
    // The Authentication message is a "message class" that covers multiple message types. The main type is
    // the AuthenticationOk message that signals the client that authentication was successful.
    AUTHENTICATION = 'R',

    BACKEND_KEY_DATA           = 'K',   // BackendKeyData
    BIND_COMPLETE              = '2',   // BindComplete
    CLOSE_COMPLETE             = '3',   // CloseComplete
    COMMAND_COMPLETE           = 'C',   // CommandComplete
    COPY_BOTH_RESPONSE         = 'W',   // CopyBothResponse, only for streaming replication
    COPY_IN_RESPONSE           = 'G',   // CopyInResponse
    COPY_OUT_RESPONSE          = 'H',   // CopyOutResponse
    DATA_ROW                   = 'D',   // DataRow
    EMPTY_QUERY_RESPONSE       = 'I',   // EmptyQueryResponse
    ERROR_RESPONSE             = 'E',   // ErrorResponse
    NEGOTIATE_PROTOCOL_VERSION = 'v',   // NegotiateProtocolVersion
    FUNCTION_CALL_RESPONSE     = 'V',   // FunctionCallResponse
    NO_DATA                    = 'n',   // NoData
    NOTICE_RESPONSE            = 'N',   // NoticeResponse
    NOTIFICATION_RESPONSE      = 'A',   // NotificationResponse
    PARAMETER_DESCRIPTION      = 't',   // ParameterDescription
    PARAMETER_STATUS           = 'S',   // ParameterStatus
    PARSE_COMPLETE             = '1',   // ParseComplete
    PORTAL_SUSPENDED           = 's',   // PortalSuspended
    READY_FOR_QUERY            = 'Z',   // ReadyForQuery
    ROW_DESCRIPTION            = 'T',   // RowDescription
};

// Client messages
enum ClientCommand : uint8_t
{
    BIND                  = 'B',// Bind
    CLOSE                 = 'C',// Close
    COPY_FAIL             = 'f',// CopyFail
    DESCRIBE              = 'D',// Describe
    EXECUTE               = 'E',// Execute
    FLUSH                 = 'F',// Flush
    GSS_RESPONSE          = 'p',// GSSResponse
    PARSE                 = 'P',// Parse
    PASSWORD_MESSAGE      = 'p',// PasswordMessage
    QUERY                 = 'Q',// Query
    SASL_INITIAL_RESPONSE = 'p',// SASLInitialResponse
    SASL_RESPONSE         = 'p',// SASLResponse
    SYNC                  = 'S',// Sync
    TERMINATE             = 'X',// Terminate
};

// Messages that are sent by both clients and backends
enum BidirectionalCommand : uint8_t
{
    COPY_DATA = 'd',    // CopyData
    COPY_DONE = 'c',    // CopyDone
};

// A connection can also send a StartupMessage as the first command. The command consists of:
//
//   Int32 -  The length of the message.
//   Int32 -  The protocol version.
//   String[]- The rest of the packet consists of null-terminated strings.
//
// In addition to a normal StartupMessage, the following special commands that look like
// a StartupMessage can be sent by the client:
//
// SSLRequest - StartupMessage with 80877103 as the version. Sent instead of the normal handshake if the
//              connection is encrypted.
//
// CancelRequest - StartupMessage with 80877102 as the version. This is a request to kill the connection.
//                 Instead of the string data, it contains two Int32 values that define the process ID and the
//                 secret key.
//
// GSSENCRequest - StartupMessage with 80877104 as the version. GSSAPI encryption request.
}