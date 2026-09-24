using IAGrim.Database;
using IAGrim.Database.Dto;
using IAGrim.Database.Interfaces;
using IAGrim.Parsers.Arz;
using IAGrim.Services.ItemStats;
using IAGrim.Settings;
using IAGrim.Theme;
using IAGrim.UI.Controller;
using IAGrim.UI.Misc.CEF;
using IAGrim.UI.Tabs.Util;
using IAGrim.Utilities;
using log4net;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

namespace IAGrim.UI.Tabs {
    internal sealed class SplitSearchWindow : Form {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(SplitSearchWindow));
        private readonly Action<string> _setStatus;
        private readonly SearchController _searchController;
        private readonly IItemTagDao _itemTagDao;
        private System.Windows.Forms.Timer? _delayedTextChangedTimer;
        private DesiredSkills? _filterWindow;
        private TextBox _searchBox;
        private CheckBox? _orderByLevel;
        private ComboBox? _modFilter;
        private ComboBox? _slotFilter;
        private SplitContainer _mainSplitter;
        private ComboBox? _itemQuality;
        private ComboBoxItem? _selectedSlot;
        private TextBox? _minLevel;
        private TextBox? _maxLevel;
        private FlowLayoutPanel? _flowPanelFilter;
        private GroupBox? _levelRequirementGroup;
        private ComboBoxItemQuality? _selectedItemQuality;
        private ScrollPanelMessageFilter? _scrollableFilterView;
        private ToolStripContainer _toolStripContainer;
        private readonly SettingsService _settings;
        private ToolTip? toolTip1;
        private System.ComponentModel.IContainer? components;
        private readonly int FilterPanelMinSize;
        private Microsoft.Web.WebView2.WinForms.WebView2 webView21;
        private bool _hasCheckedModFilterNotEmpty = false;
        private bool _isAdjustingSplitter;

        /// <summary>
        /// ModSelectionHandler
        /// </summary>
        public ModSelectionHandler ModSelectionHandler { get; }
        public WebView2 Browser => this.webView21;

        /// <summary>
        /// Native item grid, used instead of the WebView2 one when the browser cannot present (Wine).
        /// Null when the web UI is in charge. See NativeItemGrid for the diagnosis behind this.
        /// </summary>
        private NativeItemGrid? _nativeGrid;
        private bool _toolbarNormalizedAfterScale;

        /// <summary>Toggles the grid between the player's stash and the whole game item database.</summary>
        private CheckBox? _databaseMode;
        private Database.DAO.DatabaseBrowse? _databaseBrowse;
        private System.Windows.Forms.Timer? _databaseDebounce;
        private bool _searchBoxWidthPinned;
        private int _searchBoxWidth;

        /// <summary>
        /// Constructor
        /// </summary>
        /// <param name="browser"></param>
        /// <param name="setStatus"></param>
        /// <param name="playerItemDao"></param>
        /// <param name="searchController"></param>
        /// <param name="itemTagDao"></param>
        public SplitSearchWindow(Microsoft.Web.WebView2.WinForms.WebView2 browser,
            Action<string> setStatus,
            IPlayerItemDao playerItemDao,
            SearchController searchController,
            IItemTagDao itemTagDao, SettingsService settings,
            CefBrowserHandler? browserHandler = null) {
            _setStatus = setStatus;
            _searchController = searchController;
            _itemTagDao = itemTagDao;
            _settings = settings;
            InitializeComponent();

            Dock = DockStyle.Fill;

            FilterPanelMinSize = new DpiHelper(CreateGraphics()).ScaleX(250);

            // Weird hack to not have the searcbox be height=4 on windows 11. 
            _searchBox!.MaximumSize = new Size(_searchBox.MaximumSize.Width, 0);

            _mainSplitter!.SplitterDistance = FilterPanelMinSize;
            _mainSplitter.SplitterWidth = 5;
            _mainSplitter.BorderStyle = BorderStyle.None;
            _mainSplitter.SplitterMoved += MainSplitterOnSplitterMoved;
            _mainSplitter.Resize += MainSplitterOnResize;

            ModSelectionHandler = new ModSelectionHandler(_modFilter!, playerItemDao, UpdateListViewDelayed, setStatus, _settings);

            _toolStripContainer!.ContentPanel.Controls.Add(browser);

            Activated += SplitSearchWindow_Activated;
            Deactivate += SplitSearchWindow_Deactivate;

            // Painted by the webview until the page itself paints; must match the WebUI --background-color
            // of the matching theme (WebUI/src/style/index.css) or the user gets a flash on startup.
            // The WebUI leaves <body> transparent so this is the only colour on screen until it renders.
            webView21!.DefaultBackgroundColor = _settings.GetPersistent().DarkMode
                ? Color.Black
                : Color.FromArgb(65, 60, 53); // #413c35

            var conf = CreateWebViewEnvironment();
            if (conf != null) {
                webView21!.EnsureCoreWebView2Async(conf);
            }

            InitializeFilterPanel();
            MaybeUseNativeItemGrid(browserHandler);
        }

        /// <summary>
        /// Adds the "Game database" toggle to the filter toolbar.
        ///
        /// IAGD parses every item Grim Dawn defines in order to resolve names and stats, but the UI only ever
        /// queries the player's own items, so those ~9,600 records are invisible. Ticking this searches them
        /// instead - the same grid, the same search box, the same set and drop-source sections.
        /// </summary>
        private void AddDatabaseModeToggle() {
            if (_nativeGrid == null || _flowPanelFilter == null) {
                return;
            }

            _databaseBrowse = new Database.DAO.DatabaseBrowse(new Database.SessionFactory());

            _databaseMode = new CheckBox {
                Text = "Game database",
                AutoSize = true,
                Tag = "iatag_ui_databasemode",
            };

            _databaseMode.CheckedChanged += (_, _) => {
                if (_databaseMode.Checked) {
                    RefreshDatabaseListing();
                }
                else {
                    _nativeGrid.LeaveDatabaseMode();
                    UpdateListViewDelayed();
                }
            };

            _flowPanelFilter.Controls.Add(_databaseMode);
            NormalizeFilterToolbar();
        }

        /// <summary>
        /// Runs the database search for whatever is in the search box. Off the UI thread: a LIKE over 9,600
        /// names with per-row stat lookups is fast but not instant, and this runs on every keystroke.
        /// </summary>
        private void RefreshDatabaseListing() {
            if (_nativeGrid == null || _databaseBrowse == null) {
                return;
            }

            // Coalesce keystrokes: the widened search touches skills and flavour text, so firing one per
            // character would leave several hundred-millisecond queries racing each other.
            _databaseDebounce?.Stop();
            _databaseDebounce ??= new System.Windows.Forms.Timer { Interval = 250 };
            _databaseDebounce.Tick -= DatabaseDebounceTick;
            _databaseDebounce.Tick += DatabaseDebounceTick;
            _databaseDebounce.Start();
        }

        private void DatabaseDebounceTick(object? sender, EventArgs e) {
            _databaseDebounce?.Stop();
            RunDatabaseListing();
        }

        private void RunDatabaseListing() {
            if (_nativeGrid == null || _databaseBrowse == null) {
                return;
            }

            var fragment = _searchBox.Text;
            var grid = _nativeGrid;
            var browse = _databaseBrowse;

            // The toolbar filters apply here too, otherwise ticking "Game database" silently drops them.
            var rarity = _selectedItemQuality?.Rarity;
            var slots = _selectedSlot?.Filter;
            var slotInverse = _selectedSlot?.Inverse ?? false;
            var statFilters = _filterWindow?.Filters?.Filters;
            double.TryParse(_minLevel?.Text, out var minLevel);
            double.TryParse(_maxLevel?.Text, out var maxLevel);

            var thread = new Thread(() => {
                var rows = browse.Search(fragment, rarity, minLevel, maxLevel, slots, slotInverse, statFilters);

                // Run the database rows through the SAME stat resolution the stash search uses: a PlayerItem
                // carrying only a base record is enough, because ApplyStatsToPlayerItems resolves everything
                // (stats, bitmap, rarity, slot) from the records rather than from anything item-instance
                // specific. The result is a full tooltip for an item the player has never held.
                // Rarity and LevelRequirement are plain columns on PlayerItem, not derived from the stat
                // rows, so they have to be carried across explicitly - otherwise both columns render blank
                // and the tiers of a same-named item look like duplicates.
                var items = rows.Select(r => new PlayerItem {
                    BaseRecord = r.BaseRecord,
                    Name = r.Name,
                    Mod = string.Empty,
                    Rarity = Database.DAO.DatabaseBrowse.ColourForClassification(r.Rarity),
                    LevelRequirement = r.Level,
                }).ToList();

                try {
                    _searchController.ItemStats.ApplyStatsToPlayerItems(items);
                }
                catch (Exception ex) {
                    Logger.Warn($"Could not resolve stats for the database listing: {ex.Message}");
                }

                var json = Utilities.ItemHtmlWriter.ToJsonSerializable(
                    items.Select(i => new List<Database.Interfaces.PlayerHeldItem> { i }).ToList());

                grid.ShowDatabaseItems(json);
                grid.BeginInvoke(new Action(() => _setStatus($"{rows.Count} items in the game database")));
            }) {
                IsBackground = true,
                Name = "DatabaseBrowse",
            };

            thread.Start();
        }

        /// <summary>
        /// Builds the "Obtained from" index off the UI thread.
        ///
        /// Reading database.arz costs a couple of seconds and a few hundred MB of transient allocation, so it
        /// must not happen on the UI thread and must not delay the window. The grid checks IsLoaded, so item
        /// details simply omit the section until it is ready rather than blocking on it.
        /// </summary>
        private void LoadLootGraphInBackground() {
            var gameDir = _settings.GetLocal().CurrentGrimdawnLocation;
            if (string.IsNullOrEmpty(gameDir) || _nativeGrid == null) {
                return;
            }

            var grid = _nativeGrid;
            var thread = new Thread(() => {
                try {
                    var graph = new Parsers.Arz.LootGraph();
                    graph.Load(Path.Combine(gameDir, "database", "database.arz"));

                    var tags = new DatabaseItemDaoImpl(new Database.SessionFactory()).GetTagDictionary();
                    grid.BeginInvoke(new Action(() => {
                        grid.Tags = tags;
                        grid.LootGraph = graph;
                    }));
                }
                catch (Exception ex) {
                    Logger.Warn($"Could not build the loot graph: {ex.Message}");
                }
            }) {
                IsBackground = true,
                Name = "LootGraph",
            };

            thread.Start();
        }

        /// <summary>
        /// Lays the search/filter toolbar out as one left-aligned row directly above the item list.
        ///
        /// The designer gives these controls Anchor values (_searchBox is Left|Right, _orderByLevel is Right)
        /// which only make sense in an anchored container. Inside a FlowLayoutPanel an anchor stretches the
        /// control's flow cell instead, so on a wide window the search box's cell eats the row, the checkbox
        /// is flung to the far right, and the remaining filters wrap onto a second line. Their top margins are
        /// likewise hand-tuned (20, 22, ...) to centre against the stock 8.25pt row height, and go crooked as
        /// soon as the fonts grow.
        ///
        /// So: no wrapping, no anchors, and top margins recomputed to centre each control against the tallest
        /// one (the Level group box). Called again after the fonts change, since every height moves with them.
        /// </summary>
        private void NormalizeFilterToolbar() {
            var panel = _flowPanelFilter;
            if (panel == null || panel.Controls.Count == 0) {
                return;
            }

            panel.WrapContents = false;
            panel.FlowDirection = FlowDirection.LeftToRight;
            panel.AutoSize = true;
            panel.AutoSizeMode = AutoSizeMode.GrowAndShrink;

            // Clearing the anchor does not undo the width the control already took from it: the search box has
            // been stretched to the full row, which pushes everything after it off the right-hand edge. Give it
            // a definite width instead. MaximumSize is cleared first - the constructor pins it to (512, 0) and
            // a max height of 0 confuses the flow layout once the font grows.
            if (_searchBox != null) {
                _searchBox.MaximumSize = Size.Empty;
                _searchBox.MinimumSize = Size.Empty;
                _searchBoxWidth = (int)(320 * Program.UiScale);
                _searchBox.Width = _searchBoxWidth;

                // Something re-stretches it on the first real resize (the window opens small and is then
                // maximised), so hold the width rather than setting it once and hoping.
                if (!_searchBoxWidthPinned) {
                    _searchBoxWidthPinned = true;
                    _searchBox.SizeChanged += (_, _) => {
                        if (_searchBox.Width != _searchBoxWidth) {
                            _searchBox.Width = _searchBoxWidth;
                        }
                    };
                }
            }

            var tallest = 0;
            foreach (Control control in panel.Controls) {
                control.Anchor = AnchorStyles.Top | AnchorStyles.Left;
                tallest = Math.Max(tallest, control.Height);
            }

            foreach (Control control in panel.Controls) {
                var top = Math.Max(3, (tallest - control.Height) / 2);
                control.Margin = new Padding(6, top, 6, 3);
            }

            panel.PerformLayout();
        }

        /// <summary>
        /// Puts the native WinForms grid in front of the WebView2 control and feeds it the same item stream.
        ///
        /// Enabled automatically under Wine, where WebView2 loads and runs the page but never presents its
        /// child window, leaving the item grid permanently blank (see NativeItemGrid). Set IAGD_NATIVE_GRID=0
        /// to force the web UI back on, or =1 to use the native grid on Windows.
        ///
        /// The browser is left constructed and navigating even when hidden: it owns the host object, the
        /// readiness handshake and the collection/help tabs, none of which are reimplemented here.
        /// </summary>
        private void MaybeUseNativeItemGrid(CefBrowserHandler? browserHandler) {
            var useNative = Environment.GetEnvironmentVariable("IAGD_NATIVE_GRID") switch {
                "0" => false,
                "1" => true,
                _ => Services.WineDetector.IsRunningInWine(),
            };

            if (!useNative || browserHandler == null) {
                return;
            }

            _nativeGrid = new NativeItemGrid(_settings.GetPersistent().DarkMode) {
                OnTransfer = (item, transferAll) => {
                    if (item.URL == null) {
                        return;
                    }

                    // Exactly what the web UI calls; the reply is only used for its toast, which we skip.
                    _searchController.JsIntegration.TransferItem(item.URL, transferAll);
                    UpdateListViewDelayed();
                }
            };

            // Set membership comes straight out of the parsed game database; the factory is a shared, lazily
            // built singleton behind this wrapper, so constructing one here costs nothing.
            _nativeGrid.SetLookup = new Database.DAO.SetLookup(new Database.SessionFactory());
            _nativeGrid.SortByLevelSecondary = _orderByLevel?.Checked ?? false;
            _nativeGrid.RangeProvider = record => _searchController.ItemStats.ComputeRollRanges(record);

            LoadLootGraphInBackground();
            AddDatabaseModeToggle();

            _toolStripContainer!.ContentPanel.Controls.Add(_nativeGrid);
            _nativeGrid.BringToFront();
            NormalizeFilterToolbar();
            browserHandler.NativeItemSink = (items, replace, numFound) => _nativeGrid.SetItems(items, replace, numFound);

            Logger.Info("Native item grid enabled (WebView2 cannot present under Wine)");
        }

        /// <summary>
        /// Builds the WebView2 environment, or tells the user why the item grid is going to be empty.
        ///
        /// A missing runtime used to reach here as an exception out of the constructor, taking IAGD down during
        /// startup -- before the main window existed, and so before any of the WebView2 error handling could run.
        /// </summary>
        private CoreWebView2Environment? CreateWebViewEnvironment() {
            var conf = WebView2Runtime.TryCreateEnvironment(out var error);
            if (conf != null) {
                return conf;
            }

            Logger.Error("The WebView2 environment could not be created; the item grid cannot be displayed.");
            MessageBox.Show(error, "Error - WebView2", MessageBoxButtons.OK, MessageBoxIcon.Error);
            return null;
        }



        public void SelectModFilterIfNotSelected() {
            if (!_hasCheckedModFilterNotEmpty) {
                if (InvokeRequired) {
                    Invoke((MethodInvoker)delegate {
                        this.SelectModFilterIfNotSelected();
                    });
                }
                else {
                    // Should only happen for the very first item ever looted, but it's a fairly poor user experience when it does happen.
                    if (_modFilter!.SelectedItem == null) {
                        ModSelectionHandler.ConfigureModFilter();
                    }

                    if (_modFilter.SelectedItem == null && _modFilter.Items.Count > 0) {
                        _modFilter.SelectedItem = _modFilter!.Items[0];
                        _hasCheckedModFilterNotEmpty = true;
                    }
                }

            }
        }

        /// <summary>
        /// Clear all filters
        /// </summary>
        public void ClearFilters() {
            _filterWindow!.ClearFilters();
            _searchBox.Text = string.Empty;
            _itemQuality!.SelectedIndex = 0;
            _slotFilter!.SelectedIndex = 0;
            _minLevel!.Text = "0";
            _maxLevel!.Text = "110";

            UpdateListViewDelayed();
        }

        /// <summary>
        /// Update interface
        /// </summary>
        public void UpdateInterface() {
            InitializeFilterPanel();
        }

        /// <summary>
        /// Update view
        /// </summary>
        public void UpdateListView(PlayerItem? item = null) {
            if (InvokeRequired) {
                Invoke((MethodInvoker)delegate { UpdateListView(_filterWindow!.Filters, item); });
            }
            else {
                UpdateListView(_filterWindow!.Filters, item);
            }
        }

        /// <summary>
        /// Update view
        /// </summary>
        public void UpdateListView(FilterEventArgs filters, PlayerItem? item = null) {
            // UiScaler runs on the main window's Shown, after this form is built, and every control in the
            // toolbar changes height when its font does. Re-centre them once we are past that point.
            if (_nativeGrid != null && !_toolbarNormalizedAfterScale) {
                _toolbarNormalizedAfterScale = true;
                NormalizeFilterToolbar();
            }

            var transferFile = ModSelectionHandler.SelectedMod;

            if (transferFile == null) {
                Logger.Warn("Attempting to update item view, but no mod selection has been made");
                ModSelectionHandler.SetDefaultModIfAvailable();
                return;
            }

            var rarity = _selectedItemQuality;
            var slot = _selectedSlot;

            var query = new ItemSearchRequest {
                Wildcard = _searchBox.Text,
                StatValueFilters = filters.NumericFilters ?? new List<StatValueFilter>(),
                Filters = filters.Filters ?? new List<string[]>(),
                MinimumLevel = ParseNumeric(_minLevel!),
                MaximumLevel = ParseNumeric(_maxLevel!),
                Rarity = rarity?.Rarity,
                PrefixRarity = rarity?.PrefixRarity ?? 0,
                Slot = slot?.Filter,
                SlotInverse = slot?.Inverse ?? false,
                PetBonuses = filters.PetBonuses,
                HasPetBonus = filters.HasPetBonus,
                IsRetaliation = filters.IsRetaliation,
                DuplicatesOnly = filters.DuplicatesOnly,
                Mod = transferFile.Mod,
                IsHardcore = transferFile.IsHardcore,
                Classes = filters.DesiredClass ?? new List<string>(),
                SocketedOnly = filters.SocketedOnly,
                RecentOnly = filters.RecentOnly,
                WithGrantSkillsOnly = filters.GrantsSkill,
                WithSummonerSkillOnly = filters.WithSummonerSkillOnly
            };

            if (item != null) {
                _searchController.Search(query, item);
            }
            else {
                bool includeBuddyItems = !filters.DuplicatesOnly; // If we're looking for duplicates, we're probably doing a cleanup, not caring about buddyitems
                var message = _searchController.Search(query, includeBuddyItems, _orderByLevel!.Checked);

                Logger.Info("Updating UI...");

                if (!string.IsNullOrEmpty(message)) {
                    _setStatus(message);
                }

                Logger.Info("Done");
            }
        }

        /// <summary>
        /// Update view with delay
        /// </summary>
        public void UpdateListViewDelayed() {
            UpdateListViewDelayed(_settings.GetLocal().PreferDelayedSearch ? 200 : 0);
        }

        private void BeginSearchOnAutoSearch(object? sender, EventArgs e) {
            // Once the user finds the numeric stat filter on their own, the introduction banner is no longer relevant.
            var persistent = _settings.GetPersistent();
            if (!persistent.NumericFilterUsed && e is FilterEventArgs { NumericFilters.Count: > 0 }) {
                persistent.NumericFilterUsed = true;
            }

            UpdateListViewDelayed();
        }

        private void HandleDelayedTextChangedTimerTick(object? sender, EventArgs e) {
            if (_delayedTextChangedTimer != null) {
                _delayedTextChangedTimer.Stop();
                _delayedTextChangedTimer = null;
            }

            if (InvokeRequired) {
                Invoke((MethodInvoker) delegate { UpdateListView(_filterWindow!.Filters); });
            }
            else {
                UpdateListView(_filterWindow!.Filters);
            }
        }

        private void InitializeFilterPanel() {
            if (_filterWindow != null) {
                _filterWindow.OnChanged -= BeginSearchOnAutoSearch;
                _filterWindow.Close();
                _mainSplitter.Panel1.Controls.Remove(_filterWindow);
            }

            _filterWindow = new DesiredSkills(_itemTagDao) {
                TopLevel = false
            };
            _filterWindow.OnChanged += BeginSearchOnAutoSearch;

            // In database mode the panel drives the database query; BeginSearchOnAutoSearch only re-runs the
            // stash search, so the browser would ignore every checkbox without this.
            _filterWindow.OnChanged += (_, _) => {
                if (_databaseMode is { Checked: true }) {
                    RefreshDatabaseListing();
                }
            };
            _mainSplitter.Panel1.Controls.Add(_filterWindow);
            _filterWindow.Show();

            // This panel is rebuilt on every mod-selection change, i.e. after the one-shot pass at startup,
            // so it needs scaling here or it stays at the stock font size while the rest of the UI grows.
            Misc.UiScaler.Apply(_filterWindow, Program.UiScale);
        }

        /// <summary>
        /// Keeps the filter panel from being dragged below its minimum width.
        /// Never set SplitterDistance to a value the SplitContainer cannot honour: the setter silently clamps
        /// to (Width - Panel2MinSize - SplitterWidth) and then raises SplitterMoved again, so an unreachable
        /// value makes this handler re-enter itself endlessly and hangs the UI thread (blank window /
        /// "not responding") whenever the control is laid out narrower than FilterPanelMinSize.
        /// </summary>
        private void MainSplitterOnSplitterMoved(object? sender, SplitterEventArgs e) => EnforceFilterPanelMinSize();

        private void MainSplitterOnResize(object? sender, EventArgs e) => EnforceFilterPanelMinSize();

        private void EnforceFilterPanelMinSize() {
            if (_isAdjustingSplitter || _mainSplitter.SplitterDistance >= FilterPanelMinSize) {
                return;
            }

            var maxDistance = _mainSplitter.Width - _mainSplitter.Panel2MinSize - _mainSplitter.SplitterWidth;
            var desired = Math.Min(FilterPanelMinSize, maxDistance);
            if (desired < _mainSplitter.Panel1MinSize || desired == _mainSplitter.SplitterDistance) {
                return;
            }

            _isAdjustingSplitter = true;
            try {
                _mainSplitter.SplitterDistance = desired;
            }
            finally {
                _isAdjustingSplitter = false;
            }
        }

        private void MaxLevel_MouseWheel(object? sender, MouseEventArgs e) {
            if (sender is Control c) {
                if (e.Delta > 0) {
                    c.Text = Math.Min(ParseNumeric(c) + 1, 110).ToString();
                }
                else if (e.Delta < 0) {
                    var newValue = Math.Min(Math.Max(0, ParseNumeric(c) - 1), 110);
                    if (ParseNumeric(_minLevel!) > newValue) {
                        _minLevel!.Text = newValue.ToString();
                    }

                    c.Text = Math.Min(Math.Max(0, ParseNumeric(c) - 1), 110).ToString();
                }
            }

            UpdateListViewDelayed(1200);
        }

        private void MinLevel_MouseWheel(object? sender, MouseEventArgs e) {
            if (sender is Control c) {
                if (e.Delta > 0) {
                    var newValue = Math.Min(ParseNumeric(c) + 1, 110);
                    if (ParseNumeric(_maxLevel!) < newValue) {
                        _maxLevel!.Text = newValue.ToString();
                    }

                    c.Text = newValue.ToString();
                }
                else if (e.Delta < 0) {
                    c.Text = Math.Min(Math.Max(0, ParseNumeric(c) - 1), 110).ToString();
                }
            }

            UpdateListViewDelayed(1200);
        }

        private void MinLevel_KeyPress(object? sender, KeyPressEventArgs e) {
            e.Handled = !(char.IsDigit(e.KeyChar) || ParseNumeric(_minLevel!) > 105 || e.KeyChar == '\b');
            UpdateListViewDelayed(1200);
        }

        private int ParseNumeric(Control tb) {
            return int.TryParse(tb.Text, out var val) ? val : 0;
        }

        private void SplitSearchWindow_Load(object? sender, EventArgs e) {
            ModSelectionHandler.ConfigureModFilter();

            _minLevel!.KeyPress += MinLevel_KeyPress;
            _minLevel.Leave += (s, ev) => { if (!int.TryParse(_minLevel.Text, out _)) _minLevel.Text = "0"; };
            _minLevel.MouseWheel += MinLevel_MouseWheel;

            _maxLevel!.KeyPress += MinLevel_KeyPress;
            _maxLevel.Leave += (s, ev) => { if (!int.TryParse(_maxLevel.Text, out _)) _maxLevel.Text = "200"; };
            _maxLevel.MouseWheel += MaxLevel_MouseWheel;

            _itemQuality!.Items.AddRange(UIHelper.QualityFilter.ToArray<object>());
            _itemQuality.SelectedIndex = 0;
            _selectedItemQuality = _itemQuality.SelectedItem as ComboBoxItemQuality;
            _itemQuality.SelectedIndexChanged += (s, ev) => { _selectedItemQuality = _itemQuality.SelectedItem as ComboBoxItemQuality; };
            _itemQuality.SelectedIndexChanged += BeginSearchOnAutoSearch;

            _slotFilter!.Items.AddRange(UIHelper.SlotFilter.ToArray<object>());
            _slotFilter.SelectedIndex = 0;
            _selectedSlot = _slotFilter.SelectedItem as ComboBoxItem;
            _slotFilter.SelectedIndexChanged += (s, ev) => { _selectedSlot = _slotFilter.SelectedItem as ComboBoxItem; };
            _slotFilter.SelectedIndexChanged += BeginSearchOnAutoSearch;

            FormClosing += SplitSearchWindow_FormClosing;

            _searchBox.TextChanged += SearchBox_TextChanged;

            _orderByLevel!.CheckStateChanged += delegate { UpdateListViewDelayed(); };

            // The same checkbox also drives the native grid's secondary sort. In database mode no search is
            // re-run on toggle, so the grid is re-sorted directly.
            _orderByLevel.CheckStateChanged += delegate {
                if (_nativeGrid != null) {
                    _nativeGrid.SortByLevelSecondary = _orderByLevel.Checked;
                    _nativeGrid.RefreshSort();
                }
            };

            // In database mode these drive the database query instead of the stash search.
            _itemQuality!.SelectedIndexChanged += (_, _) => { if (_databaseMode is { Checked: true }) RefreshDatabaseListing(); };
            _slotFilter!.SelectedIndexChanged += (_, _) => { if (_databaseMode is { Checked: true }) RefreshDatabaseListing(); };
            _minLevel!.TextChanged += (_, _) => { if (_databaseMode is { Checked: true }) RefreshDatabaseListing(); };
            _maxLevel!.TextChanged += (_, _) => { if (_databaseMode is { Checked: true }) RefreshDatabaseListing(); };

            _flowPanelFilter!.SizeChanged += FlowPanelFilter_Resize;
            _mainSplitter.SizeChanged += FlowPanelFilter_Resize;

            LocalizationLoader.ApplyTooltipLanguage(toolTip1!, Controls, RuntimeSettings.Language!);
        }

        private void SearchBox_TextChanged(object? sender, EventArgs e) {
            // In database mode the search box drives the game-database query instead of the stash search.
            if (_databaseMode is { Checked: true }) {
                RefreshDatabaseListing();
                return;
            }

            UpdateListViewDelayed(600);
        }

        private void SplitSearchWindow_Activated(object? sender, EventArgs e) {
            _scrollableFilterView = new ScrollPanelMessageFilter(_filterWindow!);
            Application.AddMessageFilter(_scrollableFilterView);
        }

        private void SplitSearchWindow_Deactivate(object? sender, EventArgs e) {
            if (_scrollableFilterView != null) {
                Application.RemoveMessageFilter(_scrollableFilterView);
            }
        }

        private void SplitSearchWindow_FormClosing(object? sender, FormClosingEventArgs e) {
            if (InvokeRequired) {
                Invoke((System.Windows.Forms.MethodInvoker)delegate { SplitSearchWindow_FormClosing(sender, e); });
            }
            else {
                _filterWindow!.OnChanged -= BeginSearchOnAutoSearch;
                Activated -= SplitSearchWindow_Activated;
                Deactivate -= SplitSearchWindow_Deactivate;

                _toolStripContainer.ContentPanel.Controls.Clear();
            }
        }

        private void UpdateListViewDelayed(int delay) {
            _delayedTextChangedTimer?.Stop();

            if (delay > 0) {
                _delayedTextChangedTimer = new System.Windows.Forms.Timer();
                _delayedTextChangedTimer.Tick += HandleDelayedTextChangedTimerTick;
                _delayedTextChangedTimer.Interval = delay;
                _delayedTextChangedTimer.Start();
            }
            else {
                HandleDelayedTextChangedTimerTick(this, EventArgs.Empty);
            }
        }

        private void InitializeComponent() {
            components = new System.ComponentModel.Container();
            _mainSplitter = new SplitContainer();
            _toolStripContainer = new ToolStripContainer();
            webView21 = new WebView2();
            _flowPanelFilter = new FlowLayoutPanel();
            _searchBox = new TextBox();
            _orderByLevel = new CheckBox();
            _itemQuality = new ComboBox();
            _slotFilter = new ComboBox();
            _modFilter = new ComboBox();
            _levelRequirementGroup = new GroupBox();
            _minLevel = new TextBox();
            _maxLevel = new TextBox();
            toolTip1 = new ToolTip(components);
            ((System.ComponentModel.ISupportInitialize)_mainSplitter).BeginInit();
            _mainSplitter.Panel2.SuspendLayout();
            _mainSplitter.SuspendLayout();
            _toolStripContainer.ContentPanel.SuspendLayout();
            _toolStripContainer.SuspendLayout();
            ((System.ComponentModel.ISupportInitialize)webView21).BeginInit();
            _flowPanelFilter.SuspendLayout();
            _levelRequirementGroup.SuspendLayout();
            SuspendLayout();
            // 
            // _mainSplitter
            // 
            _mainSplitter.Dock = DockStyle.Fill;
            _mainSplitter.Location = new Point(0, 0);
            _mainSplitter.Margin = new Padding(4, 3, 4, 3);
            _mainSplitter.Name = "_mainSplitter";
            // 
            // _mainSplitter.Panel2
            // 
            _mainSplitter.Panel2.Controls.Add(_toolStripContainer);
            _mainSplitter.Panel2.Controls.Add(_flowPanelFilter);
            _mainSplitter.Size = new Size(1532, 750);
            _mainSplitter.SplitterDistance = 237;
            _mainSplitter.TabIndex = 0;
            _mainSplitter.TabStop = false;
            // 
            // _toolStripContainer
            // 
            _toolStripContainer.BottomToolStripPanelVisible = false;
            // 
            // _toolStripContainer.ContentPanel
            // 
            _toolStripContainer.ContentPanel.Controls.Add(webView21);
            _toolStripContainer.ContentPanel.Margin = new Padding(4, 3, 4, 3);
            _toolStripContainer.ContentPanel.Size = new Size(1291, 669);
            _toolStripContainer.Dock = DockStyle.Fill;
            _toolStripContainer.LeftToolStripPanelVisible = false;
            _toolStripContainer.Location = new Point(0, 56);
            _toolStripContainer.Margin = new Padding(4, 3, 4, 3);
            _toolStripContainer.Name = "_toolStripContainer";
            _toolStripContainer.RightToolStripPanelVisible = false;
            _toolStripContainer.Size = new Size(1291, 694);
            _toolStripContainer.TabIndex = 48;
            _toolStripContainer.Text = "toolStripContainer1";
            // 
            // webView21
            // 
            webView21.AllowExternalDrop = true;
            webView21.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            webView21.CreationProperties = null;
            webView21.Location = new Point(4, 3);
            webView21.Margin = new Padding(4, 3, 4, 3);
            webView21.Name = "webView21";
            webView21.Size = new Size(1283, 663);
            webView21.TabIndex = 0;
            webView21.ZoomFactor = 1D;
            // 
            // _flowPanelFilter
            // 
            _flowPanelFilter.AutoSize = true;
            _flowPanelFilter.AutoSizeMode = AutoSizeMode.GrowAndShrink;
            _flowPanelFilter.Controls.Add(_searchBox);
            _flowPanelFilter.Controls.Add(_orderByLevel);
            _flowPanelFilter.Controls.Add(_itemQuality);
            _flowPanelFilter.Controls.Add(_slotFilter);
            _flowPanelFilter.Controls.Add(_modFilter);
            _flowPanelFilter.Controls.Add(_levelRequirementGroup);
            _flowPanelFilter.Dock = DockStyle.Top;
            _flowPanelFilter.Location = new Point(0, 0);
            _flowPanelFilter.Margin = new Padding(4, 3, 4, 3);
            _flowPanelFilter.Name = "_flowPanelFilter";
            _flowPanelFilter.Size = new Size(1291, 56);
            _flowPanelFilter.TabIndex = 52;
            // 
            // _searchBox
            // 
            _searchBox.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            _searchBox.AutoCompleteMode = AutoCompleteMode.Suggest;
            _searchBox.AutoCompleteSource = AutoCompleteSource.RecentlyUsedList;
            _searchBox.Location = new Point(4, 20);
            _searchBox.Margin = new Padding(4, 20, 4, 3);
            _searchBox.MaximumSize = new Size(512, 0);
            _searchBox.MaxLength = 255;
            _searchBox.Name = "_searchBox";
            _searchBox.Size = new Size(125, 23);
            _searchBox.TabIndex = 41;
            _searchBox.Tag = "iatag_ui_searchbox_tooltip";
            toolTip1.SetToolTip(_searchBox, "The item name, partially works fine.");
            // 
            // _orderByLevel
            // 
            _orderByLevel.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            _orderByLevel.AutoSize = true;
            _orderByLevel.Checked = true;
            _orderByLevel.CheckState = CheckState.Checked;
            _orderByLevel.Location = new Point(137, 22);
            _orderByLevel.Margin = new Padding(4, 22, 4, 3);
            _orderByLevel.Name = "_orderByLevel";
            _orderByLevel.Size = new Size(102, 19);
            _orderByLevel.TabIndex = 42;
            _orderByLevel.Tag = "iatag_ui_orderbylevel";
            _orderByLevel.Text = "Order By Level";
            toolTip1.SetToolTip(_orderByLevel, "If items should be ordered by level, instead of alphabetically.");
            _orderByLevel.UseVisualStyleBackColor = true;
            // 
            // _itemQuality
            // 
            _itemQuality.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            _itemQuality.DropDownStyle = ComboBoxStyle.DropDownList;
            _itemQuality.FormattingEnabled = true;
            _itemQuality.Location = new Point(247, 20);
            _itemQuality.Margin = new Padding(4, 20, 4, 3);
            _itemQuality.Name = "_itemQuality";
            _itemQuality.Size = new Size(104, 23);
            _itemQuality.TabIndex = 43;
            _itemQuality.Tag = "iatag_ui_itemquality_tooltip";
            toolTip1.SetToolTip(_itemQuality, "The minimum item quality");
            // 
            // _slotFilter
            // 
            _slotFilter.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            _slotFilter.DropDownStyle = ComboBoxStyle.DropDownList;
            _slotFilter.FormattingEnabled = true;
            _slotFilter.Location = new Point(359, 20);
            _slotFilter.Margin = new Padding(4, 20, 4, 3);
            _slotFilter.Name = "_slotFilter";
            _slotFilter.Size = new Size(139, 23);
            _slotFilter.TabIndex = 44;
            _slotFilter.Tag = "iatag_ui_slotfilter_tooltip";
            toolTip1.SetToolTip(_slotFilter, "Slot/Type");
            // 
            // _modFilter
            // 
            _modFilter.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            _modFilter.DropDownStyle = ComboBoxStyle.DropDownList;
            _modFilter.FormattingEnabled = true;
            _modFilter.Location = new Point(506, 20);
            _modFilter.Margin = new Padding(4, 20, 4, 3);
            _modFilter.Name = "_modFilter";
            _modFilter.Size = new Size(118, 23);
            _modFilter.TabIndex = 45;
            _modFilter.Tag = "iatag_ui_modfilter_tooltip";
            toolTip1.SetToolTip(_modFilter, "Mod / Hardcore / Vanilla");
            // 
            // _levelRequirementGroup
            // 
            _levelRequirementGroup.Controls.Add(_minLevel);
            _levelRequirementGroup.Controls.Add(_maxLevel);
            _levelRequirementGroup.Location = new Point(632, 3);
            _levelRequirementGroup.Margin = new Padding(4, 3, 4, 3);
            _levelRequirementGroup.Name = "_levelRequirementGroup";
            _levelRequirementGroup.Padding = new Padding(4, 3, 4, 3);
            _levelRequirementGroup.Size = new Size(91, 50);
            _levelRequirementGroup.TabIndex = 50;
            _levelRequirementGroup.TabStop = false;
            _levelRequirementGroup.Tag = "iatag_ui_level_requirement";
            _levelRequirementGroup.Text = "Level";
            toolTip1.SetToolTip(_levelRequirementGroup, "Level requirements for the item");
            // 
            // _minLevel
            // 
            _minLevel.Location = new Point(6, 17);
            _minLevel.Margin = new Padding(4, 3, 4, 3);
            _minLevel.MaxLength = 3;
            _minLevel.Name = "_minLevel";
            _minLevel.Size = new Size(34, 23);
            _minLevel.TabIndex = 46;
            _minLevel.Tag = "iatag_ui_minlevel_tooltip";
            _minLevel.Text = "0";
            _minLevel.TextAlign = HorizontalAlignment.Center;
            toolTip1.SetToolTip(_minLevel, "The minimum level required to use this item");
            _minLevel.WordWrap = false;
            // 
            // _maxLevel
            // 
            _maxLevel.Location = new Point(47, 17);
            _maxLevel.Margin = new Padding(4, 3, 4, 3);
            _maxLevel.MaxLength = 3;
            _maxLevel.Name = "_maxLevel";
            _maxLevel.Size = new Size(34, 23);
            _maxLevel.TabIndex = 47;
            _maxLevel.Tag = "iatag_ui_maxlevel_tooltip";
            _maxLevel.Text = "110";
            _maxLevel.TextAlign = HorizontalAlignment.Center;
            toolTip1.SetToolTip(_maxLevel, "The maximum level required to use this item");
            _maxLevel.WordWrap = false;
            // 
            // toolTip1
            // 
            toolTip1.ToolTipTitle = "This is:";
            // 
            // SplitSearchWindow
            // 
            AutoScaleDimensions = new SizeF(7F, 15F);
            AutoScaleMode = AutoScaleMode.Font;
            ClientSize = new Size(1532, 750);
            Controls.Add(_mainSplitter);
            FormBorderStyle = FormBorderStyle.None;
            Margin = new Padding(4, 3, 4, 3);
            Name = "SplitSearchWindow";
            Text = "SearchWindow";
            Load += SplitSearchWindow_Load;
            _mainSplitter.Panel2.ResumeLayout(false);
            _mainSplitter.Panel2.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)_mainSplitter).EndInit();
            _mainSplitter.ResumeLayout(false);
            _toolStripContainer.ContentPanel.ResumeLayout(false);
            _toolStripContainer.ResumeLayout(false);
            _toolStripContainer.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)webView21).EndInit();
            _flowPanelFilter.ResumeLayout(false);
            _flowPanelFilter.PerformLayout();
            _levelRequirementGroup.ResumeLayout(false);
            _levelRequirementGroup.PerformLayout();
            ResumeLayout(false);

        }

        private void FlowPanelFilter_Resize(object? sender, EventArgs e) {
            _searchBox.Width = Math.Max(300, _flowPanelFilter!.Width - 500);
            _searchBox.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        }
    }
}