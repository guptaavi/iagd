using EvilsoftCommons.Exceptions;
using EvilsoftCommons.SingleInstance;
using IAGrim.Backup.Cloud;
using IAGrim.Database;
using IAGrim.Database.Interfaces;
using IAGrim.Database.Migrations;
using IAGrim.Parsers.GameDataParsing.Service;
using IAGrim.Services;
using IAGrim.Settings;
using IAGrim.UI;
using IAGrim.UI.Misc.CEF;
using IAGrim.Utilities;
using IAGrim.Utilities.HelperClasses;
using log4net;
using log4net.Config;
using NHibernate.SqlCommand;
using StatTranslator;
using System.Reflection;

namespace IAGrim
{
    internal static class Program {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(Program));
        private static MainWindow? _mw;
        private static readonly StartupService StartupService = new StartupService();

        private static void LoadUuid(SettingsService settings) {
            var uuid = settings.GetPersistent().UUID;

            if (string.IsNullOrEmpty(uuid)) {
                uuid = Guid.NewGuid().ToString().Replace("-", "");
                settings.GetPersistent().UUID = uuid;
            }

            RuntimeSettings.Uuid = uuid;
            ExceptionReporter.Uuid = uuid;
        }

        public static MainWindow? MainWindow => _mw;

        /// <summary>
        /// Builds the session factory ahead of the first caller that needs it.
        ///
        /// Purely an overlap: a failure here is swallowed, because <see cref="SessionFactory"/> caches it on the
        /// shared Lazy and rethrows it to whoever asks next -- on their thread, with their error handling intact.
        /// </summary>
        private static void WarmUpDatabase() {
            var thread = new Thread(() => {
                ExceptionReporter.EnableLogUnhandledOnThread();

                try {
                    var sw = System.Diagnostics.Stopwatch.StartNew();
                    SessionFactory.Warmup();
                    Logger.Info($"[timing] Session factory warmup took {sw.ElapsedMilliseconds} ms");
                }
                catch (Exception ex) {
                    Logger.Debug($"Session factory warmup failed, deferring to the first real caller: {ex.Message}");
                }
            });

            // Never hold up process exit; the second-instance path bails out long before this finishes.
            thread.IsBackground = true;
            thread.Name = "DbWarmup";
            thread.Start();
        }


        /// <summary>
        /// Scales every WinForms control by enlarging the application's default font, for large or
        /// high-density displays where the stock 8.25pt UI is unreadable.
        ///
        /// Read from IAGD_UI_SCALE (e.g. "1.5"); unset or 1.0 leaves the UI exactly as upstream ships it.
        /// Must run after ApplicationConfiguration.Initialize() (which sets its own default font) and before
        /// any form is constructed, because WinForms resolves control fonts at construction time.
        ///
        /// Forms with AutoScaleMode.Font re-layout around the larger font, so this moves the whole UI rather
        /// than overlapping it. NativeItemGrid scales its rows and icons off the same number.
        /// </summary>
        public static float UiScale { get; private set; } = 1.0f;

        private static void ApplyUiScale() {
            var raw = Environment.GetEnvironmentVariable("IAGD_UI_SCALE");
            if (string.IsNullOrWhiteSpace(raw)) {
                return;
            }

            if (!float.TryParse(raw, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var scale)) {
                Logger.Warn($"Ignoring IAGD_UI_SCALE=\"{raw}\": not a number");
                return;
            }

            // Below 1 the designer-fixed panels start clipping; above 4 nothing fits on screen at all.
            scale = Math.Clamp(scale, 1.0f, 4.0f);
            if (Math.Abs(scale - 1.0f) < 0.01f) {
                return;
            }

            UiScale = scale;

            // Deliberately NOT Application.SetDefaultFont: that only reaches controls with no font of their
            // own, and mixing it with the tree walk in UiScaler applies the factor twice to everything else.
            // UiScaler handles both cases from one rule.
            Logger.Info($"UI scale {scale:0.##}x requested");
        }

        /// <summary>
        /// Stops the tab control from covering the status bar after a font change.
        ///
        /// tabControl1 is ANCHORED (Top|Bottom|Left|Right) rather than docked, and the status strip below it
        /// is docked Bottom. Changing the form's font makes WinForms run PerformAutoScale, which scales the
        /// anchored control's height - on a maximised window the form cannot grow to match, so the tab control
        /// simply overshoots and paints over the status bar. The item count and version disappear with it.
        /// </summary>
        private static void KeepStatusBarVisible(Form form) {
            var status = form.Controls.Find("statusStrip", true).FirstOrDefault();
            if (status == null) {
                return;
            }

            status.BringToFront();

            foreach (Control sibling in form.Controls) {
                if (ReferenceEquals(sibling, status) || sibling.Bottom <= status.Top) {
                    continue;
                }

                var height = status.Top - sibling.Top;
                if (height > 50) {
                    sibling.Height = height;
                    Logger.Info($"Trimmed {sibling.Name} to {height}px so the status bar stays visible");
                }
            }
        }

        /// <summary>
        /// Keeps the window inside the screen's working area.
        ///
        /// The restored size comes from settings written on a previous run, and a larger UI scale grows the
        /// minimum size on top of that, so the window can easily end up taller than the display - with the
        /// status bar (item count, version) pushed off the bottom edge where it cannot be reached.
        /// </summary>
        private static void ClampToWorkingArea(Form form) {
            if (form.WindowState != FormWindowState.Normal) {
                return;
            }

            var area = Screen.FromControl(form).WorkingArea;
            var width = Math.Min(form.Width, area.Width);
            var height = Math.Min(form.Height, area.Height);

            if (width != form.Width || height != form.Height) {
                form.Size = new Size(width, height);
                Logger.Info($"Clamped window to the working area: {width}x{height}");
            }

            var left = Math.Max(area.Left, Math.Min(form.Left, area.Right - width));
            var top = Math.Max(area.Top, Math.Min(form.Top, area.Bottom - height));
            if (left != form.Left || top != form.Top) {
                form.Location = new Point(left, top);
            }
        }

        /// <summary>
        ///  The main entry point for the application.
        /// </summary>
        [STAThread]
        static void Main(string[] args) {
            if (Thread.CurrentThread.Name == null) {
                Thread.CurrentThread.Name = "Main";
                Thread.CurrentThread.CurrentUICulture = new System.Globalization.CultureInfo("en-US");
            }

            var logRepository = LogManager.GetRepository(Assembly.GetEntryAssembly() ?? Assembly.GetExecutingAssembly());
            XmlConfigurator.Configure(logRepository, new FileInfo("log4net.config"));


            Logger.Info("Starting IA:GD..");
            ExceptionReporter.UrlStats = "https://webstats.evilsoft.net/report/iagd";
            SQLitePCL.Batteries.Init();


            // To customize application configuration such as set high DPI settings or default font,
            // see https://aka.ms/applicationconfiguration.
            ApplicationConfiguration.Initialize();

            ApplyUiScale();


            // Compiling the NHibernate mappings is ~0.5s of work that nothing before Run() depends on, so it runs
            // alongside the version checks and the diagnostics dump instead of after them. The factory is a shared
            // Lazy, so Migrate() below either finds it built or blocks until it is.
            WarmUpDatabase();

            Logger.Info("Starting exception monitor for bug reports..");
            Logger.Debug("Anonymous usage statistics can be seen at https://webstats.evilsoft.net/iagd");
            ExceptionReporter.EnableLogUnhandledOnThread();

            Uris.Initialize(Uris.EnvCloud);
            StartupService.Init();

            if (DiagnosticsReport.IsRequested(args)) {
                var reportPath = DiagnosticsReport.WriteAndOpen();
                MessageBox.Show(
                    reportPath != null
                        ? $"Diagnostics written to:\n{reportPath}\n\nAttach this file to your bug report."
                        : "The diagnostics report could not be written. See the log for details.",
                    "Item Assistant diagnostics", MessageBoxButtons.OK, MessageBoxIcon.Information);

                LogManager.Shutdown();
                System.Environment.Exit(0);
            }

            // Into the ordinary log as well, so every log file a user sends already carries it.
            DiagnosticsReport.LogAtStartup();



#if DEBUG
            Uris.Initialize(Uris.EnvLocalDev);
#endif

            // Prevent running in RELEASE mode by accident
            // And thus risking the live database
#if !DEBUG
            if (System.Diagnostics.Debugger.IsAttached) {
                Logger.Fatal("Debugger attached, please run in DEBUG mode");
                return;
            }
#endif

            ItemHtmlWriter.CopyMissingFiles();

            Guid guid = new Guid("{F3693953-C090-4F93-86A2-B98AB96A9368}");
            var safeMode = StartupService.IsSafeMode(args);
            using (SingleInstance singleInstance = new SingleInstance(guid)) {
                if (singleInstance.IsFirstInstance) {
                    Logger.Info("Calling run..");
                    singleInstance.ListenForArgumentsFromSuccessiveInstances();
                    Application.EnableVisualStyles();
                    Application.SetCompatibleTextRenderingDefault(false);
                    Logger.Info("Visual styles enabled..");
                    Run(args);
                }
                else {
                    // Nothing listens for arguments from successive instances, so a safe mode reset here would just be overwritten by the running instance when it stores its window position on exit.
                    if (safeMode) {
                        Logger.Info("Safe mode requested, but IA is already running.");
                        MessageBox.Show(
                            "Item Assistant is already running, look for the icon in the system tray next to the clock.\n\n"
                            + "Close the running instance and then start safe mode again.",
                            "Item Assistant is already running", MessageBoxButtons.OK, MessageBoxIcon.Information);
                    }

                    // Ask the running instance to show itself, otherwise starting IA a second time looks like
                    // nothing happened at all: the window may well be hidden away in the system tray.
                    ShowExistingInstanceMessage.Notify();

                    Logger.Info("Already has an instance of IA Running, exiting..");
                }
            }

            Logger.Info("IA Exited");
            LogManager.Shutdown();
            System.Environment.Exit(0);


        }

        private static void DumpTranslationTemplate() {
            try {
                var translationsDir = Path.Combine(AppContext.BaseDirectory, @"..\..\..\..\IAGrim\Resources\translations");
                translationsDir = Path.GetFullPath(translationsDir);
                if (!Directory.Exists(translationsDir)) {
                    Logger.Debug($"Translations directory not found: {translationsDir}");
                    return;
                }

                var english = new EnglishLanguage(new Dictionary<string, string>());
                var englishEntries = english.Stats;

                foreach (var filePath in Directory.GetFiles(translationsDir, "*.txt")) {
                    var lines = File.ReadAllLines(filePath).ToList();
                    var existingKeys = new HashSet<string>();

                    foreach (var line in lines) {
                        var trimmed = line.Trim();
                        if (string.IsNullOrEmpty(trimmed) || trimmed.StartsWith("#"))
                            continue;

                        var eqIndex = trimmed.IndexOf('=');
                        if (eqIndex > 0) {
                            existingKeys.Add(trimmed.Substring(0, eqIndex));
                        }
                    }

                    var missingKeys = englishEntries.Keys
                        .Where(k => !existingKeys.Contains(k))
                        .OrderBy(k => k)
                        .ToList();

                    if (missingKeys.Count > 0) {
                        lines.Add("");
                        lines.Add("# Missing translations (English defaults)");
                        foreach (var key in missingKeys) {
                            lines.Add($"{key}={englishEntries[key].Replace("\n", "\\n")}");
                        }

                        File.WriteAllLines(filePath, lines);
                        Logger.Debug($"Added {missingKeys.Count} missing keys to {Path.GetFileName(filePath)}");
                    }
                }
            }
            catch (Exception ex) {
                Logger.Debug("Error syncing translation files", ex);
            }
        }

        private static void Run(string[] args) {
            var startupTimer = System.Diagnostics.Stopwatch.StartNew();
            void Timed(string step) {
                Logger.Info($"[timing] {step} took {startupTimer.ElapsedMilliseconds} ms");
                startupTimer.Restart();
            }

            var factory = new SessionFactory();
            Logger.Debug("Executing DB migrations..");
            new MigrationHandler(factory).Migrate();
            Timed("DB migrations");

            var serviceProvider = ServiceProvider.Initialize();
            Timed("ServiceProvider.Initialize");

            var settingsService = serviceProvider.Get<SettingsService>();

            // Must happen before the main window is created, it reads these settings on construction.
            if (StartupService.IsSafeMode(args)) {
                StartupService.ResetWindowSettings(settingsService);
                Timed("Safe mode reset");
            }

            var databaseItemDao = serviceProvider.Get<IDatabaseItemDao>();
            RuntimeSettings.InitializeLanguage(settingsService.GetLocal().LanguageCode, databaseItemDao.GetTagDictionary());
            Timed("InitializeLanguage");
#if DEBUG
            DumpTranslationTemplate();
            Timed("DumpTranslationTemplate");
#endif

            Logger.Debug("Loading UUID");
            LoadUuid(settingsService);
            Timed("LoadUuid");

            // Persist Wine detection so the injected DLL can read it from the settings file
            settingsService.GetPersistent().IsRunningInWine = WineDetector.IsRunningInWine();
            Logger.Info($"Wine detection: {settingsService.GetPersistent().IsRunningInWine}");


            var itemTagDao = serviceProvider.Get<IItemTagDao>();
            var databaseItemStatDao = serviceProvider.Get<IDatabaseItemStatDao>();
            var itemSkillDao = serviceProvider.Get<IItemSkillDao>();
            ParsingService parsingService = new ParsingService(itemTagDao, string.Empty, databaseItemDao, databaseItemStatDao, itemSkillDao, settingsService.GetLocal().LanguageCode);

            // Before the main window exists: this is modal, and it may reload the language.
            var grimDawnDetector = serviceProvider.Get<GrimDawnDetector>();
            var autoParsed = StartupService.PerformMissingExpansionDataCheck(
                parsingService,
                databaseItemDao,
                serviceProvider.Get<IPlayerItemDao>(),
                grimDawnDetector,
                settingsService
            );
            Timed("PerformMissingExpansionDataCheck");

            // Only if the parse above didn't already run, it parses in the current language anyway.
            if (!autoParsed) {
                autoParsed = StartupService.PerformLanguageChangeCheck(
                    parsingService,
                    databaseItemDao,
                    serviceProvider.Get<IPlayerItemDao>(),
                    grimDawnDetector,
                    settingsService
                );
                Timed("PerformLanguageChangeCheck");
            }

            StartupService.PrintStartupInfo(factory, settingsService);

            // TODO: Offload to the new language loader
            if (RuntimeSettings.Language is EnglishLanguage language) {
                foreach (var tag in itemTagDao.GetClassItemTags()) {
                    if (tag.Tag == null || tag.Name == null) {
                        continue;
                    }
                    language.SetTagIfMissing(tag.Tag, tag.Name);
                }
            }


            _mw = new MainWindow(
                serviceProvider,
                parsingService
            );

            Logger.Info("Checking for database updates..");

            // An automatic parse already queued a full icon extraction, no need to scan the arc files twice.
            if (!autoParsed) {
                StartupService.PerformIconCheck(grimDawnDetector, settingsService);
            }


            if (settingsService.GetPersistent().DarkMode) {
                Application.SetColorMode(SystemColorMode.Dark);
            }
            _mw.Visible = false;
            if (new DonateNagScreen(settingsService).CanNag)
                Application.Run(new DonateNagScreen(settingsService));

            Logger.Info("Running the main application..");


            StartupService.PerformGrimUpdateCheck(settingsService);

            // Self-heal a WAL that bloated from a previous unclean shutdown (e.g. a crash or the
            // debugger being stopped). Runs off the UI thread so it never delays the window.
            System.Threading.Tasks.Task.Run(() => factory.Checkpoint());

            // After Shown, so every tab and its designer-assigned fonts exist to be walked.
            _mw.Shown += (_, _) => {
                if (UiScale > 1.0f) {
                    UI.Misc.UiScaler.Apply(_mw, UiScale);
                }

                ClampToWorkingArea(_mw);
                KeepStatusBarVisible(_mw);
            };

            Application.Run(_mw);

            // Truncate the WAL back into the main db on a clean exit so the next launch starts fast.
            factory.Checkpoint();

            Logger.Info("Application ended.");
        }
    }
}