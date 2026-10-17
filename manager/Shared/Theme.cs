using System;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using Microsoft.Win32;

namespace Dxup.Shared
{
    // Light or dark, as Windows' app mode, and again whenever the user
    // switches it: the color dictionary (Themes/Light.xaml, Themes/Dark.xaml)
    // is swapped and the title bars follow.
    public static class Theme
    {
        static ResourceDictionary s_colors;
        public static bool Dark { get; private set; }
        public static event Action Changed;

        public static void Apply(Application app)
        {
            app.Resources.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri("Themes/Controls.xaml", UriKind.Relative) });
            Update(app);
            SystemEvents.UserPreferenceChanged += (sender, e) =>
                app.Dispatcher.BeginInvoke(new Action(() => Update(app)));
        }

        static bool WindowsUsesDarkApps()
        {
            // DXUP_THEME=dark|light pins the theme (for checking both).
            string pinned = Environment.GetEnvironmentVariable("DXUP_THEME");
            if (pinned == "dark" || pinned == "light")
                return pinned == "dark";
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize"))
                    return key?.GetValue("AppsUseLightTheme") is int light && light == 0;
            }
            catch (Exception)
            {
                return false;
            }
        }

        static void Update(Application app)
        {
            bool dark = WindowsUsesDarkApps();
            if (s_colors != null && dark == Dark)
                return;
            Dark = dark;
            var colors = new ResourceDictionary { Source = new Uri(dark ? "Themes/Dark.xaml" : "Themes/Light.xaml", UriKind.Relative) };
            if (s_colors != null)
                app.Resources.MergedDictionaries.Remove(s_colors);
            app.Resources.MergedDictionaries.Insert(0, colors);
            s_colors = colors;
            foreach (Window window in app.Windows)
                SetTitleBar(window);
            Changed?.Invoke();
        }

        // Windows paints the title bar: dark mode asks it for a dark one.
        public static void Attach(Window window)
        {
            window.SourceInitialized += (sender, e) => SetTitleBar(window);
        }

        static void SetTitleBar(Window window)
        {
            IntPtr hwnd = new WindowInteropHelper(window).Handle;
            if (hwnd == IntPtr.Zero)
                return;
            int value = Dark ? 1 : 0;
            // DWMWA_USE_IMMERSIVE_DARK_MODE: 20, 19 before Windows 10 20H1.
            if (DwmSetWindowAttribute(hwnd, 20, ref value, sizeof(int)) != 0)
                DwmSetWindowAttribute(hwnd, 19, ref value, sizeof(int));
        }

        [DllImport("dwmapi.dll")]
        static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    }
}
