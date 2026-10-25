using System;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using Dxup.Shared;

namespace Dxup.Manager
{
    public partial class App : Application
    {
        Mutex m_mutex;
        bool m_reporting;

        protected override void OnStartup(StartupEventArgs e)
        {
            base.OnStartup(e);
            Theme.Apply(this);
            Loc.Apply(this);
            DispatcherUnhandledException += OnUnhandledException;
            TaskScheduler.UnobservedTaskException += (s, a) => a.SetObserved();
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

        // An error nothing else caught (a game's folder that cannot be read,
        // a hand-edited games.cfg) is shown, and the manager keeps running
        // once its window is up; before that it cannot work and closes.
        void OnUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
        {
            e.Handled = true;
            var window = MainWindow != null && MainWindow.IsLoaded ? MainWindow : null;
            if (!m_reporting)
            {
                m_reporting = true;
                try { Dialog.Show(window, Loc.F("ErrUnexpected", e.Exception.Message), Dialog.Kind.Error); }
                catch (Exception) { }
                finally { m_reporting = false; }
            }
            if (window == null)
                Shutdown(1);
        }

        protected override void OnExit(ExitEventArgs e)
        {
            m_mutex?.Dispose();
            base.OnExit(e);
        }
    }
}
