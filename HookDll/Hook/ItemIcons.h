#pragma once

#include <map>
#include <string>
#include <vector>

namespace iagd {

class SqliteDb;

/// <summary>
/// Works out which image represents an item, ported from
/// IAGrim/Database/DAO/DatabaseItemStatDaoImpl.MapItemBitmaps.
///
/// A record can carry several candidate bitmap stats, so they are scored and the highest
/// wins. The scores are the client's, and must stay identical or the overlay and the
/// client will show different pictures for the same item.
///
/// The images themselves are the ones the client already extracted from the game into its
/// storage folder; the overlay never touches the game's asset archives.
/// </summary>
class ItemIcons {
public:
    /// <summary>
    /// Resolves each record to a bitmap path as stored in the database, e.g.
    /// "items/gearrelics/tier3/tier3_relic_22.tex". Records with no bitmap stat are absent
    /// from the result rather than mapped to a placeholder, so the caller can tell the
    /// difference between "no icon" and "this icon".
    /// </summary>
    static std::map<std::string, std::string> Resolve(SqliteDb& db, const std::vector<std::string>& records);

    /// <summary>
    /// Turns a stored bitmap path into the file name the client extracted it as: the last
    /// path segment with ".png" appended, so "items/.../tier3_relic_22.tex" becomes
    /// "tier3_relic_22.tex.png".
    /// </summary>
    static std::string ToImageFileName(const std::string& bitmapPath);

    /// The score for one bitmap stat name, or -1 if it is not a bitmap stat at all.
    static int ScoreFor(const std::string& statName);
};

}  // namespace iagd
