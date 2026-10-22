using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Data;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using Dxup.Shared;
using Microsoft.Win32;

namespace Dxup.Manager
{
    public partial class MainWindow : Window
    {
        readonly DxupInstall m_install;
        readonly GameDeployer m_deployer;
        readonly ObservableCollection<GameItem> m_games = new ObservableCollection<GameItem>();
        bool m_loading;

        public MainWindow(string[] args)
        {
            InitializeComponent();
            Theme.Attach(this);
            Theme.Changed += () => { foreach (var item in m_games) item.Notify(); ShowDetails(); };
            // The manager lives in the installation, next to bin\ and games.cfg.
            m_install = new DxupInstall(AppDomain.CurrentDomain.BaseDirectory);
            m_deployer = new GameDeployer(m_install);
            VersionText.Text = Loc.F("Version", m_install.DllVersion);
            string registered = DxupInstall.RegisteredDir();
            NotInstalledBanner.Visibility = registered != null && GameDeployer.SameFolder(registered, m_install.Dir)
                ? Visibility.Collapsed : Visibility.Visible;

            FillOptions();
            // The upscaler settings only with the upscalers' runtimes installed.
            if (!Directory.Exists(Path.Combine(m_install.BinDir, "upscalers")))
                foreach (UIElement element in new UIElement[] { UpscalerCaption, UpscalerBox, UpscalerHintText, UpscalerAutoBox, UpscalerInjectBox })
                    element.Visibility = Visibility.Collapsed;
            GameList.ItemsSource = m_games;
            m_view = CollectionViewSource.GetDefaultView(m_games);
            var entries = GamesConfig.Load(m_install.GamesConfigPath);
            foreach (var entry in entries)
                m_games.Add(new GameItem(entry));
            // The window shows at once; the folders are checked in the
            // background (a game's state reads its executable and DLLs).
            RefreshAll();
            if (m_games.Count != 0)
                GameList.SelectedIndex = 0;
            SourceInitialized += (s, e) => AcceptDroppedFiles();
            Loaded += async (s, e) =>
            {
                foreach (string arg in args.Where(a => a.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) && File.Exists(a)))
                    AddGame(arg, null, null);
                await Task.Run(() =>
                {
                    foreach (var entry in entries.Where(entry => entry.Enabled))
                        GameDeployer.CleanOldFiles(entry.Dir);
                });
            };
            Loaded += async (s, e) => await LoadAdapters();
        }

        readonly ICollectionView m_view;
        int m_refresh;   // the newest refresh; an older one that ends later is dropped

        // ---- Settings lists ----

        void FillOptions()
        {
            AnisotropyBox.ItemsSource = new[] {
                new Option(Loc.T("OptGame"), null), new Option(Loc.T("OptOff"), 0), new Option("2x", 2),
                new Option("4x", 4), new Option("8x", 8), new Option("16x", 16) };
            MsaaBox.ItemsSource = new[] {
                new Option(Loc.T("OptGame"), null), new Option("2x", 2), new Option("4x", 4), new Option("8x", 8) };
            ScopeBox.ItemsSource = new[] {
                new Option(Loc.T("OptScopeGeometry"), false), new Option(Loc.T("OptScopeAll"), true) };
            SgssaaBox.ItemsSource = new[] {
                new Option(Loc.T("OptOff"), "off"), new Option(Loc.T("OptSgssaaAlpha"), "alpha"), new Option(Loc.T("OptSgssaaFull"), "true") };
            UpscalerBox.ItemsSource = new[] {
                new Option(Loc.T("OptOff"), "off"), new Option("NVIDIA DLSS", "dlss"), new Option("AMD FSR", "fsr"),
                new Option("Intel XeSS", "xess") };
            VSyncBox.ItemsSource = new[] {
                new Option(Loc.T("OptGame"), null), new Option(Loc.T("OptOff"), 0), new Option(Loc.T("OptOn"), 1) };
            FramesBox.ItemsSource = new[] {
                new Option(Loc.T("OptGame"), null), new Option("1", 1), new Option("2", 2), new Option("3", 3), new Option("4", 4) };
            RenderScaleBox.ItemsSource = new[] {
                new Option(Loc.T("OptOff"), null), new Option("90 %", 90), new Option("75 %", 75), new Option("67 %", 67),
                new Option("50 %", 50) };
            AdapterBox.ItemsSource = AdapterOptions(null);
            SpoofBox.ItemsSource = new[] {
                new Option(Loc.T("OptRealGpu"), null), new Option("NVIDIA", "nvidia"), new Option("AMD", "amd"),
                new Option("Intel", "intel") };
        }

        // The graphics cards Windows knows (WMI takes a moment: they come in
        // after the window shows).
        List<string> m_gpus = new List<string>();

        async Task LoadAdapters()
        {
            m_gpus = await Task.Run(() =>
            {
                var names = new List<string>();
                try
                {
                    using (var searcher = new System.Management.ManagementObjectSearcher("SELECT Name FROM Win32_VideoController"))
                    {
                        foreach (var gpu in searcher.Get())
                        {
                            string name = (gpu["Name"] as string)?.Trim();
                            if (!string.IsNullOrEmpty(name))
                                names.Add(name);
                        }
                    }
                }
                catch (Exception)
                {
                    // Without WMI only the game's choice and the one set.
                }
                return names;
            });
            bool loading = m_loading;
            m_loading = true;
            string current = Selected?.Entry.Settings.Adapter;
            AdapterBox.ItemsSource = AdapterOptions(current);
            Select(AdapterBox, current);
            m_loading = loading;
        }

        // By name; DXUP takes the first graphics card whose name contains the value.
        List<Option> AdapterOptions(string current)
        {
            var options = new List<Option> { new Option(Loc.T("OptGame"), null) };
            foreach (string name in m_gpus)
            {
                if (!options.Exists(o => Equals(o.Value, name.ToLowerInvariant())))
                    options.Add(new Option(name, name.ToLowerInvariant()));
            }
            if (!string.IsNullOrEmpty(current) && !options.Exists(o => Equals(o.Value, current)))
                options.Add(new Option(current, current));
            return options;
        }

        static void Select(ComboBox box, object value)
        {
            foreach (Option option in box.ItemsSource)
            {
                if (Equals(option.Value, value))
                {
                    box.SelectedItem = option;
                    return;
                }
            }
            box.SelectedIndex = 0;
        }

        static T Value<T>(ComboBox box) => box.SelectedItem is Option option && option.Value is T value ? value : default(T);
        static int? IntValue(ComboBox box) => (box.SelectedItem as Option)?.Value as int?;

        GameItem Selected => GameList.SelectedItem as GameItem;

        // ---- List and details ----

        // The games' states, checked in the background.
        async void RefreshAll()
        {
            int refresh = ++m_refresh;
            UpdateEmptyText();
            var items = m_games.ToList();
            var states = await Task.Run(() => items.Select(item => m_deployer.State(item.Entry)).ToList());
            if (refresh != m_refresh)
                return;
            for (int i = 0; i < items.Count; i++)
                items[i].SetState(states[i]);
            int outdated = m_games.Count(item => item.Known && item.State == GameState.Outdated);
            OutdatedBanner.Visibility = outdated != 0 ? Visibility.Visible : Visibility.Collapsed;
            OutdatedText.Text = Loc.F("OutdatedBanner", outdated);
            if (Selected != null && items.Contains(Selected))
                ShowDetails();
        }

        void UpdateEmptyText()
        {
            bool none = m_games.Count == 0, hidden = !none && m_view.IsEmpty;
            EmptyText.Text = Loc.T(hidden ? "NoMatch" : "NoGames");
            EmptyText.Visibility = none || hidden ? Visibility.Visible : Visibility.Collapsed;
        }

        // ---- Search ----

        void OnSearchChanged(object sender, TextChangedEventArgs e)
        {
            string search = SearchBox.Text.Trim();
            SearchHint.Visibility = SearchBox.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
            m_view.Filter = search.Length == 0 ? null : (Predicate<object>)(item => ((GameItem)item).Matches(search));
            if (Selected == null && !m_view.IsEmpty)
                GameList.SelectedIndex = 0;
            UpdateEmptyText();
        }

        void OnSelectionChanged(object sender, SelectionChangedEventArgs e) => ShowDetails();

        void ShowDetails()
        {
            var item = Selected;
            RemoveButton.IsEnabled = item != null;
            DetailPanel.Visibility = item != null ? Visibility.Visible : Visibility.Collapsed;
            SelectText.Visibility = item != null ? Visibility.Collapsed : Visibility.Visible;
            if (item == null)
                return;
            m_loading = true;
            var entry = item.Entry;
            DetailIcon.Source = item.Icon;
            NameBox.Text = entry.Name;
            ExeText.Text = entry.ExePath;
            StateText.Text = item.StateText;
            StateBadge.Background = item.StateBrush;
            ToggleButton.Content = Loc.T(entry.Enabled ? "Deactivate" : "Activate");
            ToggleButton.Style = (Style)FindResource(entry.Enabled ? "Button" : "AccentButton");
            FixButton.Visibility = item.Known && (item.State == GameState.Outdated || item.State == GameState.Missing)
                ? Visibility.Visible : Visibility.Collapsed;
            FixButton.Content = Loc.T(item.State == GameState.Outdated ? "Update" : "Repair");

            var settings = entry.Settings;
            Select(AnisotropyBox, settings.Anisotropy);
            LodSlider.Value = settings.LodBias ?? 0.0;
            UpdateLodText();
            Select(MsaaBox, settings.MsaaSamples);
            ForceBox.IsChecked = settings.MsaaForce;
            Select(ScopeBox, settings.MsaaScopeAll);
            Select(SgssaaBox, settings.Sgssaa);
            Select(UpscalerBox, settings.Upscaler);
            UpscalerAutoBox.IsChecked = settings.UpscalerAuto;
            UpscalerInjectBox.IsChecked = settings.UpscalerInject;
            SpatialBox.IsChecked = settings.SpatialUpscale;
            Select(RenderScaleBox, settings.RenderScale);
            if (!string.IsNullOrEmpty(settings.Adapter) && !((List<Option>)AdapterBox.ItemsSource).Exists(o => Equals(o.Value, settings.Adapter)))
                AdapterBox.ItemsSource = AdapterOptions(settings.Adapter);
            Select(AdapterBox, settings.Adapter);
            Select(SpoofBox, settings.SpoofVendor);
            Select(VSyncBox, settings.SyncInterval.HasValue ? (object)(settings.SyncInterval.Value == 0 ? 0 : 1) : null);
            Select(FramesBox, settings.MaxFrameLatency);
            AsyncBox.IsChecked = settings.AsyncShaders;
            PrecompileBox.IsChecked = settings.ShaderPrecompile;
            ShowLargeAddress(item);
            ShowDiagnostics(item);
            UpdateEnabledControls();
            ShowWarnings(item);
            m_loading = false;
        }

        static string AntiCheatWarning(string antiCheat) =>
            antiCheat == GameDeployer.BattleNetAntiCheat ? Loc.T("WarnBattleNet") : Loc.F("WarnAntiCheat", antiCheat);

        void ShowWarnings(GameItem item)
        {
            Warnings.Children.Clear();
            var entry = item.Entry;
            if (GameDeployer.HasLocalConfig(entry))
                AddWarning(Loc.T("WarnLocalConf"));
            string antiCheat = GameDeployer.AntiCheat(entry);
            if (antiCheat != null)
                AddWarning(AntiCheatWarning(antiCheat));
            var pe = PeFile.Read(entry.ExePath);
            if (pe != null && pe.ImportsDirect3D12Only)
                AddWarning(Loc.T("WarnD3D12"));
            if (entry.Enabled && GameDeployer.IsRunning(entry))
                AddWarning(Loc.T("WarnRunning"));
            if (entry.Enabled && Directory.Exists(entry.Dir) && DiagnosticMarkers.AnyOn(entry.Dir))
                AddWarning(Loc.T("WarnDiagnostics"));
        }

        // ---- Address space of 32-bit games ----

        void ShowLargeAddress(GameItem item)
        {
            var entry = item.Entry;
            bool? bit = LargeAddressAware.Get(entry.ExePath);
            MemorySection.Visibility = bit.HasValue ? Visibility.Visible : Visibility.Collapsed;
            if (!bit.HasValue)
                return;
            // The game's executable has the bit itself: nothing to choose.
            bool own = bit == true && !LargeAddressAware.SetByDxup(entry.ExePath);
            bool drm = !own && LargeAddressAware.SteamDrm(entry.ExePath);
            bool antiCheat = GameDeployer.AntiCheat(entry) != null;
            LaaBox.IsChecked = own || (entry.LargeAddressAware && !drm);
            LaaBox.IsEnabled = !own && !drm && (!antiCheat || entry.LargeAddressAware);
            LaaHint.Text = Loc.T(own ? "LaaOwn" : drm ? "LaaSteamDrm" : antiCheat ? "LaaAntiCheat" : "LaaHint");
        }

        // While DXUP is in the game's folder the executable changes at once;
        // else when DXUP goes in.
        void OnLaaChanged(object sender, RoutedEventArgs e)
        {
            if (m_loading || Selected == null)
                return;
            var entry = Selected.Entry;
            bool on = LaaBox.IsChecked == true;
            if (entry.Enabled)
            {
                try
                {
                    if (on)
                        LargeAddressAware.Enable(entry.ExePath);
                    else
                        LargeAddressAware.Disable(entry.ExePath);
                }
                catch (Exception ex)
                {
                    m_loading = true;
                    LaaBox.IsChecked = !on;
                    m_loading = false;
                    Error(Loc.F("ErrLaa", ex.Message));
                    return;
                }
            }
            entry.LargeAddressAware = on;
            Save();
            RefreshAll();
        }

        // ---- Diagnostics ----

        IEnumerable<CheckBox> DiagBoxes => DiagSwitches.Children.OfType<CheckBox>();

        void ShowDiagnostics(GameItem item)
        {
            var entry = item.Entry;
            bool active = entry.Enabled && Directory.Exists(entry.Dir);
            foreach (var box in DiagBoxes)
            {
                box.IsChecked = active && DiagnosticMarkers.IsOn(entry.Dir, (string)box.Tag);
                box.IsEnabled = active;
            }
            bool layer = DiagnosticMarkers.DebugLayerInstalled;
            DiagLayerMissing.Visibility = layer ? Visibility.Collapsed : Visibility.Visible;
            if (!layer)
                DiagLayerBox.IsEnabled = DiagGpuBox.IsEnabled = false;
            DiagInactive.Visibility = active ? Visibility.Collapsed : Visibility.Visible;
            bool recording = active && DiagnosticMarkers.AnyOn(entry.Dir);
            RecordButton.IsEnabled = active;
            RecordButton.Content = Loc.T(recording ? "DiagRecordStop" : "DiagRecord");
            RenderDocButton.Content = Loc.T(m_renderDoc.Value != null ? "DiagRenderDoc" : "DiagRenderDocGet");
            PixButton.Content = Loc.T(m_pix.Value != null ? "DiagPix" : "DiagPixGet");
            // PIX takes 64-bit programs only.
            bool is32Bit = PeFile.Read(entry.ExePath)?.Is32Bit == true;
            PixButton.IsEnabled = !is32Bit;
            PixButton.ToolTip = is32Bit ? Loc.T("DiagPix32") : null;
        }

        // One click for a bug report: the diagnostics that cost little enough
        // to play with (the debug layer only when Windows has it), from a
        // fresh start; pressed again, or after the package, they go off.
        static readonly string[] RecordMarkers =
            { DiagnosticMarkers.Log, DiagnosticMarkers.DebugLayer, DiagnosticMarkers.Exceptions, DiagnosticMarkers.FrameDump };

        void OnRecord(object sender, RoutedEventArgs e)
        {
            if (Selected == null)
                return;
            string dir = Selected.Entry.Dir;
            try
            {
                bool recording = DiagnosticMarkers.AnyOn(dir);
                DiagnosticMarkers.ClearAll(dir);
                DiagnosticMarkers.DeleteFrames(dir);
                if (!recording)
                {
                    foreach (string marker in RecordMarkers)
                    {
                        if (marker != DiagnosticMarkers.DebugLayer || DiagnosticMarkers.DebugLayerInstalled)
                            DiagnosticMarkers.Set(dir, marker, true);
                    }
                }
                ShowDetails();
                StatusText.Text = Loc.T(recording ? "StatusRecordStopped" : "StatusRecording");
            }
            catch (Exception ex) { Error(ex.Message); }
        }

        // For the buttons' labels; pressing one looks again.
        readonly Lazy<string> m_renderDoc = new Lazy<string>(Diagnostics.RenderDocPath);
        readonly Lazy<string> m_pix = new Lazy<string>(Diagnostics.PixPath);

        void OnDiagChanged(object sender, RoutedEventArgs e)
        {
            if (m_loading || Selected == null)
                return;
            var box = (CheckBox)sender;
            bool on = box.IsChecked == true;
            // GPU-based validation runs on the debug layer.
            m_loading = true;
            if (box == DiagGpuBox && on)
                DiagLayerBox.IsChecked = true;
            if (box == DiagLayerBox && !on)
                DiagGpuBox.IsChecked = false;
            m_loading = false;
            try
            {
                foreach (var each in DiagBoxes)
                    DiagnosticMarkers.Set(Selected.Entry.Dir, (string)each.Tag, each.IsChecked == true);
                // Saved frames are large: a new run starts without old ones, and
                // turning them off removes them.
                if (box == DiagFramesBox)
                    DiagnosticMarkers.DeleteFrames(Selected.Entry.Dir);
                StatusText.Text = Loc.T("StatusSaved");
            }
            catch (Exception ex)
            {
                Error(Loc.F("ErrSave", ex.Message));
            }
            ShowWarnings(Selected);
        }

        void OnDiagPackage(object sender, RoutedEventArgs e)
        {
            if (Selected == null)
                return;
            // One click: the zip goes to the desktop, Explorer shows it.
            var entry = Selected.Entry;
            string safeName = new string(entry.Name.Select(c => c < 128 && (char.IsLetterOrDigit(c) || c == '-') ? c : '_').ToArray());
            string folder = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
            if (string.IsNullOrEmpty(folder) || !Directory.Exists(folder))
                folder = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            string zip = Path.Combine(folder, "DXUP-Diagnose-" + safeName + "-" + DateTime.Now.ToString("yyyyMMdd-HHmmss", CultureInfo.InvariantCulture) + ".zip");
            Mouse.OverrideCursor = Cursors.Wait;
            string error;
            try { error = Diagnostics.CreatePackage(entry, GamesConfig.Format(new[] { entry }), m_install.DllVersion, zip); }
            finally { Mouse.OverrideCursor = null; }
            if (error != null)
            {
                Error(Loc.F("ErrPackage", error));
                return;
            }
            // Packed: the diagnostics go off and the saved frames go.
            try
            {
                DiagnosticMarkers.ClearAll(entry.Dir);
                DiagnosticMarkers.DeleteFrames(entry.Dir);
            }
            catch (Exception) { }
            ShowDetails();
            StatusText.Text = Loc.F("StatusPackage", zip);
            Process.Start("explorer.exe", "/select,\"" + zip + "\"");
        }

        void OnRenderDoc(object sender, RoutedEventArgs e)
        {
            if (Selected == null)
                return;
            string path = Diagnostics.RenderDocPath();
            if (path == null)
            {
                Unelevated.Start("https://renderdoc.org/");
                return;
            }
            try
            {
                Diagnostics.StartRenderDoc(path, Selected.Entry);
                StatusText.Text = Loc.T("StatusRenderDoc");
            }
            catch (Exception ex) { Error(ex.Message); }
        }

        void OnPix(object sender, RoutedEventArgs e)
        {
            if (Selected == null)
                return;
            string path = Diagnostics.PixPath();
            if (path == null)
            {
                Unelevated.Start("https://devblogs.microsoft.com/pix/download/");
                return;
            }
            try
            {
                Clipboard.SetText(Selected.Entry.ExePath);
                Diagnostics.StartPix(path, Selected.Entry);
                Dialog.Show(this, Loc.F("PixHowTo", Selected.Entry.ExePath), Dialog.Kind.Info);
            }
            catch (Exception ex) { Error(ex.Message); }
        }

        void AddWarning(string text)
        {
            var border = new Border
            {
                BorderThickness = new Thickness(1),
                CornerRadius = new CornerRadius(4),
                Padding = new Thickness(10, 6, 10, 6),
                Margin = new Thickness(0, 0, 0, 6),
                Child = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap },
            };
            border.SetResourceReference(Border.BackgroundProperty, "WarnBack");
            border.SetResourceReference(Border.BorderBrushProperty, "WarnBorder");
            Warnings.Children.Add(border);
        }

        void UpdateEnabledControls()
        {
            bool force = ForceBox.IsChecked == true;
            ScopeBox.IsEnabled = force;
            UpscalerAutoBox.IsEnabled = (Value<string>(UpscalerBox) ?? "off") != "off";
            UpscalerInjectBox.IsEnabled = UpscalerAutoBox.IsEnabled;
        }

        void UpdateLodText()
        {
            LodText.Text = LodSlider.Value == 0.0 ? Loc.T("OptGame") : LodSlider.Value.ToString("+0.00;-0.00", CultureInfo.CurrentCulture);
        }

        // ---- Editing ----

        void OnSettingChanged(object sender, RoutedEventArgs e)
        {
            UpdateEnabledControls();
            if (m_loading || Selected == null)
                return;
            var settings = Selected.Entry.Settings;
            settings.Anisotropy = IntValue(AnisotropyBox);
            settings.MsaaSamples = IntValue(MsaaBox);
            settings.MsaaForce = ForceBox.IsChecked == true;
            settings.MsaaScopeAll = Value<bool>(ScopeBox);
            settings.Sgssaa = Value<string>(SgssaaBox) ?? "off";
            settings.Upscaler = Value<string>(UpscalerBox) ?? "off";
            settings.UpscalerAuto = UpscalerAutoBox.IsChecked == true;
            settings.UpscalerInject = UpscalerInjectBox.IsChecked == true;
            settings.SpatialUpscale = SpatialBox.IsChecked == true;
            settings.RenderScale = IntValue(RenderScaleBox);
            settings.Adapter = Value<string>(AdapterBox);
            settings.SpoofVendor = Value<string>(SpoofBox);
            settings.SyncInterval = IntValue(VSyncBox);
            settings.MaxFrameLatency = IntValue(FramesBox);
            settings.AsyncShaders = AsyncBox.IsChecked == true;
            settings.ShaderPrecompile = PrecompileBox.IsChecked == true;
            // Forcing MSAA needs a sample count.
            if (settings.MsaaForce && !settings.MsaaSamples.HasValue)
            {
                settings.MsaaSamples = 4;
                m_loading = true;
                Select(MsaaBox, 4);
                m_loading = false;
            }
            Save();
        }

        void OnLodChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
        {
            if (LodText == null)
                return;
            UpdateLodText();
            if (m_loading || Selected == null)
                return;
            Selected.Entry.Settings.LodBias = LodSlider.Value == 0.0 ? (float?)null : (float)LodSlider.Value;
            Save();
        }

        void OnNameChanged(object sender, RoutedEventArgs e)
        {
            if (m_loading || Selected == null || NameBox.Text.Trim().Length == 0 || NameBox.Text == Selected.Entry.Name)
                return;
            Selected.Entry.Name = NameBox.Text.Trim();
            Selected.Notify();
            Save();
        }

        void OnNameKeyDown(object sender, KeyEventArgs e)
        {
            if (e.Key == Key.Enter)
                OnNameChanged(sender, e);
        }

        bool Save()
        {
            try
            {
                GamesConfig.Save(m_install.GamesConfigPath, m_games.Select(item => item.Entry));
                StatusText.Text = Loc.T("StatusSaved");
                return true;
            }
            catch (Exception e)
            {
                Error(Loc.F("ErrSave", e.Message));
                return false;
            }
        }

        // ---- DXUP in a game's folder ----

        void OnToggle(object sender, RoutedEventArgs e)
        {
            var item = Selected;
            if (item == null)
                return;
            var entry = item.Entry;
            if (entry.Enabled)
            {
                if (!m_deployer.Remove(entry, m_games.Select(game => game.Entry), out string error))
                {
                    Error(Loc.F("ErrRemove", error));
                    return;
                }
                entry.Enabled = false;
            }
            else
            {
                if (!m_deployer.Install(entry, out string error))
                {
                    Error(Loc.F("ErrInstall", error));
                    return;
                }
                entry.Enabled = true;
            }
            Save();
            RefreshAll();
        }

        void OnFix(object sender, RoutedEventArgs e)
        {
            var item = Selected;
            if (item == null)
                return;
            if (!m_deployer.Install(item.Entry, out string error))
                Error(Loc.F("ErrInstall", error));
            RefreshAll();
        }

        void OnUpdateAll(object sender, RoutedEventArgs e)
        {
            int updated = 0;
            var errors = new StringBuilder();
            foreach (var item in m_games.Where(game => game.State == GameState.Outdated || game.State == GameState.Missing))
            {
                if (m_deployer.Install(item.Entry, out string error))
                    updated++;
                else
                    errors.AppendLine(item.Name + ": " + error);
            }
            RefreshAll();
            StatusText.Text = Loc.F("StatusUpdated", updated);
            if (errors.Length != 0)
                Error(Loc.F("ErrInstall", errors.ToString()));
        }

        void OnOpenFolder(object sender, RoutedEventArgs e)
        {
            if (Selected != null && Directory.Exists(Selected.Entry.Dir))
                Process.Start("explorer.exe", "\"" + Selected.Entry.Dir + "\"");
        }

        void OnRemove(object sender, RoutedEventArgs e)
        {
            var item = Selected;
            if (item == null)
                return;
            if (!Dialog.Show(this, Loc.F("ConfirmRemove", item.Name), Dialog.Kind.Question, Loc.T("Yes"), Loc.T("No")))
                return;
            if (item.Entry.Enabled && !m_deployer.Remove(item.Entry, m_games.Select(game => game.Entry), out string error))
            {
                Error(Loc.F("ErrRemove", error));
                return;
            }
            m_games.Remove(item);
            Save();
            RefreshAll();
            StatusText.Text = Loc.F("StatusRemoved", item.Name);
        }

        // ---- Adding games ----

        void OnAdd(object sender, RoutedEventArgs e)
        {
            var dialog = new OpenFileDialog { Filter = Loc.T("ExeFilter"), CheckFileExists = true };
            if (dialog.ShowDialog(this) == true)
                AddGame(dialog.FileName, null, null);
        }

        void OnFind(object sender, RoutedEventArgs e)
        {
            var window = new FinderWindow(m_games.Select(item => item.Entry).ToList()) { Owner = this };
            if (window.ShowDialog() != true)
                return;
            foreach (var choice in window.Chosen)
                AddGame(choice.exe, choice.game.Name, choice.game.Id);
        }

        // Adds a game by its executable and puts DXUP into its folder.
        public void AddGame(string exePath, string name, string appId)
        {
            exePath = Path.GetFullPath(exePath);
            string dir = Path.GetDirectoryName(exePath);
            string exe = Path.GetFileName(exePath);
            var existing = m_games.FirstOrDefault(item => GameDeployer.SameFolder(item.Entry.Dir, dir)
                && string.Equals(item.Entry.Exe, exe, StringComparison.OrdinalIgnoreCase));
            if (existing != null)
            {
                GameList.SelectedItem = existing;
                return;
            }
            if (exePath.IndexOf(@"\WindowsApps\", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                Error(Loc.T("ErrStore"));
                return;
            }
            var pe = PeFile.Read(exePath);
            if (pe == null)
            {
                Error(Loc.F("ErrNotExe", exe));
                return;
            }
            if (!pe.Is64Bit && !pe.Is32Bit)
            {
                Error(Loc.F("ErrArchitecture", exe));
                return;
            }
            var entry = new GameEntry { Dir = dir, Exe = exe, AppId = appId ?? "", Name = name ?? GameName(exePath) };
            if (pe.ImportsDirect3D12Only && !Confirm(Loc.T("WarnD3D12")))
                return;
            string antiCheat = GameDeployer.AntiCheat(entry);
            if (antiCheat != null && !Confirm(AntiCheatWarning(antiCheat)))
                return;

            // A dxup.conf of the game's folder would take precedence: its
            // settings move into games.cfg.
            string conf = Path.Combine(dir, "dxup.conf");
            bool imported = false;
            if (File.Exists(conf))
            {
                try
                {
                    GamesConfig.Import(entry, File.ReadAllText(conf));
                    string target = conf + ".imported";
                    if (File.Exists(target))
                        File.Delete(target);
                    File.Move(conf, target);
                    imported = true;
                }
                catch (Exception) { }
            }

            if (!m_deployer.Install(entry, out string error))
            {
                Error(Loc.F("ErrInstall", error));
                return;
            }
            entry.Enabled = true;
            var item = new GameItem(entry);
            m_games.Add(item);
            Save();
            RefreshAll();
            GameList.SelectedItem = item;
            StatusText.Text = Loc.F("StatusAdded", entry.Name) + (imported ? "  " + Loc.T("Imported") : "");
        }

        static string GameName(string exePath)
        {
            try
            {
                var info = FileVersionInfo.GetVersionInfo(exePath);
                foreach (string name in new[] { info.ProductName, info.FileDescription })
                {
                    if (!string.IsNullOrWhiteSpace(name) && name.Length < 60)
                        return name.Trim();
                }
            }
            catch (Exception) { }
            return Path.GetFileNameWithoutExtension(exePath);
        }

        bool Confirm(string text) =>
            Dialog.Show(this, Loc.F("ConfirmContinue", text), Dialog.Kind.Warning, Loc.T("Yes"), Loc.T("No"));

        void Error(string text) => Dialog.Show(this, text, Dialog.Kind.Error);

        // ---- Drag and drop from Explorer ----

        // Explorer runs without elevation: OLE drag and drop into this
        // elevated window is blocked, WM_DROPFILES is let through.
        void AcceptDroppedFiles()
        {
            IntPtr hwnd = new WindowInteropHelper(this).Handle;
            foreach (int message in new[] { NativeMethods.WM_DROPFILES, NativeMethods.WM_COPYDATA, NativeMethods.WM_COPYGLOBALDATA })
                NativeMethods.ChangeWindowMessageFilterEx(hwnd, message, NativeMethods.MSGFLT_ALLOW, IntPtr.Zero);
            NativeMethods.DragAcceptFiles(hwnd, true);
            HwndSource.FromHwnd(hwnd)?.AddHook(WndProc);
        }

        IntPtr WndProc(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
        {
            if (message != NativeMethods.WM_DROPFILES)
                return IntPtr.Zero;
            var files = new List<string>();
            int count = NativeMethods.DragQueryFile(wParam, -1, null, 0);
            for (int i = 0; i < count; i++)
            {
                var name = new StringBuilder(32768);
                if (NativeMethods.DragQueryFile(wParam, i, name, name.Capacity) > 0)
                    files.Add(name.ToString());
            }
            NativeMethods.DragFinish(wParam);
            handled = true;
            Dispatcher.BeginInvoke(new Action(() =>
            {
                foreach (string file in files.Where(f => f.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)))
                    AddGame(file, null, null);
            }));
            return IntPtr.Zero;
        }
    }
}
