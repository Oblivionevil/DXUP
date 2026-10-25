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

        // The files the setups put into the installation, relative to it,
        // one per line after the header (an upgrade adds its own): the
        // uninstaller deletes these and leaves everything else in the
        // folder alone. Without the header it is no list of DXUP's.
        public const string FileList = "dxup-files.txt";
        public const string FileListHeader = "# DXUP setup: the files of this installation";

        // What setups before the file list installed (DXUP 1.1 and older,
        // with the upscalers' runtimes of the -Upscalers setups).
        static readonly string[] s_earlierFiles =
        {
            DxupInstall.ManagerExe, DxupInstall.ManagerExe + ".config", "README.txt", "LICENSE.txt",
            @"bin\d3d11.dll", @"bin\dxgi.dll", @"bin\ddraw.dll", @"bin\d3d8.dll", @"bin\d3d9.dll",
            @"bin32\d3d11.dll", @"bin32\dxgi.dll", @"bin32\ddraw.dll", @"bin32\d3d8.dll", @"bin32\d3d9.dll",
            @"bin\upscalers\nvngx_dlss.dll", @"bin\upscalers\amd_fidelityfx_loader_dx12.dll",
            @"bin\upscalers\amd_fidelityfx_upscaler_dx12.dll", @"bin\upscalers\libxess.dll",
            @"bin\upscalers\licenses\NVIDIA-DLSS-LICENSE.txt", @"bin\upscalers\licenses\AMD-FidelityFX-LICENSE.md",
            @"bin\upscalers\licenses\Intel-XeSS-LICENSE.txt",
        };

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

        // The files this setup brings: resource names and paths relative to
        // the installation.
        static List<(string resource, string relative)> PayloadResources() => Assembly.GetExecutingAssembly().GetManifestResourceNames()
            .Where(name => name.StartsWith(PayloadPrefix, StringComparison.Ordinal))
            .Select(name => (name, name.Substring(PayloadPrefix.Length).Replace('/', '\\'))).ToList();

        public static List<string> Payload() => PayloadResources().Select(file => file.relative).ToList();

        // ---- Which folder ----

        // Whether the folder holds a DXUP installation: the setup's file
        // list, or DXUP Manager.
        public static bool IsInstallation(string dir)
        {
            try
            {
                return Directory.Exists(dir) && (HasFileList(dir)
                    || DxupInstall.IsDxupProgram(Path.Combine(dir, DxupInstall.ManagerExe)));
            }
            catch (Exception)
            {
                return false;
            }
        }

        static bool IsRegistered(string dir, string registered) =>
            !string.IsNullOrEmpty(registered) && GameDeployer.SameFolder(registered, dir);

        // DXUP goes into a new or empty folder, or over its installation.
        public static bool MayInstallInto(string dir, string registered)
        {
            try
            {
                return !Directory.Exists(dir) || IsRegistered(dir, registered) || IsInstallation(dir)
                    || !Directory.EnumerateFileSystemEntries(dir).Any();
            }
            catch (Exception)
            {
                return false;
            }
        }

        // The folder to install into for the one chosen: a folder with other
        // files (D:\Games) gets a DXUP folder in it; null when that one has
        // other files too.
        public static string InstallFolder(string chosen, string registered)
        {
            string dir = DxupInstall.TrimFolder(Path.GetFullPath(chosen));
            if (MayInstallInto(dir, registered))
                return dir;
            string inner = Path.Combine(dir, "DXUP");
            return MayInstallInto(inner, registered) ? inner : null;
        }

        // Only an installation is uninstalled: the registered one, or a
        // folder with DXUP's files (an installation the registry lost).
        public static bool MayUninstall(string dir, string registered) =>
            IsRegistered(dir, registered) || IsInstallation(dir);

        // The installation to uninstall: the registered one, else the one
        // the uninstaller runs from, else the default folder (MayUninstall
        // decides whether it is one).
        public static string UninstallFolder(string registered)
        {
            if (!string.IsNullOrEmpty(registered))
                return registered;
            string own = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
            return IsInstallation(own) ? own : DxupInstall.DefaultDir;
        }

        // ---- Installing ----

        // Copies DXUP into dir (a new installation or an upgrade) and
        // returns the games whose DXUP DLLs could not be refreshed.
        public List<string> Install(string dir, bool desktopShortcut)
        {
            var install = new DxupInstall(dir);
            if (!MayInstallInto(install.Dir, DxupInstall.RegisteredDir()))
                throw new InvalidOperationException(Loc.F("ErrFolderNotEmpty", install.Dir));
            Directory.CreateDirectory(install.Dir);

            Log(Loc.T("StepFiles"));
            CleanOldFiles(install.Dir);
            var assembly = Assembly.GetExecutingAssembly();
            long bytes = 0;
            var payload = PayloadResources();
            // The list first: what a copy that stops halfway left is DXUP's too.
            WriteFileList(install.Dir, payload.Select(file => file.relative).Concat(new[] { DxupInstall.UninstallExe }));
            foreach (var (resource, relative) in payload)
            {
                string target = Path.Combine(install.Dir, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                using (var source = assembly.GetManifestResourceStream(resource))
                using (var file = new FileStream(target + ".new", FileMode.Create, FileAccess.Write))
                {
                    source.CopyTo(file);
                    bytes += file.Length;
                }
                ReplaceFile(target + ".new", target);
            }
            string uninstaller = Path.Combine(install.Dir, DxupInstall.UninstallExe);
            string self = assembly.Location;
            if (!string.Equals(Path.GetFullPath(self), Path.GetFullPath(uninstaller), StringComparison.OrdinalIgnoreCase))
            {
                File.Copy(self, uninstaller + ".new", true);
                ReplaceFile(uninstaller + ".new", uninstaller);
            }
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
                try
                {
                    if (!GameDeployer.FolderExists(game) || deployer.State(game) != GameState.Outdated)
                        continue;
                    if (!deployer.Install(game, out string error))
                        failed.Add($"{game.Name}: {error}");
                }
                catch (Exception e)
                {
                    failed.Add($"{game.Name}: {e.Message}");
                }
            }
            return failed;
        }

        // Puts a new file in place of target. An unchanged file stays; one
        // in use (games load the upscalers' runtimes from bin\upscalers) is
        // renamed and deleted at the next restart, so an upgrade does not
        // stop halfway.
        public static void ReplaceFile(string newFile, string target)
        {
            if (File.Exists(target))
            {
                if (LargeAddressAware.SameContent(newFile, target))
                {
                    File.Delete(newFile);
                    return;
                }
                GameDeployer.MoveAside(target);
            }
            File.Move(newFile, target);
        }

        static bool HasFileList(string dir)
        {
            try
            {
                string path = Path.Combine(dir, FileList);
                return File.Exists(path) && File.ReadLines(path).FirstOrDefault() == FileListHeader;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // The file list of the installation, relative paths inside it only.
        public static List<string> ReadFileList(string dir)
        {
            var files = new List<string>();
            try
            {
                if (HasFileList(dir))
                    files.AddRange(File.ReadAllLines(Path.Combine(dir, FileList)).Skip(1).Select(line => line.Trim()).Where(IsInside));
            }
            catch (Exception) { }
            return files;
        }

        // Adds files to the list; those of earlier versions stay in it.
        static void WriteFileList(string dir, IEnumerable<string> files)
        {
            var all = new SortedSet<string>(ReadFileList(dir), StringComparer.OrdinalIgnoreCase);
            all.UnionWith(files.Where(IsInside));
            File.WriteAllLines(Path.Combine(dir, FileList), new[] { FileListHeader }.Concat(all));
        }

        // A relative path that stays inside the folder.
        static bool IsInside(string relative)
        {
            if (string.IsNullOrWhiteSpace(relative) || relative.IndexOfAny(Path.GetInvalidPathChars()) >= 0
                || relative.IndexOf(':') >= 0 || Path.IsPathRooted(relative))
                return false;
            return !relative.Split('\\', '/').Any(part => part == ".." || part == ".");
        }

        // Leftovers of files that were in use at an earlier upgrade.
        static void CleanOldFiles(string dir)
        {
            foreach (string folder in OwnFolders(dir, Enumerable.Empty<string>()).Concat(new[] { dir }))
                GameDeployer.CleanOldFiles(folder);
        }

        // ---- Uninstalling ----

        // Removes DXUP from all games, then the installation. The uninstaller
        // itself and its folder go once it has exited (DeleteSelfAfterExit).
        public List<string> Uninstall(string dir, bool keepSettings)
        {
            var install = new DxupInstall(dir);
            if (!MayUninstall(install.Dir, DxupInstall.RegisteredDir()))
                throw new InvalidOperationException(Loc.F("ErrNotInstalled", install.Dir));
            var failed = new List<string>();

            Log(Loc.T("StepRemoveGames"));
            var deployer = new GameDeployer(install);
            var document = GamesConfig.LoadDocument(install.GamesConfigPath);
            foreach (var game in document.Games.Where(g => g.Enabled && GameDeployer.FolderExists(g)))
            {
                try
                {
                    if (!deployer.Remove(game, Enumerable.Empty<GameEntry>(), out string error, uninstalling: true))
                        failed.Add($"{game.Name}: {error}");
                }
                catch (Exception e)
                {
                    failed.Add($"{game.Name}: {e.Message}");
                }
                game.Enabled = false;
            }
            foreach (var game in document.Games)
                game.Enabled = false;

            Log(Loc.T("StepShortcuts"));
            foreach (bool desktop in new[] { false, true })
            {
                try { File.Delete(ShortcutPath(desktop)); } catch (Exception) { }
            }

            Log(Loc.T("StepRegistry"));
            DxupInstall.Unregister();

            Log(Loc.T("StepRemoveFiles"));
            if (keepSettings && File.Exists(install.GamesConfigPath))
                GamesConfig.Save(install.GamesConfigPath, document.Games, document);
            string self = Assembly.GetExecutingAssembly().Location;
            failed.AddRange(RemoveFiles(install.Dir, Payload(), keepSettings, self));
            // The running uninstaller cannot delete itself.
            string uninstaller = Path.Combine(install.Dir, DxupInstall.UninstallExe);
            if (File.Exists(uninstaller) && SamePath(uninstaller, self))
                s_selfDelete = (uninstaller, install.Dir);
            return failed;
        }

        // DXUP's files in the installation: those the setups put there (the
        // file list, this setup's payload, the files of earlier versions),
        // games.cfg (unless kept) and leftovers of files that were in use.
        // Nothing else in the folder is DXUP's.
        public static List<string> OwnFiles(string dir, IEnumerable<string> payload, bool keepSettings)
        {
            var names = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            names.UnionWith(payload.Where(IsInside));
            names.UnionWith(s_earlierFiles);
            names.UnionWith(ReadFileList(dir));
            // Copies an upgrade did not finish.
            foreach (string name in names.ToList())
                names.Add(name + ".new");
            names.Add(DxupInstall.UninstallExe);
            names.Add(DxupInstall.UninstallExe + ".new");
            names.Add(GamesConfig.FileName + ".new");
            if (!keepSettings)
                names.Add(GamesConfig.FileName);
            var files = new List<string>();
            foreach (string name in names)
            {
                string path = Path.Combine(dir, name);
                if (File.Exists(path))
                    files.Add(path);
            }
            foreach (string folder in OwnFolders(dir, names).Concat(new[] { dir }))
            {
                try
                {
                    if (Directory.Exists(folder))
                        files.AddRange(Directory.GetFiles(folder, "*" + GameDeployer.OldSuffix + "*"));
                }
                catch (Exception) { }
            }
            // The list last: an uninstall that stops halfway can run again.
            // With the settings kept it stays: the folder is still DXUP's
            // for the next installation (which takes over games.cfg).
            string list = Path.Combine(dir, FileList);
            files.RemoveAll(path => string.Equals(path, list, StringComparison.OrdinalIgnoreCase));
            files = files.Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            if (!keepSettings && HasFileList(dir))
                files.Add(list);
            return files;
        }

        // DXUP's folders in the installation, the deepest first.
        static List<string> OwnFolders(string dir, IEnumerable<string> names)
        {
            var folders = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "bin", "bin32", @"bin\upscalers", @"bin\upscalers\licenses" };
            foreach (string name in names)
            {
                for (string folder = Path.GetDirectoryName(name); !string.IsNullOrEmpty(folder); folder = Path.GetDirectoryName(folder))
                    folders.Add(folder);
            }
            return folders.OrderByDescending(folder => folder.Count(c => c == '\\')).Select(folder => Path.Combine(dir, folder)).ToList();
        }

        // Deletes DXUP's files (OwnFiles; files in use go at the next
        // restart) and its folders that are empty then, the installation
        // folder too unless self is in it. Returns what could not go.
        public static List<string> RemoveFiles(string dir, IEnumerable<string> payload, bool keepSettings, string self)
        {
            var failed = new List<string>();
            if (!Directory.Exists(dir))
                return failed;
            var names = new HashSet<string>(payload.Where(IsInside).Concat(s_earlierFiles).Concat(ReadFileList(dir)), StringComparer.OrdinalIgnoreCase);
            foreach (string file in OwnFiles(dir, payload, keepSettings))
            {
                if (self != null && SamePath(file, self))
                    continue;
                try { GameDeployer.MoveAside(file); }
                catch (Exception) { failed.Add(file); }
            }
            foreach (string folder in OwnFolders(dir, names).Concat(new[] { dir }))
                RemoveEmptyFolder(folder);
            return failed;
        }

        // An empty folder goes; one that only holds files deleted at the next
        // restart goes then too.
        static void RemoveEmptyFolder(string folder)
        {
            try
            {
                if (!Directory.Exists(folder))
                    return;
                var entries = Directory.GetFileSystemEntries(folder);
                if (entries.Length == 0)
                    Directory.Delete(folder, false);
                else if (entries.All(entry => Path.GetFileName(entry).IndexOf(GameDeployer.OldSuffix, StringComparison.OrdinalIgnoreCase) >= 0))
                    GameDeployer.DeleteAtRestart(folder);
            }
            catch (Exception) { }
        }

        static bool SamePath(string a, string b)
        {
            try
            {
                return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);
            }
            catch (Exception)
            {
                return false;
            }
        }

        // The uninstaller and its folder, when it ran from the installation.
        static (string exe, string dir)? s_selfDelete;

        // Called when the uninstaller exits. The running program can be
        // renamed, not deleted: under a name of its own (deleted at the next
        // restart, so a new installation's uninstall.exe is safe) a hidden
        // command deletes it once the process has ended, retrying for two
        // minutes, and then the folder if nothing else is left in it.
        public static void DeleteSelfAfterExit()
        {
            if (s_selfDelete == null)
                return;
            var (exe, dir) = s_selfDelete.Value;
            s_selfDelete = null;
            try
            {
                string renamed = GameDeployer.MoveAside(exe);
                if (renamed == null)
                {
                    RemoveEmptyFolder(dir);
                    return;
                }
                Process.Start(new ProcessStartInfo("cmd.exe", SelfDeleteCommand(renamed, dir))
                {
                    CreateNoWindow = true,
                    UseShellExecute = false,
                    WorkingDirectory = Path.GetTempPath(),
                });
            }
            catch (Exception) { }
        }

        // cmd.exe's arguments: until the file is gone (the process has
        // exited) up to 120 tries a second apart, then the folder when empty.
        public static string SelfDeleteCommand(string exe, string dir) =>
            $"/c for /l %i in (1,1,120) do (del /f /q \"{exe}\" 2>nul & if not exist \"{exe}\" (rmdir \"{dir}\" 2>nul & exit /b 0) & ping -n 2 127.0.0.1 >nul)";
    }
}
