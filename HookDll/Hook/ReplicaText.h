#pragma once

#include <string>
#include <vector>

namespace iagd {

/// <summary>
/// One stored tooltip line, as the game's own renderer produced it.
///
/// The type identifies the line's role, and the text carries "^X" colour markers. Both are
/// stored verbatim in ReplicaItemRow, so this is the game's formatting rather than
/// anything reconstructed.
/// </summary>
struct ReplicaRow {
    int type = 0;
    std::string text;
};

/// A stretch of text sharing one colour. The code is the letter that followed '^',
/// or 0 for text before any marker.
struct TextRun {
    std::string text;
    char code = 0;
};

/// <summary>
/// The role a row plays, ported from WebUI/src/components/Item/replicaSections.ts.
///
/// The same visual role is numbered differently depending on nesting (item / component /
/// granted skill / set), which is why each of these is a set of numbers rather than one.
/// </summary>
class ReplicaSections {
public:
    static bool IsBlank(const ReplicaRow& row);
    static bool IsSetName(const ReplicaRow& row);
    static bool IsSectionStart(const ReplicaRow& row);
    static bool IsAfterSetInfo(const ReplicaRow& row);

    /// <summary>
    /// A header only opens a granted skill when a skill name follows it: the game reuses
    /// the header types for pet bonuses and pet abilities ("Bonus to All Pets", "Crab
    /// Spirit Abilities:"). Keyed off the row types rather than the header text, which is
    /// translated.
    /// </summary>
    static bool IsGrantedSkillHeader(const std::vector<ReplicaRow>& rows, size_t index);
};

/// <summary>
/// Splits a stored row's text into coloured runs.
///
/// The game embeds "^X" markers, where X selects a colour: E for a stat label, H for a
/// value inside a stat, W and S for weapon headers, Z for a skill name, and others. A
/// marker applies from where it appears until the next one.
///
/// The markers themselves are never part of the output. An unrecognised code is kept on
/// the run so the renderer can fall back to the default colour, rather than being shown to
/// the player as literal text.
/// </summary>
class ReplicaText {
public:
    static std::vector<TextRun> Parse(const std::string& text);

    /// The text with every marker removed and no styling, for searching and for logging.
    static std::string StripCodes(const std::string& text);
};

}  // namespace iagd
