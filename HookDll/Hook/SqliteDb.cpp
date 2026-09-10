#include "SqliteDb.h"

#include "sqlite3.h"

#include <cctype>

namespace iagd {

SqliteDb::SqliteDb()
    : m_db(nullptr) {}

SqliteDb::~SqliteDb() {
    Close();
}

bool SqliteDb::OpenReadOnly(const std::string& utf8Path) {
    Close();

    // SQLITE_OPEN_READONLY without SQLITE_OPEN_CREATE: a missing file is an error rather
    // than an empty database silently appearing next to the client's real one.
    const int rc = sqlite3_open_v2(utf8Path.c_str(), &m_db, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        m_lastError = m_db ? sqlite3_errmsg(m_db) : "sqlite3_open_v2 failed";
        sqlite3_close(m_db);
        m_db = nullptr;
        return false;
    }

    // The client writes while this connection is open. Waiting briefly on a lock is much
    // better than surfacing SQLITE_BUSY to the player as "no items found".
    sqlite3_busy_timeout(m_db, 2000);
    return true;
}

void SqliteDb::Close() {
    if (m_db != nullptr) {
        sqlite3_close(m_db);
        m_db = nullptr;
    }
}

bool SqliteDb::ReadDataVersion(int& outVersion) {
    if (m_db == nullptr) {
        return false;
    }

    SqliteQuery q(*this);
    if (!q.Prepare("PRAGMA data_version")) {
        m_lastError = q.LastError();
        return false;
    }

    if (!q.Step()) {
        m_lastError = "PRAGMA data_version returned no row";
        return false;
    }

    outVersion = static_cast<int>(q.GetInt64(0));
    return true;
}

SqliteQuery::SqliteQuery(SqliteDb& db)
    : m_db(db), m_stmt(nullptr) {}

SqliteQuery::~SqliteQuery() {
    if (m_stmt != nullptr) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }
}

void SqliteQuery::SetParam(const std::string& name, const std::string& value) {
    Value v;
    v.kind = ValueKind::Text;
    v.text = value;
    m_scalars[name] = v;
}

void SqliteQuery::SetParam(const std::string& name, int64_t value) {
    Value v;
    v.kind = ValueKind::Int;
    v.integer = value;
    m_scalars[name] = v;
}

void SqliteQuery::SetParam(const std::string& name, double value) {
    Value v;
    v.kind = ValueKind::Real;
    v.real = value;
    m_scalars[name] = v;
}

void SqliteQuery::SetParamList(const std::string& name, const std::vector<std::string>& values) {
    m_lists[name] = values;
}

/// Parameter names run to the first character that cannot be part of one. Kept to the
/// characters the ported queries actually use (letters, digits, underscore); anything else
/// ends the name, so ":mod)" and ":minlevel," behave.
static bool IsNameChar(char c) {
    return (c == '_') || (std::isalnum(static_cast<unsigned char>(c)) != 0);
}

bool SqliteQuery::Expand(const std::string& sql, std::vector<Value>& outOrdered) {
    m_expandedSql.clear();
    m_expandedSql.reserve(sql.size() + 64);

    size_t i = 0;
    while (i < sql.size()) {
        const char c = sql[i];

        // Skip over string literals so a colon inside one is never treated as a parameter.
        if (c == '\'') {
            size_t j = i + 1;
            while (j < sql.size()) {
                if (sql[j] == '\'' && j + 1 < sql.size() && sql[j + 1] == '\'') {
                    j += 2;  // escaped quote
                    continue;
                }
                if (sql[j] == '\'') {
                    break;
                }
                j++;
            }
            if (j >= sql.size()) {
                m_lastError = "unterminated string literal in SQL";
                return false;
            }
            m_expandedSql.append(sql, i, j - i + 1);
            i = j + 1;
            continue;
        }

        if (c != ':') {
            m_expandedSql.push_back(c);
            i++;
            continue;
        }

        size_t nameStart = i + 1;
        size_t nameEnd = nameStart;
        while (nameEnd < sql.size() && IsNameChar(sql[nameEnd])) {
            nameEnd++;
        }

        if (nameEnd == nameStart) {
            // A bare colon, not a parameter.
            m_expandedSql.push_back(c);
            i++;
            continue;
        }

        const std::string name = sql.substr(nameStart, nameEnd - nameStart);

        const auto listIt = m_lists.find(name);
        if (listIt != m_lists.end()) {
            // An empty list would produce "IN ()", which is a syntax error. The callers
            // must not add a filter with no values; say so rather than emitting bad SQL.
            if (listIt->second.empty()) {
                m_lastError = "list parameter '" + name + "' is empty";
                return false;
            }

            for (size_t k = 0; k < listIt->second.size(); k++) {
                if (k > 0) {
                    m_expandedSql.push_back(',');
                }
                m_expandedSql.push_back('?');

                Value v;
                v.kind = ValueKind::Text;
                v.text = listIt->second[k];
                outOrdered.push_back(v);
            }

            i = nameEnd;
            continue;
        }

        const auto scalarIt = m_scalars.find(name);
        if (scalarIt != m_scalars.end()) {
            m_expandedSql.push_back('?');
            outOrdered.push_back(scalarIt->second);
            i = nameEnd;
            continue;
        }

        m_lastError = "no value supplied for parameter '" + name + "'";
        return false;
    }

    return true;
}

bool SqliteQuery::Prepare(const std::string& sql) {
    if (!m_db.IsOpen()) {
        m_lastError = "database is not open";
        return false;
    }

    if (m_stmt != nullptr) {
        sqlite3_finalize(m_stmt);
        m_stmt = nullptr;
    }

    std::vector<Value> ordered;
    if (!Expand(sql, ordered)) {
        return false;
    }

    const int rc = sqlite3_prepare_v2(m_db.Handle(), m_expandedSql.c_str(),
                                      static_cast<int>(m_expandedSql.size()), &m_stmt, nullptr);
    if (rc != SQLITE_OK || m_stmt == nullptr) {
        m_lastError = sqlite3_errmsg(m_db.Handle());
        m_stmt = nullptr;
        return false;
    }

    for (size_t k = 0; k < ordered.size(); k++) {
        const int index = static_cast<int>(k) + 1;  // SQLite binds from 1
        int bindRc = SQLITE_OK;

        switch (ordered[k].kind) {
        case ValueKind::Text:
            // SQLITE_TRANSIENT: the vector goes away when this function returns.
            bindRc = sqlite3_bind_text(m_stmt, index, ordered[k].text.c_str(),
                                       static_cast<int>(ordered[k].text.size()), SQLITE_TRANSIENT);
            break;
        case ValueKind::Int:
            bindRc = sqlite3_bind_int64(m_stmt, index, ordered[k].integer);
            break;
        case ValueKind::Real:
            bindRc = sqlite3_bind_double(m_stmt, index, ordered[k].real);
            break;
        }

        if (bindRc != SQLITE_OK) {
            m_lastError = sqlite3_errmsg(m_db.Handle());
            sqlite3_finalize(m_stmt);
            m_stmt = nullptr;
            return false;
        }
    }

    return true;
}

bool SqliteQuery::Step() {
    if (m_stmt == nullptr) {
        return false;
    }

    const int rc = sqlite3_step(m_stmt);
    if (rc == SQLITE_ROW) {
        return true;
    }

    if (rc != SQLITE_DONE) {
        m_lastError = sqlite3_errmsg(m_db.Handle());
    }
    return false;
}

int SqliteQuery::ColumnCount() const {
    return m_stmt ? sqlite3_column_count(m_stmt) : 0;
}

std::string SqliteQuery::ColumnName(int index) const {
    if (m_stmt == nullptr) {
        return std::string();
    }
    const char* name = sqlite3_column_name(m_stmt, index);
    return name ? std::string(name) : std::string();
}

bool SqliteQuery::IsNull(int index) const {
    return m_stmt == nullptr || sqlite3_column_type(m_stmt, index) == SQLITE_NULL;
}

std::string SqliteQuery::GetText(int index) const {
    if (m_stmt == nullptr) {
        return std::string();
    }

    const unsigned char* text = sqlite3_column_text(m_stmt, index);
    if (text == nullptr) {
        return std::string();
    }

    return std::string(reinterpret_cast<const char*>(text),
                       static_cast<size_t>(sqlite3_column_bytes(m_stmt, index)));
}

int64_t SqliteQuery::GetInt64(int index) const {
    return m_stmt ? sqlite3_column_int64(m_stmt, index) : 0;
}

double SqliteQuery::GetDouble(int index) const {
    return m_stmt ? sqlite3_column_double(m_stmt, index) : 0.0;
}

}  // namespace iagd
