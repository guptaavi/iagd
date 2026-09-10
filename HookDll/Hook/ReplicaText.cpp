#include "ReplicaText.h"

#include <algorithm>

namespace iagd {

namespace {

bool Contains(const int* values, size_t count, int type) {
    for (size_t i = 0; i < count; i++) {
        if (values[i] == type) {
            return true;
        }
    }
    return false;
}

/// An empty row, used by the game to separate sections.
const int kBlank[] = { 0, 1 };

/// "Granted Skills". 24 is also a pet header, so see IsGrantedSkillHeader.
const int kGrantedSkillHeader[] = { 24, 34, 36 };

/// "Counterblow (15% Chance on Block)"
const int kSkillName[] = { 37, 38 };

/// The set name, which is the first row of an item's set info.
const int kSetName[] = { 21 };

/// Rows that always begin a new section: requirements, set info, pet bonuses, completion
/// bonus and the ctrl hint. None occur inside a granted skill, so they bound a skill block.
const int kSectionStart[] = { 20, 21, 22, 23, 25, 35, 65, 67, 68, 70 };

/// Set info sits at the foot of an item, with only the requirements and the ctrl hint after it.
const int kAfterSetInfo[] = { 20, 35 };

}  // namespace

bool ReplicaSections::IsBlank(const ReplicaRow& row) {
    return Contains(kBlank, sizeof(kBlank) / sizeof(kBlank[0]), row.type);
}

bool ReplicaSections::IsSetName(const ReplicaRow& row) {
    return Contains(kSetName, sizeof(kSetName) / sizeof(kSetName[0]), row.type);
}

bool ReplicaSections::IsSectionStart(const ReplicaRow& row) {
    return Contains(kSectionStart, sizeof(kSectionStart) / sizeof(kSectionStart[0]), row.type);
}

bool ReplicaSections::IsAfterSetInfo(const ReplicaRow& row) {
    return Contains(kAfterSetInfo, sizeof(kAfterSetInfo) / sizeof(kAfterSetInfo[0]), row.type);
}

bool ReplicaSections::IsGrantedSkillHeader(const std::vector<ReplicaRow>& rows, size_t index) {
    if (index >= rows.size()) {
        return false;
    }

    if (!Contains(kGrantedSkillHeader, sizeof(kGrantedSkillHeader) / sizeof(kGrantedSkillHeader[0]),
                  rows[index].type)) {
        return false;
    }

    // The next non-blank row decides it. A pet-bonus header carries the same type but is
    // followed by stats rather than by a skill name.
    for (size_t i = index + 1; i < rows.size(); i++) {
        if (IsBlank(rows[i])) {
            continue;
        }
        return Contains(kSkillName, sizeof(kSkillName) / sizeof(kSkillName[0]), rows[i].type);
    }

    return false;
}

std::vector<TextRun> ReplicaText::Parse(const std::string& text) {
    std::vector<TextRun> runs;

    TextRun current;
    current.code = 0;

    size_t i = 0;
    while (i < text.size()) {
        if (text[i] != '^') {
            current.text.push_back(text[i]);
            i++;
            continue;
        }

        // A trailing '^' with nothing after it is not a marker. Kept as text rather than
        // silently dropped, so a genuinely malformed row is visible rather than mangled.
        if (i + 1 >= text.size()) {
            current.text.push_back('^');
            i++;
            continue;
        }

        // A marker ends the current run and starts a new one. Empty runs are dropped so a
        // row beginning with a marker does not produce a leading empty segment.
        if (!current.text.empty()) {
            runs.push_back(current);
        }

        current.text.clear();
        current.code = text[i + 1];
        i += 2;
    }

    if (!current.text.empty()) {
        runs.push_back(current);
    }

    return runs;
}

std::string ReplicaText::StripCodes(const std::string& text) {
    const std::vector<TextRun> runs = Parse(text);

    std::string out;
    for (size_t i = 0; i < runs.size(); i++) {
        out += runs[i].text;
    }
    return out;
}

}  // namespace iagd
