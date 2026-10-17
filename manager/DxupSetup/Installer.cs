using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Threading;
using Dxup.Shared;
using Microsoft.Win32;

namespace Dxup.Setup
{
    // Installs, upgrades and uninstalls DXUP. Steps report progress through
    // Log; failures throw.
    public sealed class Installer
    {
        const string PayloadPrefix = "payload/";
        // Steps Install and Uninstall each report through Log.
        public const int StepCount = 4;
        public Action<string> Log = _ => { };

        public static string Version
        {
            get
            {
                var version = typeof(Installer).Assembly.GetName().Version;
                return $"{version.Major}.{version.Minor}.{version.Build}";
            }
        }

        public static bool ManagerRunning()
        {
            try
            {
                if (Mutex.TryOpenExisting(DxupInstall.ManagerMutex, out var mutex))
                {
                    mutex.Dispose();
                    return true;
                }
            }
            catch (Exception) { }
            return false;
        }

        static string ShortcutPath(bool desktop) => Path.Combine(
            Environment.GetFolderPath(desktop ? Environment.SpecialFolder.CommonDesktopDirectory : Environment.SpecialFolder.CommonPrograms),
            "DXUP Manager.lnk");

        // Copies DXUP into dir (a new installation or an upgrade) and
        // returns the games whose DXUP DLLs could not be refreshed.
        public List<string> Install(string dir, bool desktopShortcut)
        {
            var install = new DxupInstall(dir);
            Directory.CreateDirectory(install.Dir);

            Log(Loc.T("StepFiles"));
            var assembly = Assembly.GetExecutingAssembly();
            long bytes = 0;
            foreach (string name in assembly.GetManifestResourceNames().Where(n => n.StartsWith(PayloadPrefix, StringComparison.Ordinal)))
            {
                string relative = name.Substring(PayloadPrefix.Length).Replace('/', '\\');
                string target = Path.Combine(install.Dir, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                using (var source = assembly.GetManifestResourceStream(name))
                using (var file = new FileStream(target + ".new", FileMode.Create, FileAccess.Write))
                {
                    source.CopyTo(file);
                    bytes += file.Length;
                }
                if (File.Exists(target))
                    File.Delete(target);
                File.Move(target + ".new", target);
            }
            string uninstaller = Path.Combine(install.Dir, DxupInstall.UninstallExe);
            string self = assembly.Location;
            if (!string.Equals(Path.GetFullPath(self), Path.GetFullPath(uninstaller), StringComparison.OrdinalIgnoreCase))
                File.Copy(self, uninstaller, true);
            bytes += new FileInfo(uninstaller).Length;
            if (!File.Exists(install.GamesConfigPath))
                GamesConfig.Save(install.GamesConfigPath, new List<GameEntry>());

            Log(Loc.T("StepRegistry"));
            DxupInstall.Register(install.Dir, Version);
            using (var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64))
            using (var key = root.CreateSubKey(DxupInstall.UninstallKey))
            {
                key.SetValue("DisplayName", "DXUP");
                key.SetValue("DisplayVersion", Version);
                key.SetValue("Publisher", "DXUP");
                key.SetValue("InstallLocation", install.Dir);
                key.SetValue("DisplayIcon", Path.Combine(install.Dir, DxupInstall.ManagerExe));
                key.SetValue("UninstallString", $"\"{uninstaller}\" /uninstall");
                key.SetValue("QuietUninstallString", $"\"{uninstaller}\" /uninstall /quiet");
                key.SetValue("URLInfoAbout", "https://github.com/Oblivionevil/DXUP");
                key.SetValue("EstimatedSize", (int)(bytes / 1024), RegistryValueKind.DWord);
                key.SetValue("NoModify", 1, RegistryValueKind.DWord);
                key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
            }

            Log(Loc.T("StepShortcuts"));
            string manager = Path.Combine(install.Dir, DxupInstall.ManagerExe);
            ShellLink.Create(ShortcutPath(false), manager, "DXUP Manager");
            if (desktopShortcut)
                ShellLink.Create(ShortcutPath(true), manager, "DXUP Manager");
            else if (File.Exists(ShortcutPath(true)))
                File.Delete(ShortcutPath(true));

            // An upgrade brings the games' copies of DXUP up to date.
            Log(Loc.T("StepGames"));
            var failed = new List<string>();
            var deployer = new GameDeployer(install);
            foreach (var game in GamesConfig.Load(install.GamesConfigPath).Where(g => g.Enabled))
            {
                if (!Directory.Exists(game.Dir) || deployer.State(game) != GameState.Outdated)
                    continue;
                if (!deployer.Install(game, out string error))
                    failed.Add($"{game.Name}: {error}");
            }
            return failed;
        }

        // Removes DXUP from all games, then the installation. The uninstaller
        // itself and its folder go once it has exited.
        public List<string> Uninstall(string dir, bool keepSettings)
        {
            var install = new DxupInstall(dir);
            var failed = new List<string>();

            Log(Loc.T("StepRemoveGames"));
            var deployer = new GameDeployer(install);
            var games = GamesConfig.Load(install.GamesConfigPath);
            foreach (var game in games.Where(g => g.Enabled && Directory.Exists(g.Dir)))
            {
                if (!deployer.Remove(game, Enumerable.Empty<GameEntry>(), out string error))
                    failed.Add($"{game.Name}: {error}");
                game.Enabled = false;
            }

            Log(Loc.T("StepShortcuts"));
            foreach (bool desktop in new[] { false, true })
            {
                try { File.Delete(ShortcutPath(desktop)); } catch (Exception) { }
            }

            Log(Loc.T("StepRegistry"));
            DxupInstall.Unregister();

            Log(Loc.T("StepRemoveFiles"));
            foreach (string file in Directory.GetFiles(install.Dir, "*", SearchOption.AllDirectories))
            {
                string name = Path.GetFileName(file);
                if (name.Equals(DxupInstall.UninstallExe, StringComparison.OrdinalIgnoreCase))
                    continue;
                if (keepSettings && name.Equals(GamesConfig.FileName, StringComparison.OrdinalIgnoreCase))
                    continue;
                try { File.Delete(file); } catch (Exception) { failed.Add(file); }
            }
            if (keepSettings && File.Exists(install.GamesConfigPath))
                GamesConfig.Save(install.GamesConfigPath, games);
            try { Directory.Delete(install.BinDir, true); } catch (Exception) { }
            try { Directory.Delete(install.BinDir32, true); } catch (Exception) { }

            // The running uninstaller cannot delete itself: a hidden command
            // does once it has exited.
            string script = $"/c ping -n 3 127.0.0.1 >nul & del /f /q \"{Path.Combine(install.Dir, DxupInstall.UninstallExe)}\"";
            if (!keepSettings)
                script += $" & rmdir /q \"{install.Dir}\"";
            Process.Start(new ProcessStartInfo("cmd.exe", script)
            {
                CreateNoWindow = true,
                UseShellExecute = false,
                WorkingDirectory = Path.GetTempPath(),
            });
            return failed;
        }
    }
}
