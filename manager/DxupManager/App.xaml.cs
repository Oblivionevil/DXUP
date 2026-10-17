using System;
using System.Threading;
using System.Windows;
using Dxup.Shared;

namespace Dxup.Manager
{
    public partial class App : Application
    {
        Mutex m_mutex;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);
            Theme.Apply(this);
            Loc.Apply(this);
            // One manager at a time; the setup waits for it to close.
            m_mutex = new Mutex(true, DxupInstall.ManagerMutex, out bool created);
            if (!created)
            {
                Dialog.Show(null, Loc.T("AlreadyRunning"), Dialog.Kind.Info);
                Shutdown();
                return;
            }
            // Executables passed on the command line (or dropped onto the
            // manager's icon) are added as games.
            new MainWindow(e.Args).Show();
        }

        protected override void OnExit(ExitEventArgs e)
        {
            m_mutex?.Dispose();
            base.OnExit(e);
        }
    }
}
