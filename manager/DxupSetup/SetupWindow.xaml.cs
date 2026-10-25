using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using Dxup.Shared;

namespace Dxup.Setup
{
    public partial class SetupWindow : Window
    {
        enum Stage { Options, Working, Done }

        readonly bool m_uninstall;
        readonly string m_registered;
        Stage m_stage = Stage.Options;
        string m_dir;

        public SetupWindow(bool uninstall)
        {
            InitializeComponent();
            Theme.Attach(this);
            m_uninstall = uninstall;
            m_registered = DxupInstall.RegisteredDir();
            // Closed while it works, the setup would stop halfway.
            Closing += (s, e) => e.Cancel = m_stage == Stage.Working;
            TitleText.Text = Loc.F("SetupTitle", Installer.Version);
            SubtitleText.Text = Loc.T(uninstall ? "Uninstall" : "Subtitle");

            if (uninstall)
            {
                OptionsPage.Visibility = Visibility.Collapsed;
                UninstallPage.Visibility = Visibility.Visible;
                MainButton.Content = Loc.T("Uninstall");
                m_dir = Installer.UninstallFolder(m_registered);
                // Started outside an installation (the setup from the
                // downloads folder with /uninstall): there is nothing to
                // remove there.
                if (!Installer.MayUninstall(m_dir, m_registered))
                {
                    UninstallText.Text = Loc.F("ErrNotInstalled", m_dir);
                    KeepBox.Visibility = Visibility.Collapsed;
                    MainButton.Visibility = Visibility.Collapsed;
                    MainButton.IsEnabled = false;
                    CancelButton.Content = Loc.T("Close");
                }
                return;
            }
            DirBox.Text = m_registered ?? DxupInstall.DefaultDir;
            if (m_registered != null)
            {
                // An upgrade keeps the folder, the games and their settings.
                DirBox.IsEnabled = false;
                BrowseButton.IsEnabled = false;
                UpgradeText.Text = Loc.F("UpgradeInfo", DxupInstall.RegisteredVersion() ?? "?", Installer.Version);
                UpgradeText.Visibility = Visibility.Visible;
                MainButton.Content = Loc.T("Upgrade");
                DesktopBox.IsChecked = File.Exists(Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.CommonDesktopDirectory), "DXUP Manager.lnk"));
            }
            else
            {
                MainButton.Content = Loc.T("Install");
            }
        }

        void OnBrowse(object sender, RoutedEventArgs e)
        {
            using (var dialog = new System.Windows.Forms.FolderBrowserDialog { SelectedPath = DirBox.Text, ShowNewFolderButton = true })
            {
                if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
                    DirBox.Text = dialog.SelectedPath.EndsWith("DXUP", StringComparison.OrdinalIgnoreCase)
                        ? dialog.SelectedPath : Path.Combine(dialog.SelectedPath, "DXUP");
            }
        }

        void OnCancel(object sender, RoutedEventArgs e) => Close();

        async void OnMain(object sender, RoutedEventArgs e)
        {
            if (m_stage == Stage.Done)
            {
                if (!m_uninstall && StartBox.IsChecked == true)
                {
                    try { Process.Start(new ProcessStartInfo(Path.Combine(m_dir, DxupInstall.ManagerExe)) { UseShellExecute = true }); }
                    catch (Exception) { }
                }
                Close();
                return;
            }
            if (m_stage != Stage.Options)
                return;
            // The manager holds games.cfg and the DLLs: it has to be closed.
            while (Installer.ManagerRunning())
            {
                if (!Dialog.Show(this, Loc.T("ManagerRunning"), Dialog.Kind.Warning, "OK", Loc.T("Cancel")))
                    return;
            }
            if (!m_uninstall)
            {
                // A folder with other files (D:\Games) gets a DXUP folder in
                // it: the uninstaller removes DXUP's files only, but the
                // folder should be DXUP's alone.
                string dir;
                try { dir = Installer.InstallFolder(DirBox.Text.Trim(), m_registered); }
                catch (Exception)
                {
                    Dialog.Show(this, Loc.F("ErrFolder", DirBox.Text.Trim()), Dialog.Kind.Error);
                    return;
                }
                if (dir == null)
                {
                    Dialog.Show(this, Loc.F("ErrFolderNotEmpty", DirBox.Text.Trim()), Dialog.Kind.Error);
                    return;
                }
                m_dir = dir;
                DirBox.Text = dir;
            }
            bool desktop = DesktopBox.IsChecked == true;
            bool keep = KeepBox.IsChecked == true;

            m_stage = Stage.Working;
            OptionsPage.Visibility = Visibility.Collapsed;
            UninstallPage.Visibility = Visibility.Collapsed;
            ProgressPage.Visibility = Visibility.Visible;
            MainButton.IsEnabled = false;
            CancelButton.IsEnabled = false;
            LogBox.Text = Loc.F(m_uninstall ? "Uninstalling" : "Installing", m_dir) + Environment.NewLine;

            int steps = 0;
            var installer = new Installer
            {
                Log = line => Dispatcher.Invoke(() =>
                {
                    LogBox.AppendText("  • " + line + Environment.NewLine);
                    Progress.Value = 100.0 * steps++ / Installer.StepCount;
                }),
            };
            List<string> failed = null;
            string error = null;
            await Task.Run(() =>
            {
                try { failed = m_uninstall ? installer.Uninstall(m_dir, keep) : installer.Install(m_dir, desktop); }
                catch (Exception ex) { error = ex.Message; }
            });

            Progress.Value = error == null ? 100 : 0;
            if (error != null)
            {
                LogBox.AppendText(Environment.NewLine + Loc.F("ErrFailed", error));
                MainButton.Content = Loc.T("Close");
                m_stage = Stage.Done;
                MainButton.IsEnabled = true;
                return;
            }
            LogBox.AppendText(Environment.NewLine + (m_uninstall ? Loc.T("Uninstalled") : Loc.F("Done", Installer.Version)) + Environment.NewLine);
            if (failed.Count != 0)
                LogBox.AppendText(Environment.NewLine + Loc.F("GameErrors", string.Join(Environment.NewLine, failed)));
            if (!m_uninstall)
                StartBox.Visibility = Visibility.Visible;
            MainButton.Content = Loc.T(m_uninstall ? "Close" : "Finish");
            m_stage = Stage.Done;
            MainButton.IsEnabled = true;
        }
    }
}
