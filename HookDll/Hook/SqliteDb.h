#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>

struct sqlite3;
struct sqlite3_stmt;

namespace iagd {

/// <summary>
/// A read-only SQLite connection to the Item Assistant client's database.
///
/// Read-only is enforced at the connection, not by convention: the client is the only
/// writer, and a bug here must fail rather than corrupt the player's collection.
///
/// Deliberately free of any dependency on the game or on the hook's logging, so the
/// search can be built and exercised outside the game against a copy of a real database.
/// That is how the ported query is checked against the C# original.
/// </summary>
class SqliteDb {
public:
    SqliteDb();
    ~SqliteDb();

    SqliteDb(const SqliteDb&) = delete;
    SqliteDb& operator=(const SqliteDb&) = delete;

    /// Opens read-only. Returns false and leaves the object unusable if the file is
    /// missing or is not a database; the caller reports that as "no items available".
    bool OpenReadOnly(const std::string& utf8Path);
    void Close();
    bool IsOpen() const { return m_db != nullptr; }

    /// Last error text from SQLite, for logging.
    const std::string& LastError() const { return m_lastError; }

    /// <summary>
    /// PRAGMA data_version. Changes when another connection commits, which is how the
    /// overlay notices that the client has written to the database without polling the
    /// file or re-running the search.
    /// </summary>
    bool ReadDataVersion(int& outVersion);

    sqlite3* Handle() const { return m_db; }

private:
    sqlite3* m_db;
    std::string m_lastError;
};

/// <summary>
/// One prepared statement with named parameters, including list parameters.
///
/// SQLite has no list binding, so ":ids" is expanded into "?,?,?" before preparing and the
/// values are bound positionally. NHibernate does the same thing on the C# side, which is
/// what lets the ported SQL keep the ":name" placeholders of the original verbatim --
/// the less the two texts differ, the less room there is for them to drift apart.
///
/// Parameters must be supplied before Prepare, because the expansion changes the SQL.
/// </summary>
class SqliteQuery {
public:
    explicit SqliteQuery(SqliteDb& db);
    ~SqliteQuery();

    SqliteQuery(const SqliteQuery&) = delete;
    SqliteQuery& operator=(const SqliteQuery&) = delete;

    /// Names are given without the leading colon.
    void SetParam(const std::string& name, const std::string& value);
    void SetParam(const std::string& name, int64_t value);
    void SetParam(const std::string& name, double value);
    void SetParamList(const std::string& name, const std::vector<std::string>& values);

    /// Expands list parameters, prepares, and binds. False on failure; see LastError.
    bool Prepare(const std::string& sql);

    /// True while a row is available. False at the end, or on error.
    bool Step();

    int ColumnCount() const;
    std::string ColumnName(int index) const;
    bool IsNull(int index) const;
    std::string GetText(int index) const;
    int64_t GetInt64(int index) const;
    double GetDouble(int index) const;

    const std::string& LastError() const { return m_lastError; }

    /// The SQL actually handed to SQLite, after list expansion. Used by the offline
    /// comparison against the C# query, and worth logging when a query misbehaves.
    const std::string& ExpandedSql() const { return m_expandedSql; }

private:
    enum class ValueKind { Text, Int, Real };

    struct Value {
        ValueKind kind = ValueKind::Text;
        std::string text;
        int64_t integer = 0;
        double real = 0.0;
    };

    /// Rewrites ":name" occurrences into positional "?" placeholders, recording the order
    /// so values can be bound by position afterwards.
    bool Expand(const std::string& sql, std::vector<Value>& outOrdered);

    SqliteDb& m_db;
    sqlite3_stmt* m_stmt;
    std::string m_expandedSql;
    std::string m_lastError;

    std::map<std::string, Value> m_scalars;
    std::map<std::string, std::vector<std::string>> m_lists;
};

}  // namespace iagd
