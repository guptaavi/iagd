/*
 * Offline verification of the replica-row parsing (ReplicaText / ReplicaSections).
 *
 * Unit cases first, then a sweep over every stored row in a real database: with 1.7M rows
 * of the game's own tooltip text, the corpus is a far better source of edge cases than
 * anything hand-written, and the invariant that matters is easy to state -- no marker may
 * survive into displayed text, and no text may be lost.
 *
 * Usage: replica_text_test.exe <path-to-userdata.db>
 */

#include "ReplicaText.h"
#include "SqliteDb.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace iagd;

static int g_failures = 0;

static void Check(bool condition, const std::string& what) {
    std::printf("%s  %s\n", condition ? "PASS" : "FAIL", what.c_str());
    if (!condition) {
        g_failures++;
    }
}

static std::string Joined(const std::vector<TextRun>& runs) {
    std::string out;
    for (size_t i = 0; i < runs.size(); i++) {
        out += runs[i].text;
    }
    return out;
}

int main(int argc, char** argv) {
    // --- unit cases -----------------------------------------------------------------
    {
        const std::vector<TextRun> runs = ReplicaText::Parse("+165% ^EFire Damage");
        Check(runs.size() == 2, "a single marker splits the row into two runs");
        if (runs.size() == 2) {
            Check(runs[0].code == 0 && runs[0].text == "+165% ", "text before the marker is uncoloured");
            Check(runs[1].code == 'E' && runs[1].text == "Fire Damage", "text after the marker carries its code");
        }
    }
    {
        // A marker applies until the next one, so the value at the end is coloured
        // differently from the label before it.
        const std::vector<TextRun> runs = ReplicaText::Parse("^EIncreases Energy Regeneration by ^H67%");
        Check(runs.size() == 2, "two markers produce two runs");
        if (runs.size() == 2) {
            Check(runs[0].code == 'E' && runs[1].code == 'H', "each run carries the marker that opened it");
            Check(runs[1].text == "67%", "the second marker starts a new run at the value");
        }
    }
    {
        const std::vector<TextRun> runs = ReplicaText::Parse("^EOnly a marker");
        Check(runs.size() == 1 && runs[0].code == 'E',
              "a row starting with a marker produces no empty leading run");
    }
    {
        Check(ReplicaText::Parse("").empty(), "an empty row produces no runs");
        Check(ReplicaText::StripCodes("+165% ^EFire Damage") == "+165% Fire Damage",
              "stripping removes markers and keeps the text");
    }
    {
        // Case matters: the corpus contains both 'o' and 'O', and both 'k' and 'K'.
        const std::vector<TextRun> lower = ReplicaText::Parse("a^ob");
        const std::vector<TextRun> upper = ReplicaText::Parse("a^Ob");
        Check(lower.size() == 2 && upper.size() == 2 && lower[1].code == 'o' && upper[1].code == 'O',
              "marker codes are case-sensitive");
    }
    {
        const std::vector<TextRun> runs = ReplicaText::Parse("trailing caret ^");
        Check(Joined(runs) == "trailing caret ^", "a trailing caret is kept as text, not swallowed");
    }

    // --- section classification ------------------------------------------------------
    {
        std::vector<ReplicaRow> rows;
        ReplicaRow header; header.type = 36; header.text = "Granted Skills";
        ReplicaRow blank;  blank.type = 0;
        ReplicaRow name;   name.type = 37; name.text = "Aldanar's Vanity";
        rows.push_back(header); rows.push_back(blank); rows.push_back(name);
        Check(ReplicaSections::IsGrantedSkillHeader(rows, 0),
              "a header followed by a skill name is a granted-skill header");
    }
    {
        // Same header type, but followed by stats rather than a skill name: a pet bonus.
        std::vector<ReplicaRow> rows;
        ReplicaRow header; header.type = 36;
        ReplicaRow stat;   stat.type = 19; stat.text = "+50% Fire Damage";
        rows.push_back(header); rows.push_back(stat);
        Check(!ReplicaSections::IsGrantedSkillHeader(rows, 0),
              "the same header type followed by stats is not a granted skill");
    }
    {
        ReplicaRow setName; setName.type = 21;
        ReplicaRow blank;   blank.type = 0;
        Check(ReplicaSections::IsSetName(setName), "type 21 is the set name");
        Check(ReplicaSections::IsBlank(blank), "type 0 is blank");
        Check(ReplicaSections::IsSectionStart(setName), "the set name also starts a section");
    }

    // --- sweep the whole corpus ------------------------------------------------------
    if (argc >= 2) {
        SqliteDb db;
        if (!db.OpenReadOnly(argv[1])) {
            Check(false, std::string("open database: ") + db.LastError());
        } else {
            SqliteQuery q(db);
            q.Prepare("SELECT Text FROM ReplicaItemRow WHERE Text IS NOT NULL");

            long long scanned = 0, withMarkers = 0, lostText = 0, leakedMarker = 0;
            std::string worstExample;

            while (q.Step()) {
                const std::string text = q.GetText(0);
                scanned++;

                const bool hasMarker = text.find('^') != std::string::npos;
                if (hasMarker) { withMarkers++; }

                const std::vector<TextRun> runs = ReplicaText::Parse(text);
                const std::string rejoined = Joined(runs);

                // No marker may survive into what the player sees. A trailing caret is the
                // one legitimate exception, since it is not a marker at all.
                const bool trailingCaretOnly =
                    !text.empty() && text[text.size() - 1] == '^' &&
                    rejoined.find('^') == rejoined.size() - 1;
                if (rejoined.find('^') != std::string::npos && !trailingCaretOnly) {
                    if (leakedMarker++ == 0) { worstExample = text; }
                }

                // Every character except the markers must survive. Two bytes are consumed
                // per marker, so the arithmetic is exact rather than approximate.
                size_t markerBytes = 0;
                for (size_t i = 0; i + 1 < text.size(); i++) {
                    if (text[i] == '^') { markerBytes += 2; i++; }
                }
                if (rejoined.size() + markerBytes != text.size()) {
                    if (lostText++ == 0) { worstExample = text; }
                }
            }

            std::printf("      swept %lld stored rows, %lld carrying markers\n", scanned, withMarkers);
            Check(scanned > 0, "the corpus actually contained rows");
            Check(withMarkers > 0, "the corpus actually contained colour markers");
            Check(leakedMarker == 0, "no marker survives into displayed text ("
                                     + std::to_string(leakedMarker) + " leaked)");
            Check(lostText == 0, "no text is lost or duplicated by parsing ("
                                 + std::to_string(lostText) + " rows differ)");
            if (!worstExample.empty()) {
                std::printf("      first offending row: %s\n", worstExample.c_str());
            }
        }
    } else {
        std::printf("NOTE  no database given, skipping the corpus sweep\n");
    }

    std::printf("\n%s (%d failures)\n", g_failures == 0 ? "OK" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
