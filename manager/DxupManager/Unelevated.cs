using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Dxup.Manager
{
    // Starts programs as the signed-in user without the manager's
    // administrator rights, through the desktop's Explorer, which runs
    // unelevated. A game that RenderDoc or PIX starts then runs at the same
    // level as Steam: Steam's DRM refuses an elevated game while Steam is
    // not (Application load error 5:0000065434).
    public static class Unelevated
    {
        // With environment variables, a hidden cmd sets them and starts the
        // program, which passes them on to the games it starts.
        public static void Start(string file, string arguments = "", string directory = "",
            IDictionary<string, string> environment = null)
        {
            try
            {
                if (environment == null || environment.Count == 0)
                {
                    ShellExecute(file, arguments, directory, 1 /* SW_SHOWNORMAL */);
                    return;
                }
                var command = new StringBuilder("/s /c \"");
                foreach (var variable in environment)
                    command.Append("set \"").Append(variable.Key).Append('=').Append(variable.Value).Append("\" && ");
                command.Append("start \"\" \"").Append(file).Append('"');
                if (arguments.Length != 0)
                    command.Append(' ').Append(arguments);
                command.Append('"');
                ShellExecute(Path.Combine(Environment.SystemDirectory, "cmd.exe"), command.ToString(), directory, 0 /* SW_HIDE */);
                return;
            }
            catch (Exception) { }
            // No Explorer desktop (another shell): as the manager runs.
            var info = new ProcessStartInfo(file, arguments) { UseShellExecute = environment == null, WorkingDirectory = directory };
            if (environment != null)
                foreach (var variable in environment)
                    info.EnvironmentVariables[variable.Key] = variable.Value;
            Process.Start(info);
        }

        static void ShellExecute(string file, string arguments, string directory, int show)
        {
            var windows = (IShellWindows)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("9BA05972-F6A8-11CF-A442-00A0C90A8F39")));
            object desktopLocation = 0;   // CSIDL_DESKTOP
            object root = null;
            object desktop = windows.FindWindowSW(ref desktopLocation, ref root, SwcDesktop, out _, SwfoNeedDispatch);
            Guid topLevelBrowser = new Guid("4C96BE40-915C-11CF-99D3-00AA004AE837");
            Guid shellBrowserIid = typeof(IShellBrowser).GUID;
            var browser = (IShellBrowser)((IServiceProvider)desktop).QueryService(ref topLevelBrowser, ref shellBrowserIid);
            browser.QueryActiveShellView(out IShellView view);
            Guid dispatchIid = new Guid("00020400-0000-0000-C000-000000000046");
            object folderView = view.GetItemObject(SvgioBackground, ref dispatchIid);
            object shell = folderView.GetType().InvokeMember("Application", BindingFlags.GetProperty, null, folderView, null);
            shell.GetType().InvokeMember("ShellExecute", BindingFlags.InvokeMethod, null, shell,
                new object[] { file, arguments, directory, "", show });
        }

        const int SwcDesktop = 8;
        const int SwfoNeedDispatch = 1;
        const uint SvgioBackground = 0;

        // Only the methods called here have their real signatures; the others
        // keep the vtable order.
        [ComImport, Guid("85CB6900-4D95-11CF-960C-0080C7F4EE85"), InterfaceType(ComInterfaceType.InterfaceIsDual)]
        interface IShellWindows
        {
            int Count { get; }
            void Item();
            void NewEnum();
            void Register();
            void RegisterPending();
            void Revoke();
            void OnNavigate();
            void OnActivated();
            [return: MarshalAs(UnmanagedType.IDispatch)]
            object FindWindowSW(ref object location, ref object locationRoot, int windowClass, out int hwnd, int options);
        }

        [ComImport, Guid("6D5140C1-7436-11CE-8034-00AA006009FA"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IServiceProvider
        {
            [return: MarshalAs(UnmanagedType.IUnknown)]
            object QueryService(ref Guid service, ref Guid iid);
        }

        [ComImport, Guid("000214E2-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellBrowser
        {
            void GetWindow();
            void ContextSensitiveHelp();
            void InsertMenusSB();
            void SetMenuSB();
            void RemoveMenusSB();
            void SetStatusTextSB();
            void EnableModelessSB();
            void TranslateAcceleratorSB();
            void BrowseObject();
            void GetViewStateStream();
            void GetControlWindow();
            void SendControlMsg();
            void QueryActiveShellView(out IShellView view);
        }

        [ComImport, Guid("000214E3-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        interface IShellView
        {
            void GetWindow();
            void ContextSensitiveHelp();
            void TranslateAccelerator();
            void EnableModeless();
            void UIActivate();
            void Refresh();
            void CreateViewWindow();
            void DestroyViewWindow();
            void GetCurrentInfo();
            void AddPropertySheetPages();
            void SaveViewState();
            void SelectItem();
            [return: MarshalAs(UnmanagedType.IUnknown)]
            object GetItemObject(uint item, ref Guid iid);
        }
    }
}
