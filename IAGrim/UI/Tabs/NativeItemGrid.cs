using System.ComponentModel;
using IAGrim.UI.Controller.dto;
using log4net;

namespace IAGrim.UI.Tabs {

    /// <summary>
    /// A plain WinForms replacement for the WebView2 item grid.
    ///
    /// WHY THIS EXISTS: under Wine (box64 + x86_64 Wine on ARM64), WebView2 loads and runs the page --
    /// navigation completes, the SPA's JavaScript executes and calls back into the host -- but the browser
    /// never presents a single pixel into its child HWND, so the item grid is permanently blank while the
    /// rest of the WinForms chrome paints fine. Verified not to be a flag problem: standalone Edge in the
    /// same prefix paints correctly with --no-sandbox --disable-gpu, while IAGD stays blank with that exact
    /// combination, with single-process disabled, with DirectComposition disabled, and inside a Wine virtual
    /// desktop. The failing piece is WebView2-as-a-child-window, which nothing in IAGD can influence.
    ///
    /// So this control mirrors the same item stream the SPA receives (CefBrowserHandler.NativeItemSink) and
    /// renders it with controls Wine is happy to draw. It is deliberately fed from the existing pipeline
    /// rather than querying the DAOs itself: filters, paging, mod/hardcore scoping and merging all keep
    /// working exactly as they do for the web UI.
    /// </summary>
    public class NativeItemGrid : UserControl {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(NativeItemGrid));
        private readonly DataGridView _grid;
        private readonly RichTextBox _details;
        private readonly SplitContainer _split;

        /// <summary>Rows currently displayed, in display order. Index-aligned with the grid's rows.</summary>
        private readonly List<ItemRow> _rows = new();

        private string? _sortColumn;
        private bool _sortAscending = true;
        private bool _userSizedSplitter;
        private bool _inSetBlock;
        private readonly HashSet<string> _tickedMembers = new(StringComparer.OrdinalIgnoreCase);
        private bool _applyingSplit;
        private bool _syncedDetailsFont;
        private bool _firstFillRepeated;
        private readonly bool _darkMode;
        private Color _foreColour;
        private Color _mutedColour;

        /// <summary>Set details for the selected item; null until SplitSearchWindow hands one over.</summary>
        public Database.DAO.SetLookup? SetLookup;

        /// <summary>Drop sources / blueprints, loaded off the UI thread; null until it is ready.</summary>
        public Parsers.Arz.LootGraph? LootGraph;

        /// <summary>Tag -> display name, for turning a creature's description tag into its name.</summary>
        public IReadOnlyDictionary<string, string>? Tags;

        /// <summary>True while the grid is showing the game's item database instead of the player's stash.</summary>
        public bool DatabaseMode { get; private set; }

        /// <summary>Roll bands for a base record, used to show "21/31" instead of one sampled value.</summary>
        public Func<string, IReadOnlyDictionary<double, (double Min, double Max)>>? RangeProvider;

        /// <summary>Ranges for the selected item only; rebuilt on each selection change.</summary>
        private IReadOnlyDictionary<double, (double Min, double Max)>? _ranges;

        /// <summary>
        /// Range lookups hit the database, and binding 500 rows fires SelectionChanged repeatedly, so an
        /// uncached lookup turns one listing into hundreds of queries on the UI thread.
        /// </summary>
        private readonly Dictionary<string, IReadOnlyDictionary<double, (double Min, double Max)>> _rangeCache = new();

        /// <summary>Set while the grid is being rebound, so the details pane is not rebuilt per intermediate selection.</summary>
        private bool _binding;

        /// <summary>Mirrors the toolbar's "Order By Level": adds level as a secondary sort under any column.</summary>
        public bool SortByLevelSecondary { get; set; }

        /// <summary>Re-sorts the current rows in place, for when the sort options change but the data has not.</summary>
        public void RefreshSort() {
            if (_rows.Count > 0) {
                ApplySortAndBind();
            }
        }

        /// <summary>Raised on double click: (item, transferAll). Wire to JavascriptIntegration.TransferItem.</summary>
        public Action<JsonItem, bool>? OnTransfer;

        public NativeItemGrid(bool darkMode) {
            _darkMode = darkMode;
            Dock = DockStyle.Fill;

            _grid = new DataGridView {
                Dock = DockStyle.Fill,
                ReadOnly = true,
                AllowUserToAddRows = false,
                AllowUserToDeleteRows = false,
                AllowUserToResizeRows = false,
                AutoGenerateColumns = false,
                SelectionMode = DataGridViewSelectionMode.FullRowSelect,
                MultiSelect = false,
                RowHeadersVisible = false,
                AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill,
                BorderStyle = BorderStyle.None,
                EnableHeadersVisualStyles = false,
            };

            // Tall enough for the item icons; GD icons are up to 64x128 and get zoomed to fit.
            // The row has to grow with IAGD_UI_SCALE too, or a larger font just clips inside a 44px row.
            _grid.RowTemplate.Height = (int)(44 * Program.UiScale);

            _grid.Columns.Add(new DataGridViewImageColumn {
                Name = nameof(ItemRow.Icon),
                DataPropertyName = nameof(ItemRow.Icon),
                HeaderText = string.Empty,
                // Icons zoom to the row height, so the column needs a proportionally wider share as rows grow;
                // a fixed weight would letterbox them in a narrow strip at higher scales.
                FillWeight = 7 * Program.UiScale,
                ImageLayout = DataGridViewImageCellLayout.Zoom,
                SortMode = DataGridViewColumnSortMode.NotSortable,
                ReadOnly = true,
                // Without this a null icon renders as an error glyph rather than an empty cell.
                DefaultCellStyle = { NullValue = null },
            });

            AddColumn(nameof(ItemRow.Name), "Name", 45);
            AddColumn(nameof(ItemRow.Quality), "Rarity", 12);
            AddColumn(nameof(ItemRow.Level), "Level", 8);
            AddColumn(nameof(ItemRow.Slot), "Slot", 20);
            AddColumn(nameof(ItemRow.Count), "#", 6);

            _details = new RichTextBox {
                Dock = DockStyle.Fill,
                ReadOnly = true,
                BorderStyle = BorderStyle.None,
                TabStop = false,
                DetectUrls = false,
                WordWrap = true,
                ScrollBars = RichTextBoxScrollBars.Vertical,
            };

            // Vertical split: list left, stats right. On an ultrawide the list never needs the full width,
            // and a stats pane along the bottom wastes the screen's long axis while truncating long tooltips.
            _split = new SplitContainer {
                Dock = DockStyle.Fill,
                Orientation = Orientation.Vertical,
                SplitterWidth = 4,
            };
            _split.Panel1.Controls.Add(_grid);
            _split.Panel2.Controls.Add(_details);
            Controls.Add(_split);

            // Keep the 72/28 split proportional as the window grows (maximising an ultrawide otherwise leaves
            // the stats pane at whatever pixel width the restored window happened to have) - but stop the
            // moment the user drags the splitter themselves.
            // SplitterMoving also fires for programmatic SplitterDistance changes, so without the guard the
            // first default placement marks the splitter as "user sized" and it never tracks the window again.
            _split.SplitterMoving += (_, _) => {
                if (!_applyingSplit) {
                    _userSizedSplitter = true;
                }
            };
            _split.SizeChanged += (_, _) => ApplyDefaultSplit();

            _grid.ContextMenuStrip = BuildContextMenu();
            _grid.CellMouseDown += (_, e) => {
                // Right-click should act on the row under the cursor, not whatever was selected before.
                if (e.Button == MouseButtons.Right && e.RowIndex >= 0 && e.ColumnIndex >= 0) {
                    _grid.CurrentCell = _grid.Rows[e.RowIndex].Cells[e.ColumnIndex];
                }
            };

            _grid.SelectionChanged += (_, _) => ShowDetailsForSelection();
            _grid.CellFormatting += GridOnCellFormatting;
            _grid.CellDoubleClick += GridOnCellDoubleClick;
            _grid.ColumnHeaderMouseClick += GridOnColumnHeaderMouseClick;

            ApplyTheme();
        }

        /// <summary>
        /// Right-click actions for a row.
        ///
        /// "Look up on GrimTools" opens the user's browser at grimtools.com's search for the item name rather
        /// than fetching anything itself. Two reasons: their robots.txt carries "Disallow: /db/search" for every
        /// user agent, and the results are not in the served HTML anyway - the page ships a JS bundle that
        /// renders them client-side, so an HTTP fetch returns an empty shell. Opening a browser is the user
        /// navigating there, which is what the site is built for.
        /// </summary>
        private ContextMenuStrip BuildContextMenu() {
            var menu = new ContextMenuStrip();

            var lookup = new ToolStripMenuItem("Look up on GrimTools");
            lookup.Click += (_, _) => WithSelectedRow(row =>
                OpenUrl("https://www.grimtools.com/db/search/?query=" + Uri.EscapeDataString(row.Name) + "&exact_match=1"));

            var copy = new ToolStripMenuItem("Copy item name");
            copy.Click += (_, _) => WithSelectedRow(row => Clipboard.SetText(row.Name));

            menu.Items.Add(lookup);
            menu.Items.Add(copy);
            return menu;
        }

        /// <summary>
        /// Opens a URL in the user's browser.
        ///
        /// Under Wine the normal route (ShellExecute -> winebrowser) silently does nothing in this prefix: the
        /// call returns without an exception and no browser ever appears. Handing the URL to the host's
        /// xdg-open through start.exe /unix does work, so Wine gets that path and everything else keeps the
        /// standard one.
        /// </summary>
        private static void OpenUrl(string url) {
            try {
                if (Services.WineDetector.IsRunningInWine()) {
                    System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo {
                        FileName = "start.exe",
                        Arguments = $"/unix /usr/bin/xdg-open \"{url}\"",
                        UseShellExecute = false,
                        CreateNoWindow = true,
                    });

                    Logger.Info($"Opened {url} via xdg-open");
                    return;
                }

                System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo { FileName = url, UseShellExecute = true });
            }
            catch (Exception ex) {
                Logger.Warn($"Could not open {url}: {ex.Message}");
            }
        }

        private void WithSelectedRow(Action<ItemRow> action) {
            var index = _grid.CurrentRow?.Index ?? -1;
            if (index >= 0 && index < _rows.Count) {
                action(_rows[index]);
            }
        }

        protected override void OnHandleCreated(EventArgs e) {
            base.OnHandleCreated(e);
            ApplyDefaultSplit();

        }

        /// <summary>
        /// Matches the stats pane to the grid's cell font, once, after UiScaler has run.
        ///
        /// DataGridView materialises DefaultCellStyle.Font from the control font, so by the time the scaler
        /// reaches it the value is already scaled once by inheritance and gets scaled again - cells end up at
        /// scale^2 (21.12pt at 1.6x) while everything else sits at scale (13.2pt). That larger size is the
        /// right one for the item list, so rather than undo it, the stats pane is pinned to the same value:
        /// list and stats are the content, the filter panel and tabs stay one step smaller as chrome.
        /// </summary>
        private bool SyncDetailsFontOnce() {
            if (_syncedDetailsFont) {
                return false;
            }

            _syncedDetailsFont = true;

            if (_grid.DefaultCellStyle.Font is { } cellFont && Math.Abs(cellFont.Size - _details.Font.Size) > 0.1f) {
                _details.Font = new Font(cellFont.FontFamily, cellFont.Size, cellFont.Style);
                return true;
            }

            return false;
        }

        /// <summary>
        /// Puts the splitter at 72% of the width. Only meaningful once the control has a width to divide,
        /// which is why it is not done in the constructor.
        /// </summary>
        private void ApplyDefaultSplit() {
            if (_userSizedSplitter || _split.Width <= 600) {
                return;
            }

            var distance = (int)(_split.Width * 0.72);

            // SplitterDistance throws if it cannot be honoured given the panel minimums.
            var max = _split.Width - _split.Panel2MinSize - _split.SplitterWidth;
            if (distance <= _split.Panel1MinSize || distance >= max) {
                return;
            }

            _applyingSplit = true;
            try {
                _split.SplitterDistance = distance;
            }
            finally {
                _applyingSplit = false;
            }
        }

        private void AddColumn(string property, string header, int fillWeight) {
            _grid.Columns.Add(new DataGridViewTextBoxColumn {
                Name = property,
                DataPropertyName = property,
                HeaderText = header,
                FillWeight = fillWeight,
                SortMode = DataGridViewColumnSortMode.Programmatic,
                ReadOnly = true,
            });
        }

        private void ApplyTheme() {
            var back = _darkMode ? Color.FromArgb(30, 30, 30) : Color.FromArgb(245, 245, 245);
            var fore = _darkMode ? Color.FromArgb(220, 220, 220) : Color.FromArgb(20, 20, 20);
            var header = _darkMode ? Color.FromArgb(45, 45, 45) : Color.FromArgb(225, 225, 225);

            _foreColour = fore;
            _mutedColour = _darkMode ? Color.FromArgb(153, 153, 153) : Color.FromArgb(114, 106, 91);

            BackColor = back;
            _split.BackColor = back;
            _details.BackColor = back;
            _details.ForeColor = fore;

            _grid.BackgroundColor = back;
            _grid.GridColor = header;
            _grid.DefaultCellStyle.BackColor = back;
            _grid.DefaultCellStyle.ForeColor = fore;
            _grid.DefaultCellStyle.SelectionBackColor = _darkMode ? Color.FromArgb(70, 70, 90) : Color.FromArgb(200, 210, 235);
            _grid.DefaultCellStyle.SelectionForeColor = fore;
            _grid.ColumnHeadersDefaultCellStyle.BackColor = header;
            _grid.ColumnHeadersDefaultCellStyle.ForeColor = fore;
            _grid.ColumnHeadersDefaultCellStyle.SelectionBackColor = header;
        }

        /// <summary>
        /// Replaces the grid contents with rows from the game's item database (every item Grim Dawn defines),
        /// rather than the player's own items.
        ///
        /// The rows are wrapped in the same JsonItem the stash path uses, so icons, rarity colouring, sorting,
        /// the set block and "Obtained from" all keep working unchanged - every one of those is keyed on the
        /// base record, which a database row has. What it cannot have is a stat replica: those are captured
        /// from the live game as an item is looted, so an item never held has no tooltip to show.
        /// </summary>
        public void ShowDatabaseItems(List<List<JsonItem>> groups) {
            if (InvokeRequired) {
                // Decode the icons here, on the caller's background thread: building the rows on the UI
                // thread would otherwise pull 500 PNGs off disk inside the message loop.
                foreach (var group in groups) {
                    if (group.Count > 0) {
                        LoadIcon(group[0].Icon);
                    }
                }

                BeginInvoke(new Action(() => ShowDatabaseItems(groups)));
                return;
            }

            DatabaseMode = true;

            // "#" is the stack count of owned copies; every database row is a definition, so it would read 1
            // on every line.
            _grid.Columns[nameof(ItemRow.Count)].Visible = false;

            _rows.Clear();

            foreach (var group in groups) {
                if (group.Count > 0) {
                    _rows.Add(new ItemRow(group[0], 1));
                }
            }

            ApplySortAndBind();
        }

        public void LeaveDatabaseMode() {
            DatabaseMode = false;
            _grid.Columns[nameof(ItemRow.Count)].Visible = true;
        }

        /// <summary>
        /// Mirrors CefBrowserHandler.SetItems/AddItems. <paramref name="groups"/> is the same shape the web UI
        /// gets: one inner list per "merged" stack of identical items, so the stack size is the inner count.
        /// </summary>
        public void SetItems(List<List<JsonItem>> groups, bool replaceExisting, int numItemsFound) {
            if (InvokeRequired) {
                BeginInvoke(new Action(() => SetItems(groups, replaceExisting, numItemsFound)));
                return;
            }

            // Assigning a RichTextBox's Font resets the character formatting of everything already in it,
            // which is why the FIRST details render came out uncoloured: the pane had been written before the
            // font was synced to the grid's. Re-render once after it settles.
            var fontChanged = SyncDetailsFontOnce();

            // A search result arriving from the stash pipeline would otherwise wipe a database listing.
            if (DatabaseMode) {
                return;
            }

            if (replaceExisting) {
                _rows.Clear();
            }

            foreach (var group in groups) {
                if (group.Count == 0) {
                    continue;
                }

                _rows.Add(new ItemRow(group[0], group.Count));
            }

            ApplySortAndBind();

            if (fontChanged) {
                ShowDetailsForSelection();
            }
        }

        private void ApplySortAndBind() {
            if (_sortColumn != null) {
                Comparison<ItemRow> cmp = _sortColumn switch {
                    nameof(ItemRow.Level) => (a, b) => a.Level.CompareTo(b.Level),
                    nameof(ItemRow.Count) => (a, b) => a.Count.CompareTo(b.Count),
                    nameof(ItemRow.Quality) => (a, b) => string.Compare(a.Quality, b.Quality, StringComparison.OrdinalIgnoreCase),
                    nameof(ItemRow.Slot) => (a, b) => string.Compare(a.Slot, b.Slot, StringComparison.OrdinalIgnoreCase),
                    _ => (a, b) => string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase),
                };

                _rows.Sort((a, b) => {
                    var primary = _sortAscending ? cmp(a, b) : cmp(b, a);
                    if (primary != 0) {
                        return primary;
                    }

                    // "Order By Level" is a tie-breaker, not a replacement: sorting by Name with it ticked
                    // gives name-then-level, which is what makes the tiers of a same-named item readable.
                    // Level always ascends here - reversing the name order should not put level 94 first.
                    if (SortByLevelSecondary && _sortColumn != nameof(ItemRow.Level)) {
                        var byLevel = a.Level.CompareTo(b.Level);
                        if (byLevel != 0) {
                            return byLevel;
                        }
                    }

                    return string.Compare(a.Name, b.Name, StringComparison.OrdinalIgnoreCase);
                });
            }

            // Rebinding is cheap at these sizes (a page is capped at 1000 rows) and avoids having to
            // implement IBindingList just to get sorting. Binding raises SelectionChanged repeatedly as rows
            // are added, and rebuilding the details pane each time (set lookup, loot graph, range query) is
            // what made a 500-row listing crawl - so the pane is left alone until the bind is finished.
            _binding = true;
            try {
                _grid.DataSource = null;
                _grid.DataSource = new BindingSource { DataSource = new BindingList<ItemRow>(_rows) };
            }
            finally {
                _binding = false;
            }

            foreach (DataGridViewColumn column in _grid.Columns) {
                column.HeaderCell.SortGlyphDirection = column.Name == _sortColumn
                    ? (_sortAscending ? SortOrder.Ascending : SortOrder.Descending)
                    : SortOrder.None;
            }

            ShowDetailsForSelection();
        }

        private void GridOnColumnHeaderMouseClick(object? sender, DataGridViewCellMouseEventArgs e) {
            var column = _grid.Columns[e.ColumnIndex].Name;
            if (column == _sortColumn) {
                _sortAscending = !_sortAscending;
            }
            else {
                _sortColumn = column;
                _sortAscending = true;
            }

            ApplySortAndBind();
        }

        /// <summary>Tints rows with Grim Dawn's rarity colours, the one cue the text columns cannot carry.</summary>
        private void GridOnCellFormatting(object? sender, DataGridViewCellFormattingEventArgs e) {
            if (e.RowIndex < 0 || e.RowIndex >= _rows.Count) {
                return;
            }

            var colour = RarityColour(_rows[e.RowIndex].Quality);

            if (colour.HasValue) {
                e.CellStyle!.ForeColor = colour.Value;
                e.CellStyle.SelectionForeColor = colour.Value;
            }
        }

        private void GridOnCellDoubleClick(object? sender, DataGridViewCellEventArgs e) {
            if (e.RowIndex < 0 || e.RowIndex >= _rows.Count) {
                return;
            }

            // Shift sends the whole stack, matching the web UI's "transfer all" affordance.
            var transferAll = ModifierKeys.HasFlag(Keys.Shift);
            OnTransfer?.Invoke(_rows[e.RowIndex].Item, transferAll);
        }

        private void ShowDetailsForSelection() {
            if (_binding) {
                return;
            }

            _details.Clear();

            var index = _grid.CurrentRow?.Index ?? -1;
            if (index < 0 || index >= _rows.Count) {
                return;
            }

            var row = _rows[index];

            // Owned items carry the game's own rolled values, so a range would be wrong for them; only a
            // database item (no seed, showing base values) gets one.
            _ranges = DatabaseMode && RangeProvider != null && row.Item.BaseRecord != null
                ? RangesFor(row.Item.BaseRecord!)
                : null;

            _details.SuspendLayout();

            AppendRun(row.Name + Environment.NewLine, RarityColour(row.Quality) ?? _foreColour);
            if (row.Count > 1) {
                AppendRun($"Stack of {row.Count}{Environment.NewLine}", _mutedColour);
            }

            // The game's own tooltip already carries the set block (name, members, "(2) Set" tiers). Rather than
            // appending a second copy, the set data is used to colour those lines and mark the pieces already
            // in the collection - which is the one thing neither the tooltip nor GrimTools can know.
            var set = SetLookup?.Find(row.Item.BaseRecord);
            _inSetBlock = false;
            _tickedMembers.Clear();

            var diagLines = row.DescribeLines(_ranges).ToList();
            Logger.Info($"[details] {row.Name} lines={diagLines.Count} withCodes={diagLines.Count(l => l.Contains('^'))} " +
                        $"replica={row.Item.ReplicaStats?.Count ?? -1} header={row.Item.HeaderStats?.Count ?? -1} " +
                        $"body={row.Item.BodyStats?.Count ?? -1} handle={_details.IsHandleCreated} visible={_details.Visible} vis2={Visible}");

            foreach (var line in diagLines) {
                if (set == null || !TryAppendSetLine(line, set)) {
                    AppendColoured(line);
                }
            }

            AppendSourceSection(row);

            if (set != null) {
                var owned = set.Members.Count(m => m.Owned);
                AppendRun($"{owned} of {set.Members.Count} pieces collected{Environment.NewLine}",
                    owned == set.Members.Count ? Color.FromArgb(112, 200, 112) : _mutedColour);
            }

            _details.ResumeLayout();

            // Wine's RichEdit stamps its own default character format over the first content it is given,
            // discarding the colour runs (the stored RTF still has them - only the painted output is plain).
            // Filling the pane this early used to be impossible: WebView2 delayed the first search by ~14s,
            // by which point the control was long since realised. Now the list arrives in ~5s and the race
            // shows, so the very first fill is repeated once the control has settled. RichTextBox is a native
            // control and does not raise Paint, hence a timer rather than an event.
            if (!_firstFillRepeated) {
                _firstFillRepeated = true;
                var settle = new System.Windows.Forms.Timer { Interval = 400 };
                settle.Tick += (_, _) => {
                    settle.Stop();
                    settle.Dispose();
                    ShowDetailsForSelection();
                };

                settle.Start();
            }
            _details.SelectionStart = 0;
            _details.ScrollToCaret();


        }

        /// <summary>
        /// "Dropped by" / "Crafted from", read out of the game's own loot tables (see LootGraph).
        ///
        /// Most items legitimately have neither: Grim Dawn rolls the bulk of its gear from level-appropriate
        /// dynamic pools with no named source, which is the same thing GrimTools reports as "Random drop".
        /// Saying so explicitly is better than an empty space that reads like missing data.
        /// </summary>
        private void AppendSourceSection(ItemRow row) {
            if (LootGraph is not { IsLoaded: true }) {
                return;
            }

            var record = row.Item.BaseRecord;
            var sources = LootGraph.SourcesFor(record);
            var blueprints = LootGraph.BlueprintsFor(record);

            var heading = _darkMode ? Color.FromArgb(0xa8, 0x80, 0x54) : Color.FromArgb(0x33, 0x29, 0x1f);

            AppendRun(Environment.NewLine + "Obtained from" + Environment.NewLine, heading);

            foreach (var creature in sources) {
                AppendRun("  " + DisplayName(creature) + Environment.NewLine, _foreColour);
            }

            foreach (var blueprint in blueprints) {
                AppendRun("  Blueprint: " + DisplayName(blueprint) + Environment.NewLine, _foreColour);
            }

            if (sources.Count == 0 && blueprints.Count == 0) {
                AppendRun("  Random drop" + Environment.NewLine, _mutedColour);
            }
        }

        /// <summary>
        /// Creature records hold a description TAG, not a name; ItemTag carries the translation the game
        /// shows ("tagNemesis_Aetherial01" -> "Valdaran, the Storm Scourge"). Records with no tag fall back to
        /// the file name, which is still more use than nothing.
        /// </summary>
        private string DisplayName(string record) {
            return LootGraph?.SourceLabel(record, Tags) ?? record;
        }

        /// <summary>
        /// Colours a line that belongs to the tooltip's set block, and reports whether it did.
        ///
        /// Grim Dawn emits the set name, its member list and the "(N) Set" bonus tiers as plain lines with no
        /// ^-codes, so they arrive uncoloured. Matching them against the parsed set record lets them be tinted
        /// the way the web UI (and GrimTools) shows them - set name in teal, pieces in the set colour - and
        /// lets each piece be ticked when it is already in the user's collection.
        /// </summary>
        private bool TryAppendSetLine(string line, Database.DAO.ItemSetDetails set) {
            var text = StripCodes(line).Trim();
            if (text.Length == 0) {
                return false;
            }

            var setName = _darkMode ? Color.FromArgb(0x4f, 0xbc, 0xbf) : Color.FromArgb(0x1a, 0x60, 0x5c);
            var memberColour = _darkMode ? Color.FromArgb(0xdb, 0xb2, 0x84) : Color.FromArgb(0x3f, 0x35, 0x27);
            var ownedColour = Color.FromArgb(112, 200, 112);

            // Outside the set block nothing is matched, because a set's granted skill is frequently named after
            // one of its pieces: "Apothecary's Touch" is both a glove and the skill that set grants, and matching
            // member names anywhere in the tooltip ticked the Granted Skills line as if it were the item.
            if (!_inSetBlock) {
                if (string.Equals(text, set.SetName, StringComparison.OrdinalIgnoreCase)) {
                    _inSetBlock = true;
                    AppendRun(text + Environment.NewLine, setName);
                    return true;
                }

                return false;
            }

            // The block runs from the set name to the end of the bonus tiers; the requirements that follow it
            // belong to the item again.
            if (text.StartsWith("Required ", StringComparison.OrdinalIgnoreCase)) {
                _inSetBlock = false;
                return false;
            }

            // Each piece is listed once. Guarding against a repeat keeps a later line that happens to share a
            // piece's name from being ticked a second time.
            var member = set.Members.FirstOrDefault(m =>
                string.Equals(m.Name, text, StringComparison.OrdinalIgnoreCase) && !_tickedMembers.Contains(m.Name));

            if (member != null) {
                _tickedMembers.Add(member.Name);
                AppendRun(member.Owned ? "  [x] " : "  [ ] ", member.Owned ? ownedColour : _mutedColour);
                AppendRun(text + Environment.NewLine, member.Owned ? ownedColour : memberColour);
                return true;
            }

            // Tier headers: "(2) Set", "(3) Set", ...
            if (System.Text.RegularExpressions.Regex.IsMatch(text, @"^\(\d+\)\s+Set$")) {
                AppendRun(text + Environment.NewLine, setName);
                return true;
            }

            return false;
        }

        private static string StripCodes(string text) {
            return System.Text.RegularExpressions.Regex.Replace(text, @"\{?\^.\}?", string.Empty);
        }

        private IReadOnlyDictionary<double, (double Min, double Max)> RangesFor(string baseRecord) {
            if (_rangeCache.TryGetValue(baseRecord, out var cached)) {
                return cached;
            }

            var ranges = RangeProvider!(baseRecord);
            _rangeCache[baseRecord] = ranges;
            return ranges;
        }

        /// <summary>
        /// Writes one tooltip line, honouring the ^-codes Grim Dawn embeds in it.
        ///
        /// The game marks up its own tooltips ("+91% ^EFire Damage", "+3 ^Eto ^ZPanetti's Replicating Missile"):
        /// a caret plus one letter switches colour for the rest of the segment. The web UI turns those into CSS
        /// classes (WebUI/src/components/Item/ReplicaStat.tsx); here they become RichTextBox colour runs, using
        /// the same palette its stylesheet does. Codes we have no colour for fall back to the body colour, so an
        /// unrecognised one loses the tint rather than printing a stray "^X".
        /// </summary>
        private void AppendColoured(string line) {
            var colour = _foreColour;
            var run = new System.Text.StringBuilder();

            for (var i = 0; i < line.Length; i++) {
                var c = line[i];

                // The codes are sometimes wrapped in braces: "{^E}".
                if ((c == '{' || c == '}') && i + 1 < line.Length && (line[i + 1] == '^' || c == '}')) {
                    continue;
                }

                if (c == '^' && i + 1 < line.Length) {
                    if (run.Length > 0) {
                        AppendRun(run.ToString(), colour);
                        run.Clear();
                    }

                    colour = CodeColour(line[++i]);
                    continue;
                }

                run.Append(c);
            }

            AppendRun(run.ToString() + Environment.NewLine, colour);
        }

        private void AppendRun(string text, Color colour) {
            if (text.Length == 0) {
                return;
            }

            _details.SelectionStart = _details.TextLength;
            _details.SelectionLength = 0;
            _details.SelectionColor = colour;
            _details.AppendText(text);
        }

        /// <summary>Palette lifted from WebUI/src/containers/ReplicaStat.css and index.css.</summary>
        private Color CodeColour(char code) => code switch {
            'E' or 'S' => _darkMode ? Color.FromArgb(0xa8, 0x80, 0x54) : Color.FromArgb(0x33, 0x29, 0x1f),
            'H' or 'W' => _darkMode ? Color.FromArgb(0xdb, 0xb2, 0x84) : Color.FromArgb(0x4a, 0x40, 0x34),
            'Z' => _darkMode ? Color.FromArgb(0x33, 0x8c, 0xce) : Color.FromArgb(0x15, 0x5d, 0x90),
            _ => _foreColour,
        };

        /// <summary>
        /// Row/title colour for a rarity.
        ///
        /// Keyed on IAGD's rarity names, which are not the game's classification names: IAGD calls an
        /// Epic-class record "Blue" and a LEGENDARY-class record "Epic". Without that last entry every
        /// legendary rendered in the default white, which is how this was spotted.
        /// </summary>
        private static Color? RarityColour(string quality) => quality.ToLowerInvariant() switch {
            "yellow" => Color.FromArgb(224, 200, 96),
            "green" => Color.FromArgb(112, 200, 112),
            "blue" => Color.FromArgb(112, 160, 232),
            "epic" or "purple" => Color.FromArgb(192, 128, 232),
            "orange" => Color.FromArgb(232, 152, 80),
            "red" => Color.FromArgb(232, 112, 112),
            "white" => Color.FromArgb(220, 220, 220),
            _ => null,
        };

        /// <summary>
        /// Icons by file name. IAGD extracts every item icon out of the game's .arc files into its storage
        /// folder once (~4000 PNGs), so this only ever reads from disk - and only once per distinct icon,
        /// because a stash is mostly repeats. Misses are cached as null so a missing file is not re-probed
        /// on every rebind.
        /// </summary>
        private static readonly Dictionary<string, Image?> IconCache = new();

        private static Image? LoadIcon(string? fileName) {
            if (string.IsNullOrEmpty(fileName)) {
                return null;
            }

            lock (IconCache) {
                if (IconCache.TryGetValue(fileName, out var cached)) {
                    return cached;
                }

                Image? image = null;
                try {
                    var path = Path.Combine(Utilities.GlobalPaths.StorageFolder, fileName);
                    if (File.Exists(path)) {
                        // Decode from a byte[] rather than Image.FromFile, which keeps the file handle open
                        // for the lifetime of the Image and would lock IAGD's own storage folder.
                        using var stream = new MemoryStream(File.ReadAllBytes(path));
                        image = Image.FromStream(stream);
                    }
                }
                catch (Exception ex) {
                    Logger.Warn($"Could not load item icon '{fileName}': {ex.Message}");
                }

                IconCache[fileName] = image;
                return image;
            }
        }

        /// <summary>One displayed row. Public properties are the grid's columns.</summary>
        private class ItemRow {
            [Browsable(false)]
            public JsonItem Item { get; }

            public Image? Icon { get; }
            public string Name { get; }
            public string Quality { get; }
            public int Level { get; }
            public string Slot { get; }
            public int Count { get; }

            public ItemRow(JsonItem item, int count) {
                Item = item;
                Count = count;
                Icon = LoadIcon(item.Icon);
                Name = (item.Name ?? string.Empty) + (string.IsNullOrEmpty(item.Socket) ? string.Empty : " " + item.Socket);
                Quality = item.Quality ?? string.Empty;
                Level = (int)item.Level;
                Slot = PrettySlot(item.Slot);
            }

            /// <summary>Slots arrive as raw record names ("OneShot_Scroll", "ArmorTorso_Chest").</summary>
            private static string PrettySlot(string? slot) {
                if (string.IsNullOrEmpty(slot)) {
                    return string.Empty;
                }

                return slot.Replace('_', ' ');
            }

            /// <summary>
            /// The tooltip body, one line per entry, with Grim Dawn's ^-colour codes LEFT IN so the caller can
            /// turn them into colour runs. The name is emitted by the caller (in its rarity colour), and the
            /// replica already repeats it, so the first replica line is dropped when it does.
            /// </summary>
            public IEnumerable<string> DescribeLines(IReadOnlyDictionary<double, (double Min, double Max)>? ranges = null) {
                var lines = new List<string>();

                // Items captured by the hook carry a stat "replica" (the exact in-game tooltip lines, name
                // included). When one is present ItemHtmlWriter deliberately empties HeaderStats/BodyStats and
                // expects the view to render ReplicaStats instead - skipping this fallback leaves looted items
                // with no stats at all, which is every item that matters here.
                var hasReplica = Item.ReplicaStats is { Count: > 0 };

                if (hasReplica) {
                    var first = true;
                    foreach (var stat in Item.ReplicaStats!) {
                        if (string.IsNullOrWhiteSpace(stat.Text)) {
                            continue;
                        }

                        var line = stat.Text!.Trim();
                        if (first) {
                            first = false;

                            // The replica opens with the item's own name, which the caller has already printed
                            // in its rarity colour.
                            if (StripColourCodes(line).Trim() == Name) {
                                continue;
                            }
                        }

                        lines.Add(line);
                    }
                }

                AppendStats(lines, Item.HeaderStats, ranges);
                AppendStats(lines, Item.BodyStats, ranges);

                if (Item.PetStats is { Count: > 0 }) {
                    lines.Add(string.Empty);
                    lines.Add("Pet bonuses:");
                    AppendStats(lines, Item.PetStats, ranges);
                }

                return lines;
            }

            private static void AppendStats(List<string> lines, IList<JsonStat>? stats, IReadOnlyDictionary<double, (double Min, double Max)>? ranges) {
                if (stats == null) {
                    return;
                }

                foreach (var stat in stats) {
                    var line = FormatStat(stat, ranges);
                    if (!string.IsNullOrWhiteSpace(line)) {
                        lines.Add(line);
                    }
                }
            }

            /// <summary>
            /// Grim Dawn's tooltip text carries inline colour markup ("^E" and friends). The web UI renders it
            /// as styling; a TextBox would show the escapes verbatim, so drop them.
            /// </summary>
            private static string StripColourCodes(string text) {
                return System.Text.RegularExpressions.Regex.Replace(text, @"\^[a-zA-Z]", string.Empty);
            }

            /// <summary>Same {0}..{6} substitution the web UI does in ItemStat.tsx.</summary>
            private static string FormatStat(JsonStat stat, IReadOnlyDictionary<double, (double Min, double Max)>? ranges) {
                if (string.IsNullOrEmpty(stat.Text)) {
                    return string.Empty;
                }

                // A rollable stat shows its band ("21/31 Physique") rather than one value. The line does not
                // carry the field it came from, so it is matched on its value; values that were ambiguous
                // within the item are absent from the map, so a line can never get the wrong range.
                if (ranges != null && stat.Param0 is { } value && ranges.TryGetValue(value, out var range)) {
                    return stat.Text!
                        .Replace("{0}", $"{range.Min:0.##}/{range.Max:0.##}")
                        .Replace("{1}", stat.Param1?.ToString() ?? string.Empty)
                        .Replace("{2}", stat.Param2?.ToString() ?? string.Empty)
                        .Replace("{3}", stat.Param3 ?? string.Empty)
                        .Replace("{4}", stat.Param4?.ToString() ?? string.Empty)
                        .Replace("{5}", stat.Param5 ?? string.Empty)
                        .Replace("{6}", stat.Param6 ?? string.Empty)
                        .Trim();
                }

                return stat.Text!
                    .Replace("{0}", stat.Param0?.ToString() ?? string.Empty)
                    .Replace("{1}", stat.Param1?.ToString() ?? string.Empty)
                    .Replace("{2}", stat.Param2?.ToString() ?? string.Empty)
                    .Replace("{3}", stat.Param3 ?? string.Empty)
                    .Replace("{4}", stat.Param4?.ToString() ?? string.Empty)
                    .Replace("{5}", stat.Param5 ?? string.Empty)
                    .Replace("{6}", stat.Param6 ?? string.Empty)
                    .Trim();
            }
        }
    }
}
