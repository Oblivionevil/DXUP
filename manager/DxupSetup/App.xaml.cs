using System;
using System.Linq;
using System.Windows;
using Dxup.Shared;

namespace Dxup.Setup
{
    // DXUP-Setup.exe installs or upgrades DXUP; as uninstall.exe in the
    // installation, "/uninstall" removes it. "/quiet" does either without
    // windows (exit code 0 on success).
    public partial class App : Application
    {
        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);
            Theme.Apply(this);
            Loc.Apply(this);
            bool uninstall = e.Args.Any(arg => arg.Equals("/uninstall", StringComparison.OrdinalIgnoreCase));
            bool quiet = e.Args.Any(arg => arg.Equals("/quiet", StringComparison.OrdinalIgnoreCase));

            if (!Environment.Is64BitOperatingSystem || !Environment.Is64BitProcess)
            {
                if (!quiet)
                    Dialog.Show(null, Loc.T("NeedsX64"), Dialog.Kind.Error);
                Shutdown(1);
                return;
            }

            if (quiet)
            {
                Shutdown(RunQuiet(uninstall) ? 0 : 1);
                return;
            }
            var window = new SetupWindow(uninstall);
            window.Closed += (s, a) => Shutdown(0);
            window.Show();
        }

        static bool RunQuiet(bool uninstall)
        {
            try
            {
                if (Installer.ManagerRunning())
                    return false;
                var installer = new Installer();
                string dir = DxupInstall.RegisteredDir() ?? DxupInstall.DefaultDir;
                var failed = uninstall ? installer.Uninstall(dir, false) : installer.Install(dir, false);
                return failed.Count == 0;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
