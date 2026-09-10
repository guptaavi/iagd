#include "stdafx.h"
#include "OverlayShell.h"
#include "ItemIcons.h"
#include "Logger.h"
#include "OverlayInput.h"
#include "OverlaySearch.h"
#include "OverlayTransfer.h"
#include "ReplicaText.h"
#include "SettingsReader.h"

#include <sstream>
#include <string>
#include <vector>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

namespace {

// ---------------------------------------------------------------------------------------
// Ported tables
//
// Each of these has a C# original that the client builds its own controls from. They are
// transcribed rather than shared, and the original is named above each one: if a stat field
// is added there it has to be added here too, or the two UIs quietly disagree about what a
// checkbox means.
// ---------------------------------------------------------------------------------------

struct StatFilter {
    /// The name of the checkbox this row was transcribed from, in the C# file named above
    /// the table. Not used at runtime -- the sidebar generates its own ids -- but it is what
    /// makes the two tables diffable by eye when the client's filters change.
    const char* sourceControl;

    const char* label;

    /// Comma-separated, split at bind time. Written as one string so the table reads like
    /// the C# array it came from rather than like a list of lists.
    const char* fields;
};

/// From IAGrim/UI/Filters/Damage.cs. The elemental types carry the elemental fields as well,
/// because an item with "+15% Elemental Damage" satisfies a search for fire damage.
const StatFilter kDamageFilters[] = {
    { "dmgTotal",      "Total damage", "offensiveTotalDamageModifier" },
    { "dmgPhysical",   "Physical",     "offensivePhysical,offensivePhysicalModifier" },
    { "dmgPiercing",   "Piercing",     "offensivePierce,offensivePierceModifier" },
    { "dmgFire",       "Fire",         "offensiveFire,offensiveFireModifier,offensiveElemental,offensiveElementalModifier" },
    { "dmgCold",       "Cold",         "offensiveCold,offensiveColdModifier,offensiveElemental,offensiveElementalModifier" },
    { "dmgLightning",  "Lightning",    "offensiveLightning,offensiveLightningModifier,offensiveElemental,offensiveElementalModifier" },
    { "dmgAether",     "Aether",       "offensiveAether,offensiveAetherModifier" },
    { "dmgVitality",   "Vitality",     "offensiveLife,offensiveLifeModifier" },
    { "dmgChaos",      "Chaos",        "offensiveChaos,offensiveChaosModifier" },
    { "dmgAcid",       "Acid",         "offensivePoison,offensivePoisonModifier" },
    { "dmgElemental",  "Elemental",    "offensiveElemental,offensiveElementalModifier" },
};

/// From IAGrim/UI/Filters/Resistances.cs. Each resistance covers the flat and modifier
/// fields and their "slow" counterparts, which is where duration reduction lives.
const StatFilter kResistanceFilters[] = {
    { "resElemental",  "Elemental", "defensiveElementalResistance" },
    { "resPhysical",   "Physical",  "defensivePhysical,defensivePhysicalModifier,defensiveSlowPhysical,defensiveSlowPhysicalModifier" },
    { "resPiercing",   "Piercing",  "defensivePierce,defensivePierceModifier,defensiveSlowPierce,defensiveSlowPierceModifier" },
    { "resFire",       "Fire",      "defensiveFire,defensiveFireModifier,defensiveSlowFire,defensiveSlowFireModifier" },
    { "resCold",       "Cold",      "defensiveCold,defensiveColdModifier,defensiveSlowCold,defensiveSlowColdModifier" },
    { "resLightning",  "Lightning", "defensiveLightning,defensiveLightningModifier,defensiveSlowLightning,defensiveSlowLightningModifier" },
    { "resAether",     "Aether",    "defensiveAether,defensiveAetherModifier,defensiveSlowAether,defensiveSlowAetherModifier" },
    { "resVitality",   "Vitality",  "defensiveLife,defensiveLifeModifier,defensiveSlowLife,defensiveSlowLifeModifier" },
    { "resChaos",      "Chaos",     "defensiveChaos,defensiveChaosModifier,defensiveSlowChaos,defensiveSlowChaosModifier" },
    { "resPoison",     "Poison",    "defensivePoison,defensivePoisonModifier,defensiveSlowPoison,defensiveSlowPoisonModifier" },
    { "resBleeding",   "Bleeding",  "defensiveBleeding,defensiveBleedingModifier,defensiveSlowBleeding,defensiveSlowBleedingModifier" },
    { "resStun",       "Stun",      "defensiveStun,defensiveStunModifier,defensiveSlowStun,defensiveSlowStunModifier" },
    { "resSlow",       "Slow",      "defensiveTotalSpeedResistance" },
};

struct QualityOption {
    const char* label;
    const char* rarity;   ///< Empty means "any".
    int prefixRarity;
};

/// From IAGrim/UI/UIHelper.QualityFilter. "Rare" is Green with a prefix rarity, which is
/// why quality is two fields rather than one.
const QualityOption kQualityOptions[] = {
    { "Any quality",       "",      0 },
    { "Magical",           "Yellow", 0 },
    { "Rare",              "Green",  0 },
    { "Rare (1 affix)",    "Green",  1 },
    { "Rare (2 affixes)",  "Green",  2 },
    { "Epic",              "Blue",   0 },
    { "Legendary",         "Epic",   0 },
};

struct SlotOption {
    const char* label;
    const char* classes;  ///< Comma-separated "Class" text values; empty means "any".
    bool inverse;
};

/// From IAGrim/UI/UIHelper.SlotFilter. The last entry is the client's "Other": every slot
/// the list names, inverted, so it catches what none of the others do.
const SlotOption kSlotOptions[] = {
    { "Any slot", "", false },
    { "Armour", "ArmorProtective_Head,ArmorProtective_Hands,ArmorProtective_Feet,ArmorProtective_Legs,ArmorProtective_Chest,ArmorProtective_Waist,ArmorJewelry_Medal,ArmorJewelry_Ring,ArmorProtective_Shoulders,ArmorJewelry_Amulet", false },
    { "Head", "ArmorProtective_Head", false },
    { "Hands", "ArmorProtective_Hands", false },
    { "Feet", "ArmorProtective_Feet", false },
    { "Legs", "ArmorProtective_Legs", false },
    { "Chest", "ArmorProtective_Chest", false },
    { "Belt", "ArmorProtective_Waist", false },
    { "Medal", "ArmorJewelry_Medal", false },
    { "Ring", "ArmorJewelry_Ring", false },
    { "Shoulders", "ArmorProtective_Shoulders", false },
    { "Amulet", "ArmorJewelry_Amulet", false },
    { "One-handed melee", "WeaponMelee_Dagger,WeaponMelee_Mace,WeaponMelee_Axe,WeaponMelee_Scepter,WeaponMelee_Sword", false },
    { "Two-handed melee", "WeaponMelee_Sword2h,WeaponMelee_Mace2h,WeaponMelee_Axe2h,WeaponMelee_Spear2h", false },
    { "One-handed ranged", "WeaponHunting_Ranged1h", false },
    { "Two-handed ranged", "WeaponHunting_Ranged2h", false },
    { "Off-hand", "WeaponArmor_Offhand", false },
    { "Shield", "WeaponArmor_Shield", false },
    { "Relic", "ItemArtifact", false },
    { "Component", "ItemRelic", false },
    { "Other",
      "ArmorProtective_Head,ArmorProtective_Hands,ArmorProtective_Feet,ArmorProtective_Legs,ArmorProtective_Chest,"
      "ArmorProtective_Waist,ArmorJewelry_Medal,ArmorJewelry_Ring,ArmorProtective_Shoulders,ArmorJewelry_Amulet,"
      "WeaponMelee_Dagger,WeaponMelee_Mace,WeaponMelee_Axe,WeaponMelee_Scepter,WeaponMelee_Sword,"
      "WeaponMelee_Sword2h,WeaponMelee_Mace2h,WeaponMelee_Axe2h,WeaponMelee_Spear2h,"
      "WeaponHunting_Ranged1h,WeaponHunting_Ranged2h,WeaponArmor_Offhand,WeaponArmor_Shield,ItemArtifact",
      true },
};

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------

std::vector<std::string> Split(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::string current;

    for (char c : value) {
        if (c == separator) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        }
        else {
            current += c;
        }
    }

    if (!current.empty()) {
        parts.push_back(current);
    }

    return parts;
}

/// Item names and stat text come from the game and are pasted into RML. A name with an
/// ampersand in it would otherwise be read as the start of an entity, and one with a '<'
/// would end the element it is inside.
std::string Escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());

    for (char c : text) {
        switch (c) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        default:  escaped += c; break;
        }
    }

    return escaped;
}

/// The client's rarity names, which are the values stored in PlayerItem.Rarity rather than
/// the words a player would use. Kept as the class name so the RCSS below reads the same
/// way the WebUI's does.
/// U+00B7 MIDDLE DOT, as a numeric character reference.
///
/// RmlUi's StringUtilities::DecodeRml resolves exactly four named entities -- &lt; &gt;
/// &amp; &quot; -- plus numeric references. "&middot;" is not among them, so writing it
/// put the letters of the entity on screen instead of a dot. A numeric reference is
/// resolved, and unlike a literal character it keeps this file pure ASCII, so the bytes
/// cannot change meaning with the compiler's source encoding.
const char* const kMiddleDot = "&#183;";

std::string RarityClass(const std::string& rarity) {
    if (rarity == "Yellow" || rarity == "Green" || rarity == "Blue" || rarity == "Epic" || rarity == "Legendary") {
        return "rarity-" + rarity;
    }

    return "rarity-White";
}

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------

struct ShellState {
    Rml::ElementDocument* document = nullptr;

    std::string wildcard;
    std::string minimumLevel;
    std::string maximumLevel;
    int qualityIndex = 0;
    int slotIndex = 0;
    std::vector<bool> damageChecked;
    std::vector<bool> resistanceChecked;

    /// Where in the result set the current page starts, in database rows.
    int skip = 0;

    /// A search is posted a moment after the last keystroke rather than on each one, so
    /// typing a word costs one query instead of one per letter.
    unsigned long scheduledAtTick = 0;
    bool hasScheduledSearch = false;

    /// The stacks the current search returned, so a click can be resolved back to an item
    /// without reading it out of the DOM. Only a slice of these is on screen at a time.
    std::vector<std::vector<iagd::ItemSearchRow>> stacks;
    int selectedIndex = -1;

    /// Base record -> bitmap path, for the current result.
    std::map<std::string, std::string> icons;

    /// Where the visible slice starts within `stacks`.
    int viewOffset = 0;

    /// Whether the database had more rows than the query's cap, so the count can say so.
    bool lastWasTruncated = false;

    /// Stat rows for the items on screen, keyed by player item id. Filled in a moment
    /// after the cards appear. Kept across a refresh -- the key is the item, not the
    /// position, so rows fetched before the refresh are still the right rows after it, and
    /// keeping them is what stops the cards collapsing to bare names and expanding again.
    std::map<int64_t, std::vector<iagd::ReplicaRow>> pageDetails;

    unsigned long long lastDataVersionChanges = 0;

    /// A foreign write has been seen and a refresh is owed, once the writing stops. See
    /// kRefreshQuietMilliseconds.
    bool hasDeferredRefresh = false;
    unsigned long refreshQuietUntilTick = 0;
    unsigned long refreshDeferredSinceTick = 0;

    /// Whether the search now in flight is a refresh of what is already on screen rather
    /// than a new question from the player. A refresh keeps their place; a new question
    /// starts at the top.
    bool scheduledPreservesView = false;
    bool preserveViewOnNextResult = false;

    /// Where the results were scrolled to before a refresh rebuilt them, and how many more
    /// frames to keep putting them back there. One assignment is not enough: replacing the
    /// contents resets the offset, and so does each relayout that follows -- the stat rows
    /// arriving from the worker change every card's height a frame or two later. So the
    /// offset is re-applied until it sticks rather than once and hopefully.
    float restoreScrollTop = 0.0f;
    int restoreScrollFrames = 0;

    /// What the scroll thumb was last set to, so an unchanged frame writes no properties.
    float thumbHeight = -1.0f;
    float thumbOffset = -1.0f;

    bool isDarkMode = false;
};

ShellState& state() {
    static ShellState instance;
    return instance;
}

const unsigned long kSearchDelayMilliseconds = 250;

/// One page of the result set, in database rows. The query itself returns up to
/// ItemSearch::MaxSearchResults, so a page never asks for more than one query's worth.
const int kPageSize = iagd::ItemSearch::MaxSearchResults;

/// How many cards are drawn at once. With the stats on the cards a card is tens of elements
/// rather than four, so the whole database page cannot be drawn the way it could when a card
/// was a name and a level. The client serves its own grid in batches of 64 for the same
/// reason; this is the same idea with a round number.
const int kCardsPerView = 60;

/// <summary>
/// How quiet the database has to go before a foreign write turns into a refresh.
///
/// PRAGMA data_version says "somebody committed", not "your results changed", and the
/// client commits in bursts: a replica backfill writes twice a second for as long as it
/// takes to walk the collection. Refreshing on each one re-ran the search several times a
/// second, which read as flicker and made the list impossible to scroll or click. So a
/// write starts a timer instead, and every further write restarts it -- a burst of a
/// thousand commits costs one refresh, at the end.
const unsigned long kRefreshQuietMilliseconds = 1500;

/// The longest a refresh is put off while the client keeps writing. Without this, a backfill
/// that runs for ten minutes would leave the overlay ten minutes stale, since it never goes
/// quiet. With the view and the fetched stats both preserved across a refresh, one every
/// fifteen seconds is not something the player sees.
const unsigned long kRefreshMaximumDeferMilliseconds = 15000;

/// A ceiling on the retained stat rows, since a refresh no longer clears them. Reached only
/// after hundreds of refreshes; dropping the lot is fine because the next page re-fetches
/// whatever it needs.
const size_t kMaxCachedDetails = 4000;

/// How many frames a refresh keeps re-asserting the scroll offset. Long enough to outlast
/// the relayout caused by the page's stat rows arriving, short enough that a player who
/// grabs the wheel in the same tenth of a second is not fought for long.
const int kScrollRestoreFrames = 12;

void ScheduleSearch() {
    ShellState& s = state();
    s.scheduledAtTick = ::GetTickCount() + kSearchDelayMilliseconds;
    s.hasScheduledSearch = true;
    s.scheduledPreservesView = false;
}

/// <summary>
/// Re-runs the current search because the database changed underneath it, keeping the
/// player where they were: same page, same selection, same stats already on the cards.
/// </summary>
void ScheduleRefresh() {
    ShellState& s = state();

    // A question the player has already asked for wins: they are typing, and what they are
    // typing matters more than what the client just wrote.
    if (s.hasScheduledSearch && !s.scheduledPreservesView) {
        return;
    }

    s.scheduledAtTick = ::GetTickCount() + kSearchDelayMilliseconds;
    s.hasScheduledSearch = true;
    s.scheduledPreservesView = true;
}

float ToLevel(const std::string& text) {
    if (text.empty()) {
        return 0.0f;
    }

    try {
        return (float)std::stod(text);
    }
    catch (...) {
        // A half-typed number is not an error, it is a player mid-keystroke.
        return 0.0f;
    }
}

iagd::ItemSearchRequest BuildRequest() {
    ShellState& s = state();
    iagd::ItemSearchRequest request;

    request.wildcard = s.wildcard;
    request.minimumLevel = ToLevel(s.minimumLevel);
    request.maximumLevel = ToLevel(s.maximumLevel);

    if (s.qualityIndex > 0 && s.qualityIndex < (int)(sizeof(kQualityOptions) / sizeof(kQualityOptions[0]))) {
        request.rarity = kQualityOptions[s.qualityIndex].rarity;
        request.prefixRarity = kQualityOptions[s.qualityIndex].prefixRarity;
    }

    if (s.slotIndex > 0 && s.slotIndex < (int)(sizeof(kSlotOptions) / sizeof(kSlotOptions[0]))) {
        request.slot = Split(kSlotOptions[s.slotIndex].classes, ',');
        request.slotInverse = kSlotOptions[s.slotIndex].inverse;
    }

    for (size_t i = 0; i < s.damageChecked.size(); i++) {
        if (s.damageChecked[i]) {
            request.filters.push_back(Split(kDamageFilters[i].fields, ','));
        }
    }

    for (size_t i = 0; i < s.resistanceChecked.size(); i++) {
        if (s.resistanceChecked[i]) {
            request.filters.push_back(Split(kResistanceFilters[i].fields, ','));
        }
    }

    // The mod and hardcore flag are filled in by the worker from the world the player is
    // standing in; a caller cannot get them wrong because a caller does not supply them.
    return request;
}

void SetStatus(const std::string& text) {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    if (Rml::Element* status = s.document->GetElementById("status")) {
        status->SetInnerRML(Escape(text));
    }
}

void RunSearch(bool preserveView = false) {
    ShellState& s = state();
    s.hasScheduledSearch = false;
    s.preserveViewOnNextResult = preserveView;

    if (!OverlaySearch::IsReady()) {
        SetStatus("The item database is not available.");
        return;
    }

    OverlaySearch::Submit(BuildRequest(), s.skip, false);

    // Not on a refresh: the count under the grid is still true until the new one arrives,
    // and replacing it with "Searching..." twice a minute is the flicker in miniature.
    if (!preserveView) {
        SetStatus("Searching...");
    }
}

// ---------------------------------------------------------------------------------------
// Rendering the model into the document
// ---------------------------------------------------------------------------------------

/// <summary>
/// Renders one item's stat rows as the client's item grid does: each row keeps its own type
/// as a class for the RCSS to colour by, and the "^" codes inside it become spans.
///
/// Shared by the cards and the detail pane so an item cannot read differently in the two
/// places. `compact` drops the blank spacer rows, which are worth the vertical space in a
/// pane devoted to one item but not in a grid meant for comparing many.
/// </summary>
void AppendReplicaRows(std::ostringstream& rml, const std::vector<iagd::ReplicaRow>& rows, bool compact) {
    for (size_t i = 0; i < rows.size(); i++) {
        const iagd::ReplicaRow& row = rows[i];

        if (iagd::ReplicaSections::IsBlank(row)) {
            if (!compact) {
                rml << "<div class=\"replica-blank\"/>";
            }
            continue;
        }

        rml << "<div class=\"replica-row replica-type-" << row.type << "\">";

        for (const iagd::TextRun& run : iagd::ReplicaText::Parse(row.text)) {
            if (run.text.empty()) {
                continue;
            }

            if (run.code == 0) {
                rml << Escape(run.text);
            }
            else {
                rml << "<span class=\"replica-letter-" << run.code << "\">" << Escape(run.text) << "</span>";
            }
        }

        rml << "</div>";
    }
}

/// <summary>
/// Writes one card's stat block, including the case where the item has none.
///
/// An empty entry is not the same as a missing one: the worker returns an entry for every
/// item on the page, so an entry that is present and empty means the item genuinely has no
/// stored stat text. Saying so is the difference between a card that explains itself and a
/// card that looks like it failed to load -- and on this collection it is not a rare case:
/// items the client has never been able to generate stat text for sort first under an empty
/// search, so it is the first thing the overlay shows.
/// </summary>
void AppendCardStats(std::ostringstream& rml, const std::vector<iagd::ReplicaRow>& rows) {
    if (rows.empty()) {
        rml << "<div class=\"nostats\">No stat details stored for this item.</div>";
        return;
    }

    AppendReplicaRows(rml, rows, true);
}

/// <summary>
/// Draws the visible slice of the current result.
///
/// Only a slice: with the stats on the cards, a card is twenty or thirty elements rather
/// than four, and a database page is several hundred stacks. Drawing them all was
/// affordable when a card was a name and a level, and is not now. The client solves the
/// same problem the same way, serving its grid in batches rather than the whole match set.
/// </summary>
void RenderView(bool preserveScroll = false) {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    Rml::Element* container = s.document->GetElementById("results");
    if (container == nullptr) {
        return;
    }

    // Replacing the contents scrolls the container back to the top. On a refresh that is
    // the player being yanked to the top of the list mid-read -- and, while the client was
    // writing in a loop, it was why the wheel appeared not to work at all: it scrolled, and
    // was put back a fraction of a second later.
    const float scrollTop = preserveScroll ? container->GetScrollTop() : 0.0f;

    const size_t first = (size_t)s.viewOffset;
    const size_t last = (std::min)(s.stacks.size(), first + (size_t)kCardsPerView);

    std::ostringstream rml;
    std::vector<int64_t> visibleIds;

    for (size_t i = first; i < last; i++) {
        const iagd::ItemSearchRow& row = s.stacks[i].front();
        visibleIds.push_back(row.id);

        long long count = 0;
        for (const auto& member : s.stacks[i]) {
            count += member.stackCount > 0 ? member.stackCount : 1;
        }

        std::string icon;
        auto found = s.icons.find(row.baseRecord);
        if (found != s.icons.end()) {
            icon = "/storage/" + iagd::ItemIcons::ToImageFileName(found->second);
        }

        // data-index is the absolute index into the result, not the index within the
        // slice, so a click still resolves to the right item after paging.
        rml << "<div class=\"card " << RarityClass(row.rarity) << "\" data-index=\"" << i << "\">";

        rml << "<div class=\"cardhead\">";
        if (icon.empty()) {
            // No bitmap stat on the record at all, which is different from a bitmap whose
            // file is missing -- that one is the render interface's placeholder.
            rml << "<div class=\"icon noicon\"/>";
        }
        else {
            rml << "<img class=\"icon\" src=\"" << Escape(icon) << "\"/>";
        }

        rml << "<div class=\"cardtext\">"
            << "<div class=\"name\">" << Escape(row.nameIsNull || row.name.empty() ? "Unknown" : row.name) << "</div>"
            << "<div class=\"meta\">level " << (int)row.levelRequirement;

        if (count > 1) {
            rml << " " << kMiddleDot << " x" << count;
        }

        rml << "</div></div></div>";

        // Filled in when the worker returns the page's rows. Present but empty until then,
        // so the grid appears at once and gains its stats a moment later rather than the
        // player waiting on a second query before seeing anything.
        rml << "<div class=\"stats\" id=\"stats-" << i << "\">";
        auto cached = s.pageDetails.find(row.id);
        if (cached != s.pageDetails.end()) {
            AppendCardStats(rml, cached->second);
        }
        rml << "</div>";

        rml << "</div>";
    }

    container->SetInnerRML(rml.str());

    if (preserveScroll && scrollTop > 0.0f) {
        s.document->UpdateDocument();
        container->SetScrollTop(scrollTop);

        s.restoreScrollTop = scrollTop;
        s.restoreScrollFrames = kScrollRestoreFrames;
    }

    // Re-apply the selection: the cards were just rebuilt, so the class went with them.
    if (s.selectedIndex >= (int)first && s.selectedIndex < (int)last) {
        if (Rml::Element* card = container->GetChild((int)((size_t)s.selectedIndex - first))) {
            card->SetClass("selected", true);
        }
    }

    std::ostringstream status;
    if (s.stacks.empty()) {
        status << "No matching items";
    }
    else {
        status << "showing " << (first + 1) << "-" << last << " of " << s.stacks.size();
        if (s.lastWasTruncated) {
            status << "+";
        }
        status << (s.stacks.size() == 1 ? " stack" : " stacks");
    }
    SetStatus(status.str());

    if (!visibleIds.empty()) {
        OverlaySearch::SubmitPageDetails(visibleIds, OverlaySearch::CurrentGeneration());
    }
}

/// <summary>
/// Fills in the stat blocks of the cards already on screen.
///
/// Writes into each card's existing stats element rather than rebuilding the grid: the
/// player may already be reading it, and replacing the whole list under them would lose
/// their scroll position and blink every card.
/// </summary>
void ApplyPageDetails() {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    const size_t first = (size_t)s.viewOffset;
    const size_t last = (std::min)(s.stacks.size(), first + (size_t)kCardsPerView);

    for (size_t i = first; i < last; i++) {
        const int64_t itemId = s.stacks[i].front().id;

        auto rows = s.pageDetails.find(itemId);
        if (rows == s.pageDetails.end()) {
            continue;
        }

        Rml::Element* target = s.document->GetElementById("stats-" + std::to_string(i));
        if (target == nullptr) {
            continue;
        }

        std::ostringstream rml;
        AppendCardStats(rml, rows->second);
        target->SetInnerRML(rml.str());
    }
}

/// <summary>
/// Sizes and positions the results scroll thumb from the pane it describes.
///
/// Done per frame rather than on scroll: the thumb depends on the content height as well as
/// the offset, and the content height changes on its own when a page's stat rows arrive.
/// Nothing is written unless a value actually changed, so a still list costs two float
/// comparisons.
/// </summary>
void UpdateScrollThumb() {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    Rml::Element* results = s.document->GetElementById("results");
    Rml::Element* thumb = s.document->GetElementById("scrollthumb");
    if (results == nullptr || thumb == nullptr) {
        return;
    }

    const float content = results->GetScrollHeight();
    const float visible = results->GetClientHeight();
    const float range = content - visible;

    float height = 0.0f;
    float offset = 0.0f;

    // Nothing to scroll leaves the thumb at zero height, which reads as "this is all of it"
    // rather than as a scrollbar stuck at full length.
    if (range > 1.0f && content > 0.0f) {
        const float track = thumb->GetParentNode()->GetClientHeight();
        const float minimum = 24.0f;

        height = track * (visible / content);
        height = height < minimum ? minimum : height;
        offset = (track - height) * (results->GetScrollTop() / range);
    }

    if (height != s.thumbHeight || offset != s.thumbOffset) {
        s.thumbHeight = height;
        s.thumbOffset = offset;

        thumb->SetProperty("height", std::to_string((int)height) + "px");
        thumb->SetProperty("margin-top", std::to_string((int)offset) + "px");
    }
}

void RenderResults(const OverlaySearchResult& result) {
    ShellState& s = state();

    const bool preserveView = s.preserveViewOnNextResult;
    s.preserveViewOnNextResult = false;

    const int previousOffset = s.viewOffset;
    const int previousSelection = s.selectedIndex;
    const int64_t previousSelectedId =
        (previousSelection >= 0 && previousSelection < (int)s.stacks.size())
            ? s.stacks[previousSelection].front().id
            : 0;

    s.stacks = result.stacks;
    s.icons = result.icons;
    s.lastWasTruncated = result.wasTruncated;

    if (!preserveView) {
        s.selectedIndex = -1;
        s.viewOffset = 0;

        // A new question: the stats on hand answer the old one, and the player is about to
        // be looking at different items.
        s.pageDetails.clear();
        RenderView();
        return;
    }

    // A refresh. The rows may have shifted -- an item can have been transferred away, or
    // the client may have just given one the stats it was missing -- so the selection is
    // followed by identity rather than by index, and the page is clamped rather than reset.
    s.viewOffset = 0;
    if (!s.stacks.empty() && previousOffset > 0) {
        const int lastStart = ((int)(s.stacks.size() - 1) / kCardsPerView) * kCardsPerView;
        s.viewOffset = previousOffset < lastStart ? previousOffset : lastStart;
    }

    s.selectedIndex = -1;
    if (previousSelectedId != 0) {
        for (size_t i = 0; i < s.stacks.size(); i++) {
            if (s.stacks[i].front().id == previousSelectedId) {
                s.selectedIndex = (int)i;
                break;
            }
        }
    }

    if (s.pageDetails.size() > kMaxCachedDetails) {
        s.pageDetails.clear();
    }

    RenderView(true);
}

/// <summary>
/// Renders one item's stored tooltip rows.
///
/// Each row keeps its own type as a class, which is what the RCSS colours it by, and the
/// "^" codes inside it become spans. Both mappings are ports: the types from
/// WebUI/src/components/Item/replicaSections.ts and the colours from ReplicaStat.css.
/// </summary>
void RenderDetail(const OverlayItemDetail& detail) {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    Rml::Element* pane = s.document->GetElementById("detail");
    if (pane == nullptr) {
        return;
    }

    std::ostringstream rml;
    AppendReplicaRows(rml, detail.rows, false);

    if (detail.rows.empty()) {
        rml << "<div class=\"replica-row\">This item has no stored stat text.</div>";
    }

    pane->SetInnerRML(rml.str());
}

void SelectCard(int index) {
    ShellState& s = state();
    if (index < 0 || index >= (int)s.stacks.size() || s.document == nullptr) {
        return;
    }

    s.selectedIndex = index;

    // The selection is a class on the card rather than a redraw of the list: rebuilding a
    // thousand cards to move a highlight would be visible as a stutter.
    if (Rml::Element* container = s.document->GetElementById("results")) {
        for (int i = 0; i < (int)container->GetNumChildren(); i++) {
            container->GetChild(i)->SetClass("selected", i == index);
        }
    }

    OverlaySearch::SubmitDetail(s.stacks[index].front().id);
}

void SetTransferStatus(const std::string& text) {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    if (Rml::Element* element = s.document->GetElementById("transfer-status")) {
        element->SetInnerRML(Escape(text));
    }
}

/// <summary>
/// Asks for the selected item.
///
/// Two steps, because the row the grid holds carries only what the grid draws: the worker
/// reads the rest of the columns, and Update hands the result to the transfer once it
/// arrives. The player sees one click.
/// </summary>
void RequestTransfer() {
    ShellState& s = state();

    if (s.selectedIndex < 0 || s.selectedIndex >= (int)s.stacks.size()) {
        SetTransferStatus("Select an item first.");
        return;
    }

    const iagd::ItemSearchRow& row = s.stacks[s.selectedIndex].front();
    OverlaySearch::SubmitTransferLookup(row.id);
    SetTransferStatus("Preparing the transfer...");
}

// ---------------------------------------------------------------------------------------
// Listeners
// ---------------------------------------------------------------------------------------

/// <summary>
/// One listener for the whole document rather than one per control.
///
/// RmlUi events bubble, so a listener on the document sees every change and every click and
/// can decide what it was from the element's id. That keeps the wiring in one readable
/// place, and it keeps working when the sidebar's controls are generated rather than
/// written out.
/// </summary>
class ShellListener : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override {
        try {
            Rml::Element* target = event.GetTargetElement();
            if (target == nullptr) {
                return;
            }

            if (event.GetId() == Rml::EventId::Change) {
                OnChange(target, event);
            }
            else if (event.GetId() == Rml::EventId::Click) {
                OnClick(target);
            }
        }
        catch (const std::exception& ex) {
            LogToFile(LogLevel::WARNING, std::string("Overlay shell: ") + ex.what());
        }
        catch (...) {
            LogToFile(LogLevel::WARNING, "Overlay shell: unknown error handling an event.");
        }
    }

private:
    void OnChange(Rml::Element* target, Rml::Event& event) {
        ShellState& s = state();
        const Rml::String id = target->GetId();
        const Rml::String value = event.GetParameter<Rml::String>("value", "");

        if (id == "q") {
            s.wildcard = value;
        }
        else if (id == "minlevel") {
            s.minimumLevel = value;
        }
        else if (id == "maxlevel") {
            s.maximumLevel = value;
        }
        else if (id == "quality") {
            s.qualityIndex = std::atoi(value.c_str());
        }
        else if (id == "slot") {
            s.slotIndex = std::atoi(value.c_str());
        }
        else if (id.size() > 4 && id.compare(0, 4, "dmg_") == 0) {
            s.damageChecked[(size_t)std::atoi(id.c_str() + 4)] = event.GetParameter<bool>("checked", false);
        }
        else if (id.size() > 4 && id.compare(0, 4, "res_") == 0) {
            s.resistanceChecked[(size_t)std::atoi(id.c_str() + 4)] = event.GetParameter<bool>("checked", false);
        }
        else {
            return;
        }

        // Any change starts the result set again from the top: page 3 of the old search
        // has nothing to do with the new one.
        s.skip = 0;
        ScheduleSearch();
    }

    void OnClick(Rml::Element* target) {
        ShellState& s = state();

        // Paging walks the slice first and only asks the database when the slice runs out
        // of the page it already has. A player stepping through results should not pay for
        // a query per step when the rows are already in hand.
        if (target->GetId() == "prev") {
            if (s.viewOffset > 0) {
                s.viewOffset = s.viewOffset > kCardsPerView ? s.viewOffset - kCardsPerView : 0;
                RenderView();
            }
            else if (s.skip > 0) {
                s.skip = s.skip > kPageSize ? s.skip - kPageSize : 0;
                RunSearch();
            }
            return;
        }

        if (target->GetId() == "next") {
            if (s.viewOffset + kCardsPerView < (int)s.stacks.size()) {
                s.viewOffset += kCardsPerView;
                RenderView();
            }
            else if (s.lastWasTruncated) {
                // The slice is at the end of this database page and there are more rows
                // behind it.
                s.skip += kPageSize;
                RunSearch();
            }
            return;
        }

        if (target->GetId() == "close") {
            OverlayInput::RequestClose();
            return;
        }

        if (target->GetId() == "transfer") {
            RequestTransfer();
            return;
        }

        // A click lands on whatever part of the card is under the pointer -- the icon, the
        // name -- so walk up until the card itself is found.
        for (Rml::Element* element = target; element != nullptr; element = element->GetParentNode()) {
            const Rml::String index = element->GetAttribute<Rml::String>("data-index", "");
            if (!index.empty()) {
                SelectCard(std::atoi(index.c_str()));
                return;
            }
        }
    }
};

ShellListener& listener() {
    static ShellListener instance;
    return instance;
}

// ---------------------------------------------------------------------------------------
// The document
// ---------------------------------------------------------------------------------------

const char* const kDocument = R"RML(<rml>
<head>
    <title>Item Assistant</title>
    <style>
        body, div, h1, h2, p, label, span.block { display: block; }

        body {
            font-family: overlay-ui;
            font-size: 14dp;
            width: 100%;
            height: 100%;
        }

        /* Flexbox rather than absolute offsets. The first attempt positioned each band
           against #root with top/bottom/right; the bands landed correctly but the columns
           inside them never took a usable width, so the stat pane wrapped one word per
           line. Flex gives the rows a definite height to share out and the columns a
           definite width, which is what both of those needed. */
        #root {
            position: absolute;
            top: 4%;
            left: 4%;
            width: 92%;
            height: 92%;
            display: flex;
            flex-direction: column;
            border: 2dp;
            border-radius: 6dp;
        }

        #topbar {
            display: block;
            flex: 0 0 auto;
            padding: 10dp;
        }

        #topbar input.text, #topbar select {
            display: inline-block;
            height: 26dp;
            margin-right: 8dp;
            padding: 2dp 6dp;
            border: 1dp;
        }

        #q { width: 320dp; }
        #minlevel, #maxlevel { width: 60dp; }
        #quality { width: 170dp; }
        #slot { width: 190dp; }

        #status {
            display: inline-block;
            padding-top: 4dp;
        }

        /* Floated so it keeps the top-right corner as the bar's contents change, which is
           where a window's close control is expected to be. Escape does the same thing;
           the button is there so that is discoverable without being told. */
        #close {
            display: inline-block;
            float: right;
            padding: 4dp 14dp;
            border: 1dp;
        }

        #body {
            display: flex;
            flex-direction: row;
            flex: 1 1 0dp;
            min-height: 0dp;
        }

        /* overflow-y is "hidden" rather than "auto" on all three scrolling panes, and the
           wheel is driven from OverlayUi instead. Giving RmlUi a scrollbar to lay out here
           collapses the pane's children to their minimum content width -- every stat line
           wrapping to one word -- which was reproduced in-game with "auto" and again with
           "scroll", and disappears with "hidden". Clipping is all these panes need from the
           layout; the scrolling they need is an offset, which OverlayUi sets directly. */
        #sidebar {
            display: block;
            flex: 0 0 190dp;
            overflow-y: hidden;
            padding: 8dp;
        }

        /* flex-basis is 0 rather than auto so this row cannot claim its content's height.
           With "auto" the column of cards -- twenty thousand dp of it -- became the height
           the row asked for, #root handed it over, and nothing ever overflowed, so there
           was nothing to scroll. */
        #results {
            display: block;
            flex: 1 1 auto;
            overflow-y: hidden;
            padding: 4dp;
        }

        /* The scroll indicator RmlUi's own overflow would have drawn.
           #results clips rather than scrolls (see the overflow comment above), so there is
           no scrollbar and a player has nothing telling them the list continues. This is a
           sibling of the results rather than a child, so it does not scroll along with the
           content it is describing; the thumb is sized and positioned from C++ in Update. */
        #scrolltrack {
            display: block;
            flex: 0 0 6dp;
            margin: 4dp 2dp;
        }

        #scrollthumb {
            display: block;
            width: 6dp;
            height: 0dp;
            border-radius: 3dp;
        }

        /* The stat pane is a column of its own so the transfer bar stays put while the
           stats scroll: a button that scrolls out of reach is a button the player has to
           hunt for. */
        #detailcol {
            display: flex;
            flex-direction: column;
            flex: 0 0 380dp;
            min-height: 0dp;
        }

        #transfer-bar {
            display: block;
            flex: 0 0 auto;
            padding: 8dp 10dp;
        }

        #transfer {
            display: inline-block;
            padding: 5dp 12dp;
            border: 1dp;
        }

        #transfer-status {
            display: block;
            padding-top: 6dp;
            font-size: 11dp;
        }

        #detail {
            display: block;
            flex: 1 1 0dp;
            overflow-y: hidden;
            padding: 10dp;
        }

        .grouphead {
            display: block;
            font-weight: bold;
            margin: 8dp 0dp 4dp 0dp;
        }

        label {
            display: block;
            padding: 1dp 0dp;
        }

        /* RmlUi draws nothing for a checkbox on its own: the element has no intrinsic size
           and no default appearance, so it is invisible until it is given both. The tick is
           the :checked fill rather than a glyph, which keeps it to one font-independent
           rule. */
        label input {
            display: inline-block;
            width: 12dp;
            height: 12dp;
            vertical-align: -2dp;
            margin-right: 6dp;
            border: 1dp;
        }

        /* A card is a header and a stat block rather than a fixed-height row: the whole
           point of the stats being here is that several items can be read at once, which
           needs each card to be as tall as the item it describes. The client's own grid
           does the same. */
        .card {
            display: block;
            margin-bottom: 4dp;
            padding: 6dp;
            border-left: 3dp;
        }

        .cardhead { display: block; }

        .card .icon {
            display: inline-block;
            width: 36dp;
            height: 36dp;
            vertical-align: -12dp;
        }

        .cardtext { display: inline-block; margin-left: 8dp; }
        .name { display: block; }
        .meta { display: block; font-size: 11dp; }

        /* Indented under the header so the eye can run down one item's stats without
           picking up the next item's. */
        .stats {
            display: block;
            margin-left: 44dp;
            margin-top: 3dp;
        }

        .replica-row { display: block; margin-bottom: 1dp; }

        /* An explanation, not a stat, so it does not read as one. Distinguished by size and
           colour rather than by italics: only one font face is registered (see LoadFontFace,
           FontStyle::Normal), and RmlUi resolves font-style against the faces it has -- asking
           for italic finds none and silently draws nothing at all. */
        .nostats { display: block; font-size: 12dp; }
        .replica-blank { display: block; height: 8dp; }

        /* Ported from WebUI/src/containers/ReplicaStat.css: the item's own name and the
           "release ctrl" hint are the game's chrome, not stats. */
        .replica-type-3, .replica-type-4, .replica-type-5, .replica-type-6, .replica-type-7,
        .replica-type-13, .replica-type-14, .replica-type-35, .replica-type-64, .replica-type-77 {
            display: none;
        }

        .replica-type-24, .replica-type-25, .replica-type-34, .replica-type-36,
        .replica-type-65, .replica-type-67, .replica-type-68, .replica-type-70 {
            font-weight: bold;
        }

        .replica-type-26, .replica-type-33, .replica-type-42, .replica-type-55,
        .replica-type-69, .replica-type-71, .replica-type-83 {
            margin-left: 14dp;
        }

        .replica-type-37, .replica-type-38, .replica-type-53 { font-weight: bold; }

        #pager {
            display: block;
            position: absolute;
            left: 0dp;
            bottom: 0dp;
            width: 100%;
            height: 38dp;
            padding: 6dp;
        }
        #prev, #next {
            display: inline-block;
            padding: 4dp 14dp;
            margin-right: 8dp;
            border: 1dp;
        }
    </style>
</head>
<body>
    <div id="root">
        <div id="topbar">
            <input type="text" class="text" id="q" placeholder="Search by name or stat text"/>
            <input type="text" class="text" id="minlevel" placeholder="min"/>
            <input type="text" class="text" id="maxlevel" placeholder="max"/>
            <select id="quality"/>
            <select id="slot"/>
            <div id="status">Searching...</div>
            <div id="close" title="Close (Esc)">Close</div>
        </div>
        <div id="body">
            <div id="sidebar"/><div id="results"/><div id="scrolltrack"><div id="scrollthumb"/></div>
            <div id="detailcol">
                <div id="transfer-bar">
                    <div id="transfer">Transfer to stash</div>
                    <div id="transfer-status">Select an item.</div>
                </div>
                <div id="detail"/>
            </div>
        </div>
        <div id="pager">
            <div id="prev">Previous</div>
            <div id="next">Next</div>
        </div>
    </div>
</body>
</rml>
)RML";

/// <summary>
/// The two colour schemes, applied as a class on the body.
///
/// RCSS has no custom properties, so the palette cannot be swapped one variable at a time
/// the way the WebUI does it; each theme is written out in full instead. The colours are
/// the WebUI's, from WebUI/src/style/index.css -- the light block and the dark block of the
/// same file -- so an item reads the same in the overlay as it does in the client.
/// </summary>
const char* const kDarkTheme = R"RCSS(
#detailcol { background-color: #1b1710; }
#transfer { background-color: #2a2118; border-color: #6b5a3a; }
#transfer:hover { background-color: #4a3a22; }
#transfer-status { color: #999999; }
label input { background-color: #0d0b08; border-color: #c8a35a; }
label input:checked { background-color: #c8a35a; }
body { color: #cfc8bb; }
#root { background-color: #16130fe8; border-color: #6b5a3a; }
#topbar { background-color: #221c14; }
#topbar input.text, #topbar select { background-color: #0d0b08; border-color: #4a4034; color: #e8e0d0; }
#status { color: #999999; }
#sidebar { background-color: #1b1710; }
#detail { background-color: #1b1710; }
.grouphead { color: #e3ba6b; }
.card { background-color: #221c14; }
.card:hover { background-color: #33291f; }
.card.selected { background-color: #3d3223; }
.name { color: #dbb284; }
.meta { color: gray; }
.icon.noicon { background-color: #2b241a; }
#prev, #next, #close { background-color: #2a2118; border-color: #6b5a3a; }
#scrolltrack { background-color: #1b1710; }
#scrollthumb { background-color: #6b5a3a; }
#prev:hover, #next:hover, #close:hover { background-color: #4a3a22; }

.rarity-White { border-left-color: #b0b0b0; }
.rarity-Yellow { border-left-color: #ffff00; }
.rarity-Green { border-left-color: #2e8b57; }
.rarity-Blue { border-left-color: #4169e1; }
.rarity-Epic { border-left-color: #9932cc; }
.rarity-Legendary { border-left-color: #9932cc; }

.replica-row { color: gray; }
.nostats { color: #6f675b; }
.replica-type-16, .replica-type-17, .replica-type-30, .replica-type-32 { color: #b3af8c; }
.replica-type-66 { color: #dbb284; }
.replica-type-20 { color: gray; }
.replica-type-18, .replica-type-19, .replica-type-26, .replica-type-28, .replica-type-33,
.replica-type-42, .replica-type-55, .replica-type-69, .replica-type-71, .replica-type-81,
.replica-type-82, .replica-type-83 { color: #dbb284; }
.replica-type-24, .replica-type-25, .replica-type-34, .replica-type-36, .replica-type-65,
.replica-type-67, .replica-type-68, .replica-type-70 { color: #e3ba6b; }
.replica-type-37, .replica-type-38, .replica-type-53 { color: #6699ff; }
.replica-type-39, .replica-type-40, .replica-type-84 { color: #999999; }
.replica-type-79 { color: #dbb284; }
.replica-type-21 { color: #4fbcbf; }
.replica-type-22 { color: #b3af8c; }
.replica-type-23 { color: #e3ba6b; }
.replica-type-27, .replica-type-29, .replica-type-31 { color: #f1e51b; }

.replica-letter-E { color: #a88054; }
.replica-letter-H { color: #dbb284; }
.replica-letter-W { color: #dbb284; }
.replica-letter-S { color: #a88054; }
.replica-letter-Z { color: #338cce; }
.replica-letter-r { color: gray; }
.replica-letter-o { color: #e3ba6b; }
.replica-letter-O { color: #e3ba6b; }
.replica-letter-k { color: #999999; }
)RCSS";

const char* const kLightTheme = R"RCSS(
#detailcol { background-color: #e8dfcd; }
#transfer { background-color: #e2d8c4; border-color: #a08a5a; }
#transfer:hover { background-color: #d6c9ad; }
#transfer-status { color: #726a5b; }
label input { background-color: #fbf7ee; border-color: #7a4e14; }
label input:checked { background-color: #7a4e14; }
body { color: #4a4034; }
#root { background-color: #efe7d8f0; border-color: #a08a5a; }
#topbar { background-color: #e2d8c4; }
#topbar input.text, #topbar select { background-color: #fbf7ee; border-color: #b6a888; color: #33291f; }
#status { color: #726a5b; }
#sidebar { background-color: #e8dfcd; }
#detail { background-color: #e8dfcd; }
.grouphead { color: #7a4e14; }
.card { background-color: #e2d8c4; }
.card:hover { background-color: #d6c9ad; }
.card.selected { background-color: #cbba98; }
.name { color: #4a4034; }
.meta { color: #726a5b; }
.icon.noicon { background-color: #d6c9ad; }
#prev, #next, #close { background-color: #e2d8c4; border-color: #a08a5a; }
#scrolltrack { background-color: #e8dfcd; }
#scrollthumb { background-color: #a08a5a; }
#prev:hover, #next:hover, #close:hover { background-color: #d6c9ad; }

.rarity-White { border-left-color: #808080; }
.rarity-Yellow { border-left-color: #b8a000; }
.rarity-Green { border-left-color: #2e8b57; }
.rarity-Blue { border-left-color: #4169e1; }
.rarity-Epic { border-left-color: #9932cc; }
.rarity-Legendary { border-left-color: #9932cc; }

.replica-row { color: #655e52; }
.nostats { color: #8a8172; }
.replica-type-16, .replica-type-17, .replica-type-30, .replica-type-32 { color: #4a4034; }
.replica-type-66 { color: #4a4034; }
.replica-type-20 { color: #655e52; }
.replica-type-18, .replica-type-19, .replica-type-26, .replica-type-28, .replica-type-33,
.replica-type-42, .replica-type-55, .replica-type-69, .replica-type-71, .replica-type-81,
.replica-type-82, .replica-type-83 { color: #4a4034; }
.replica-type-24, .replica-type-25, .replica-type-34, .replica-type-36, .replica-type-65,
.replica-type-67, .replica-type-68, .replica-type-70 { color: #7a4e14; }
.replica-type-37, .replica-type-38, .replica-type-53 { color: #155d90; }
.replica-type-39, .replica-type-40, .replica-type-84 { color: #726a5b; }
.replica-type-79 { color: #4a4034; }
.replica-type-21 { color: #1a605c; }
.replica-type-22 { color: #4a4034; }
.replica-type-23 { color: #7a4e14; }
.replica-type-27, .replica-type-29, .replica-type-31 { color: #6b5c0a; }

.replica-letter-E { color: #33291f; }
.replica-letter-H { color: #4a4034; }
.replica-letter-W { color: #4a4034; }
.replica-letter-S { color: #33291f; }
.replica-letter-Z { color: #155d90; }
.replica-letter-r { color: #655e52; }
.replica-letter-o { color: #7a4e14; }
.replica-letter-O { color: #7a4e14; }
.replica-letter-k { color: #726a5b; }
)RCSS";

/// Fills in the parts of the document that are generated from the ported tables, so the
/// tables stay the single place a filter or a slot is defined.
void PopulateControls(Rml::ElementDocument* document) {
    ShellState& s = state();

    std::ostringstream sidebar;

    sidebar << "<div class=\"grouphead\">Damage</div>";
    for (size_t i = 0; i < sizeof(kDamageFilters) / sizeof(kDamageFilters[0]); i++) {
        sidebar << "<label><input type=\"checkbox\" id=\"dmg_" << i << "\"/>"
                << Escape(kDamageFilters[i].label) << "</label>";
    }

    sidebar << "<div class=\"grouphead\">Resistances</div>";
    for (size_t i = 0; i < sizeof(kResistanceFilters) / sizeof(kResistanceFilters[0]); i++) {
        sidebar << "<label><input type=\"checkbox\" id=\"res_" << i << "\"/>"
                << Escape(kResistanceFilters[i].label) << "</label>";
    }

    if (Rml::Element* element = document->GetElementById("sidebar")) {
        element->SetInnerRML(sidebar.str());
    }

    // Through the select's own Add rather than SetInnerRML: a drop-down owns its option
    // elements and rebuilds them from its own list, so RML written into it is discarded.
    if (auto* quality = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("quality"))) {
        for (size_t i = 0; i < sizeof(kQualityOptions) / sizeof(kQualityOptions[0]); i++) {
            quality->Add(Escape(kQualityOptions[i].label), std::to_string(i));
        }
        quality->SetSelection(0);
    }

    if (auto* slot = rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(document->GetElementById("slot"))) {
        for (size_t i = 0; i < sizeof(kSlotOptions) / sizeof(kSlotOptions[0]); i++) {
            slot->Add(Escape(kSlotOptions[i].label), std::to_string(i));
        }
        slot->SetSelection(0);
    }

    s.damageChecked.assign(sizeof(kDamageFilters) / sizeof(kDamageFilters[0]), false);
    s.resistanceChecked.assign(sizeof(kResistanceFilters) / sizeof(kResistanceFilters[0]), false);
}

}  // namespace

const char* OverlayShell::DocumentRml() {
    static std::string document;

    if (!document.empty()) {
        return document.c_str();
    }

    ShellState& s = state();

    try {
        SettingsReader settings;
        s.isDarkMode = settings.GetIsDarkMode();
    }
    catch (...) {
        LogToFile(LogLevel::WARNING, "Overlay shell: could not read the dark-mode setting, using dark.");
        s.isDarkMode = true;
    }

    // The theme is spliced into the document's own style block rather than added to the DOM
    // afterwards. RmlUi hands a <style> element to the style-sheet parser while it parses
    // the document; one appended to a live document is just an element with text in it.
    const std::string base(kDocument);
    const size_t insertAt = base.find("</style>");

    if (insertAt == std::string::npos) {
        document = base;
    }
    else {
        document = base.substr(0, insertAt)
            + (s.isDarkMode ? kDarkTheme : kLightTheme)
            + base.substr(insertAt);
    }

    return document.c_str();
}

void OverlayShell::Bind(Rml::ElementDocument* document) {
    ShellState& s = state();
    s.document = document;

    if (document == nullptr) {
        return;
    }

    PopulateControls(document);

    document->AddEventListener(Rml::EventId::Change, &listener());
    document->AddEventListener(Rml::EventId::Click, &listener());

    s.skip = 0;
    s.selectedIndex = -1;
    s.stacks.clear();
    s.lastDataVersionChanges = OverlaySearch::DataVersionChanges();

    RunSearch();

    LogToFile(LogLevel::INFO, std::string("Overlay shell: bound, theme is ") + (s.isDarkMode ? "dark." : "light."));
}

void OverlayShell::Unbind() {
    ShellState& s = state();

    if (s.document != nullptr) {
        s.document->RemoveEventListener(Rml::EventId::Change, &listener());
        s.document->RemoveEventListener(Rml::EventId::Click, &listener());
    }

    s.document = nullptr;
    s.stacks.clear();
    s.selectedIndex = -1;
}

void OverlayShell::Update() {
    ShellState& s = state();
    if (s.document == nullptr) {
        return;
    }

    const unsigned long tick = ::GetTickCount();

    // The client has written to the database. Whatever is on screen may name an item that
    // has just been transferred away, so ask again rather than leave it there -- but not
    // yet, and not once per commit. See kRefreshQuietMilliseconds.
    const unsigned long long changes = OverlaySearch::DataVersionChanges();
    if (changes != s.lastDataVersionChanges) {
        s.lastDataVersionChanges = changes;

        if (!s.hasDeferredRefresh) {
            s.hasDeferredRefresh = true;
            s.refreshDeferredSinceTick = tick;
        }

        s.refreshQuietUntilTick = tick + kRefreshQuietMilliseconds;
    }

    if (s.hasDeferredRefresh) {
        const bool hasGoneQuiet = (long)(tick - s.refreshQuietUntilTick) >= 0;
        const bool hasWaitedLongEnough =
            (long)(tick - (s.refreshDeferredSinceTick + kRefreshMaximumDeferMilliseconds)) >= 0;

        if (hasGoneQuiet || hasWaitedLongEnough) {
            s.hasDeferredRefresh = false;
            ScheduleRefresh();
        }
    }

    if (s.hasScheduledSearch) {
        if ((long)(tick - s.scheduledAtTick) >= 0) {
            RunSearch(s.scheduledPreservesView);
        }
    }

    if (OverlaySearchResultPtr result = OverlaySearch::TakeResult()) {
        if (result->ok) {
            RenderResults(*result);
        }
        else {
            s.stacks.clear();
            SetStatus("The search failed: " + result->error);
        }
    }

    // The stat rows for the cards on screen. Applied to the existing cards rather than by
    // redrawing them, so the grid does not flicker when the stats arrive.
    if (OverlayPageDetailsPtr page = OverlaySearch::TakePageDetails()) {
        if (page->ok && page->generation == OverlaySearch::CurrentGeneration()) {
            for (auto& entry : page->byItem) {
                s.pageDetails[entry.first] = entry.second;
            }
            ApplyPageDetails();
        }
    }

    if (OverlayItemDetailPtr detail = OverlaySearch::TakeDetail()) {
        RenderDetail(*detail);
    }

    // The worker has finished reading an item the player asked for. Handing it on here
    // rather than from the worker keeps the database on one thread and the game on
    // another, with the frame in between.
    OverlayTransferRequest request;
    if (OverlaySearch::TakeTransferRequest(request)) {
        OverlayTransfer::Request(request);
    }

    const std::string transferStatus = OverlayTransfer::TakeStatus();
    if (!transferStatus.empty()) {
        SetTransferStatus(transferStatus);
    }

    UpdateScrollThumb();

    // Last, after the stat rows have been written into the cards and changed their heights:
    // put the scroll back where the refresh found it. See restoreScrollFrames.
    if (s.restoreScrollFrames > 0) {
        s.restoreScrollFrames--;

        if (Rml::Element* results = s.document->GetElementById("results")) {
            if (results->GetScrollTop() != s.restoreScrollTop) {
                results->SetScrollTop(s.restoreScrollTop);
            }
        }
    }
}
