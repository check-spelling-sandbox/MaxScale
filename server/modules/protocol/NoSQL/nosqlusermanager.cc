/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2025-11-19
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

#include "nosqlusermanager.hh"
#include <uuid/uuid.h>
#include <map>
#include <maxscale/paths.hh>
#include <maxscale/utils.hh>

using namespace std;

namespace
{

const int SCHEMA_VERSION = 1;

//
// User Database
//
static const char SQL_CREATE[] =
    "CREATE TABLE IF NOT EXISTS accounts "
    "(db_user TEXT UNIQUE, db TEXT, user TEXT, pwd TEXT, uuid TEXT, salt_b64 TEXT, "
    "mechanisms TEXT, roles TEXT)";

static const char SQL_INSERT_HEAD[] =
    "INSERT INTO accounts (db_user, db, user, pwd, uuid, salt_b64, mechanisms, roles) VALUES ";

static const char SQL_DELETE_HEAD[] =
    "DELETE FROM accounts WHERE db_user = ";

static const char SQL_SELECT_ONE_HEAD[] =
    "SELECT db_user, db, user, pwd, uuid, salt_b64, mechanisms, roles FROM accounts WHERE db_user = ";

static const char SQL_SELECT_ALL_USERS[] =
    "SELECT db_user, db, user, pwd, uuid, salt_b64, mechanisms, roles FROM accounts";

static const char SQL_SELECT_ALL_DB_USERS_HEAD[] =
    "SELECT db_user, db, user, pwd, uuid, salt_b64, mechanisms, roles FROM accounts WHERE db = ";

static const char SQL_SELECT_SOME_DB_USERS_HEAD[] =
    "SELECT db_user, db, user, pwd, uuid, salt_b64, mechanisms, roles FROM accounts WHERE ";

int select_cb(void* pData, int nColumns, char** pzColumn, char** pzNames)
{
    mxb_assert(nColumns == 8);

    auto* pInfos = static_cast<vector<nosql::UserManager::UserInfo>*>(pData);

    nosql::UserManager::UserInfo info;
    info.db_user = pzColumn[0];
    info.db = pzColumn[1];
    info.user = pzColumn[2];
    info.pwd = pzColumn[3];
    info.uuid = pzColumn[4];
    info.salt_b64 = pzColumn[5];
    info.salt = mxs::from_base64(info.salt_b64);

    bool ok = false;

    vector<nosql::scram::Mechanism> mechanisms;
    if (nosql::scram::from_json(pzColumn[6], &mechanisms))
    {
        info.mechanisms = std::move(mechanisms);

        vector<nosql::role::Role> roles;
        if (nosql::role::from_json(pzColumn[7], &roles))
        {
            info.roles = std::move(roles);

            pInfos->push_back(info);
            ok = true;
        }
        else
        {
            MXS_ERROR("The 'roles' value of '%s' is not valid.", info.db_user.c_str());
        }
    }
    else
    {
        MXS_ERROR("The 'mechanisms' value of '%s' is not valid.", info.db_user.c_str());
    }

    if (!ok)
    {
        MXS_WARNING("Ignoring user '%s'.", info.db_user.c_str());
    }

    return 0;
}

bool create_schema(sqlite3* pDb)
{
    char* pError = nullptr;
    int rv = sqlite3_exec(pDb, SQL_CREATE, nullptr, nullptr, &pError);

    if (rv != SQLITE_OK)
    {
        MXS_ERROR("Could not initialize sqlite3 database: %s", pError ? pError : "Unknown error");
        sqlite3_free(pError);
    }

    return rv == SQLITE_OK;
}

sqlite3* open_or_create_db(const std::string& path)
{
    sqlite3* pDb = nullptr;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_CREATE;
    int rv = sqlite3_open_v2(path.c_str(), &pDb, flags, nullptr);

    if (rv == SQLITE_OK)
    {
        if (create_schema(pDb))
        {
            MXS_NOTICE("sqlite3 database %s open/created and initialized.", path.c_str());
        }
        else
        {
            MXS_ERROR("Could not create schema in sqlite3 database %s.", path.c_str());

            if (unlink(path.c_str()) != 0)
            {
                MXS_ERROR("Failed to delete database %s that could not be properly "
                          "initialized. It should be deleted manually.", path.c_str());
                sqlite3_close_v2(pDb);
                pDb = nullptr;
            }
        }
    }
    else
    {
        if (pDb)
        {
            // Memory allocation failure is explained by the caller. Don't close the handle, as the
            // caller will still use it even if open failed!!
            MXS_ERROR("Opening/creating the sqlite3 database %s failed: %s",
                      path.c_str(), sqlite3_errmsg(pDb));
        }
        MXS_ERROR("Could not open sqlite3 database for storing user information.");
    }

    return pDb;
}


}


namespace nosql
{

namespace role
{

namespace
{

const map<string, Id> roles =
{
    { "dbAdmin",   Id::DB_ADMIN },
    { "read",      Id::READ },
    { "readWrite", Id::READ_WRITE }
};

}

}

string role::to_string(Id id)
{
    for (const auto& kv : roles)
    {
        if (id == kv.second)
        {
            return kv.first;
        }
    }

    mxb_assert(!true);

    return "unknown";
}

bool role::from_string(const string& key, Id* pValue)
{
    auto it = roles.find(key);

    bool found = (it != roles.end());

    if (found)
    {
        *pValue = it->second;
    }

    return found;
}

string role::to_json(const Role& role)
{
    ostringstream ss;

    ss << "{"
       << "\"db\": \"" << role.db << "\", "
       << "\"role\": \"" << to_string(role.id) << "\""
       << "}";

    return ss.str();
}

bool role::from_json(const mxb::Json& json, Role* pRole)
{
    bool rv = false;

    if (json.type() == mxb::Json::Type::OBJECT)
    {
        string db;
        if (json.try_get_string("db", &db))
        {
            string role_name;
            if (json.try_get_string("role", &role_name))
            {
                Id id;
                rv = from_string(role_name, &id);

                if (rv)
                {
                    pRole->db = std::move(db);
                    pRole->id = id;
                }
            }
        }
    }

    return rv;
}

bool role::from_json(const string& s, Role* pRole)
{
    bool rv = false;

    mxb::Json json;

    if (json.load_string(s))
    {
        rv = from_json(json, pRole);
    }

    return rv;
}

string role::to_json(const std::vector<role::Role>& roles)
{
    ostringstream ss;

    ss << "[";

    auto it = roles.begin();
    for (; it != roles.end(); ++it)
    {
        const auto& role = *it;

        if (it != roles.begin())
        {
            ss << ", ";
        }

        ss << "{"
           << "\"db\": \"" << role.db << "\", "
           << "\"role\": \"" << to_string(role.id) << "\""
           << "}";
    }

    ss << "]";

    return ss.str();
}

bool role::from_json(const string& s, std::vector<role::Role>* pRoles)
{
    bool rv = false;

    mxb::Json json;

    if (json.load_string(s))
    {
        if (json.type() == mxb::Json::Type::ARRAY)
        {
            vector<Role> roles;

            auto elements = json.get_array_elems();

            rv = true;
            for (const auto& element : elements)
            {
                auto type = element.type();

                Role role;

                if (type == mxb::Json::Type::OBJECT)
                {
                    rv = from_json(element, &role);

                    if (!rv)
                    {
                        MXB_ERROR("'%s' is not a valid.role.",
                                  element.to_string(mxb::Json::Format::NORMAL).c_str());
                        break;
                    }
                }
                else
                {
                    MXB_ERROR("'%s' is a JSON array, but all elements are not objects.", s.c_str());
                    rv = false;
                    break;
                }

                roles.push_back(role);
            }

            if (rv)
            {
                pRoles->swap(roles);
            }
        }
        else
        {
            MXS_ERROR("'%s' is valid JSON, but not an array.", s.c_str());
        }
    }
    else
    {
        MXS_ERROR("'%s' is not valid JSON: %s", s.c_str(), json.error_msg().c_str());
    }

    return rv;
}

UserManager::UserManager(string path, sqlite3* pDb)
    : m_path(std::move(path))
    , m_db(*pDb)
{
}

UserManager::~UserManager()
{
    sqlite3_close_v2(&m_db);
}

//static
unique_ptr<UserManager> UserManager::create(const string& name)
{
    nosql::UserManager* pThis = nullptr;

    string path = mxs::datadir();

    path += "/nosqlprotocol/";
    path += name;

    if (mxs_mkdir_all(path.c_str(), 0744))
    {
        path += "/users-v";
        path += std::to_string(SCHEMA_VERSION);
        path += ".db";

        sqlite3* pDb = open_or_create_db(path);

        if (pDb)
        {
            pThis = new UserManager(std::move(path), pDb);
        }
        else
        {
            // The handle will be null, *only* if the opening fails due to a memory
            // allocation error.
            MXS_ALERT("sqlite3 memory allocation failed, nosqlprotocol cannot continue.");
        }
    }
    else
    {
        MXS_ERROR("Could not create the directory %s, needed by the listener %s "
                  "for storing nosqlprotocol user information.",
                  path.c_str(), name.c_str());
    }

    return unique_ptr<UserManager>(pThis);
}

bool UserManager::add_user(const string& db,
                           const string_view& user,
                           const string_view& pwd,
                           const string& salt_b64,
                           const vector<scram::Mechanism>& mechanisms,
                           const vector<role::Role>& roles)
{
    string db_user = db + "." + string(user.data(), user.length());

    uuid_t uuid;
    uuid_generate(uuid);

    char zUuid[UUID_STR_LEN];
    uuid_unparse(uuid, zUuid);

    ostringstream ss;
    ss << SQL_INSERT_HEAD << "('"
       << db_user << "', '"
       << db << "', '"
       << user << "', '"
       << pwd << "', '"
       << zUuid << "', '"
       << salt_b64 << "', '"
       << scram::to_json(mechanisms) << "', '"
       << role::to_json(roles)
       << "')";

    string sql = ss.str();

    char* pError = nullptr;
    int rv = sqlite3_exec(&m_db, sql.c_str(), nullptr, nullptr, &pError);

    if (rv != SQLITE_OK)
    {
        MXS_ERROR("Could not add user '%s' to local database: %s",
                  db_user.c_str(),
                  pError ? pError : "Unknown error");
        sqlite3_free(pError);
    }

    return rv == SQLITE_OK;
}

bool UserManager::remove_user(const string& db, const string& user)
{
    string db_user = db + "." + user;

    ostringstream ss;
    ss << SQL_DELETE_HEAD << "\"" << db_user << "\"";

    string sql = ss.str();

    char* pError = nullptr;
    int rv = sqlite3_exec(&m_db, sql.c_str(), nullptr, nullptr, &pError);

    if (rv != SQLITE_OK)
    {
        MXS_ERROR("Could not remove user '%s' from local database: %s",
                  user.c_str(),
                  pError ? pError : "Unknown error");
        sqlite3_free(pError);
    }

    return rv == SQLITE_OK;
}

bool UserManager::get_info(const string& db, const string& user, UserInfo* pInfo) const
{
    string db_user = db + "." + user;

    return get_info(db_user, pInfo);
}

bool UserManager::get_info(const string& db_user, UserInfo* pInfo) const
{
    ostringstream ss;
    ss << SQL_SELECT_ONE_HEAD << "\"" << db_user << "\"";

    string sql = ss.str();

    vector<UserInfo> infos;
    char* pError = nullptr;
    int rv = sqlite3_exec(&m_db, sql.c_str(), select_cb, &infos, &pError);

    if (rv != SQLITE_OK)
    {
        MXS_ERROR("Could not get data for user '%s' from local database: %s",
                  db_user.c_str(),
                  pError ? pError : "Unknown error");
        sqlite3_free(pError);
    }

    if (!infos.empty() && pInfo)
    {
        mxb_assert(infos.size() == 1);
        *pInfo = infos.front();
    }

    return !infos.empty();
}

bool UserManager::get_pwd(const string& db, const string& user, std::string* pPwd) const
{
    UserInfo info;
    bool rv = get_info(db, user, &info);

    if (rv)
    {
        *pPwd = info.pwd;
    }

    return rv;
}

bool UserManager::get_salt_b64(const string& db, const string& user, std::string* pSalt_b64) const
{
    UserInfo info;
    bool rv = get_info(db, user, &info);

    if (rv)
    {
        *pSalt_b64 = info.salt_b64;
    }

    return rv;
}

vector<UserManager::UserInfo> UserManager::get_infos() const
{
    vector<UserInfo> infos;
    char* pError = nullptr;
    int rv = sqlite3_exec(&m_db, SQL_SELECT_ALL_USERS, select_cb, &infos, &pError);

    if (rv != SQLITE_OK)
    {
        MXS_ERROR("Could not get user data from local database: %s",
                  pError ? pError : "Unknown error");
        sqlite3_free(pError);
    }

    return infos;
}

vector<UserManager::UserInfo> UserManager::get_infos(const std::string& db) const
{
    ostringstream ss;
    ss << SQL_SELECT_ALL_DB_USERS_HEAD << "\"" << db << "\"";

    string sql = ss.str();

    vector<UserInfo> infos;
    char* pError = nullptr;
    int rv = sqlite3_exec(&m_db, sql.c_str(), select_cb, &infos, &pError);

    if (rv != SQLITE_OK)
    {
        MXS_ERROR("Could not get user data from local database: %s",
                  pError ? pError : "Unknown error");
        sqlite3_free(pError);
    }

    return infos;
}

vector<UserManager::UserInfo> UserManager::get_infos(const vector<string>& db_users) const
{
    vector<UserInfo> infos;

    if (!db_users.empty())
    {
        ostringstream ss;
        ss << SQL_SELECT_SOME_DB_USERS_HEAD;

        auto it = db_users.begin();
        for (; it != db_users.end(); ++it)
        {
            if (it != db_users.begin())
            {
                ss << " OR ";
            }

            ss << "db_user = '" << *it << "'";
        }

        string sql = ss.str();

        char* pError = nullptr;
        int rv = sqlite3_exec(&m_db, sql.c_str(), select_cb, &infos, &pError);

        if (rv != SQLITE_OK)
        {
            MXS_ERROR("Could not get user data from local database: %s",
                      pError ? pError : "Unknown error");
            sqlite3_free(pError);
        }
    }

    return infos;
}

}
