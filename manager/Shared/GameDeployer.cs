using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace Dxup.Shared
{
    public enum GameState
    {
        Inactive,      // DXUP is not in the game's folder
        Active,        // DXUP's current DLLs are in the folder
        Outdated,      // DXUP's DLLs in the folder differ from the installed ones (or the
                       // executable lost the large address aware bit, e.g. to an update)
        Missing,       // set up, but DXUP's DLLs are gone (e.g. the launcher repaired the game)
        FolderMissing, // set up, but the game's folder is gone or cannot be reached (a drive
                       // that is not connected, a path with characters paths cannot hold)
    }

    // Puts DXUP's DLLs into a game's folder and takes them out again. The
    // folder is the unit: games of the same folder share the DLLs. DLLs of
    // other software found there are kept as *.dxup-backup and come back
    // when DXUP is removed. DLLs in use (the game runs) are renamed and
    // deleted later, as Windows allows renaming a mapped file.
    public sealed class GameDeployer
    {
        public const string BackupSuffix = ".dxup-backup";
        public const string OldSuffix = ".dxup-old";
        readonly DxupInstall m_install;

        public GameDeployer(DxupInstall install) { m_install = install; }

        public GameState State(GameEntry game)
        {
            if (!game.Enabled)
                return GameState.Inactive;
            if (!FolderExists(game))
                return GameState.FolderMissing;
            bool any = false, all = true, current = true;
            string sourceDir = SourceDir(game);
            foreach (string dll in DllsFor(game))
            {
                string source = Path.Combine(sourceDir, dll);
                if (!File.Exists(source))
                    continue;
                string target = Path.Combine(game.Dir, dll);
                if (!DxupInstall.IsDxupFile(target))
                {
                    all = false;
                    continue;
                }
                any = true;
                if (DxupInstall.Hash(target) != DxupInstall.Hash(source))
                    current = false;
            }
            if (!any)
                return GameState.Missing;
            if (LargeAddressAware.Wanted(game) && LargeAddressAware.Get(game.ExePath) == false)
                current = false;
            return all && current ? GameState.Active : GameState.Outdated;
        }

        // The DLLs a game gets: DXUP's Direct3D 11 and DXGI, and those of the
        // older APIs the game uses: its executable or an engine DLL next to it
        // imports them (Risen's Engine.dll imports d3d9.dll), or the
        // executable loads them by name. Not every DLL that merely names one:
        // Bink video looks for ddraw.dll in Direct3D 9 games and should keep
        // getting Windows' DirectDraw. DXUP's DLLs already in the folder (an
        // older setup or a copy by hand) are kept up to date.
        public static List<string> DllsFor(GameEntry game)
        {
            var dlls = new List<string>(DxupInstall.Dlls);
            if (!game.ValidPath)
                return dlls;
            HashSet<string> used = null;
            foreach (string dll in DxupInstall.LegacyDlls)
            {
                if (DxupInstall.IsDxupFile(Path.Combine(game.Dir, dll)) || (used ?? (used = UsedLegacyDlls(game))).Contains(dll))
                    dlls.Add(dll);
            }
            return dlls;
        }

        // The older APIs' DLLs the executable imports or names, or a DLL in
        // its folder imports. Worked out once while the executable and the
        // folder's DLLs stay the same: reading a 100 MB executable takes a
        // while, and the list asks again at every refresh.
        static HashSet<string> UsedLegacyDlls(GameEntry game)
        {
            string signature = FolderSignature(game);
            lock (s_usage)
            {
                if (s_usage.TryGetValue(game.ExePath, out var cached) && cached.signature == signature)
                    return cached.dlls;
            }
            var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            string exe = game.ExePath;
            if (!string.IsNullOrEmpty(game.Exe) && File.Exists(exe))
            {
                var pe = PeFile.Read(exe);
                foreach (string dll in DxupInstall.LegacyDlls)
                {
                    if (pe?.ImportsDll(dll) == true)
                        used.Add(dll);
                }
                // LoadLibrary("d3d9.dll") or LoadLibraryW(L"d3d9.dll"): the name is in the file.
                used.UnionWith(NamesIn(exe, DxupInstall.LegacyDlls.Where(dll => !used.Contains(dll)).ToList()));
            }
            try
            {
                foreach (string module in Directory.GetFiles(game.Dir, "*.dll"))
                {
                    if (IsOwnDll(Path.GetFileName(module)))
                        continue;
                    var pe = PeFile.Read(module);
                    foreach (string dll in DxupInstall.LegacyDlls)
                    {
                        if (pe?.ImportsDll(dll) == true)
                            used.Add(dll);
                    }
                }
            }
            catch (Exception) { }
            lock (s_usage)
                s_usage[game.ExePath] = (signature, used);
            return used;
        }

        static readonly Dictionary<string, (string signature, HashSet<string> dlls)> s_usage =
            new Dictionary<string, (string, HashSet<string>)>(StringComparer.OrdinalIgnoreCase);

        static bool IsOwnDll(string name) =>
            DxupInstall.Dlls.Concat(DxupInstall.LegacyDlls).Any(own => string.Equals(own, name, StringComparison.OrdinalIgnoreCase));

        // Sizes and times of the executable and the folder's DLLs (without
        // DXUP's, which come and go with DXUP).
        static string FolderSignature(GameEntry game)
        {
            var text = new StringBuilder();
            try
            {
                var exe = new FileInfo(game.ExePath);
                if (exe.Exists)
                    text.Append(exe.Length).Append('/').Append(exe.LastWriteTimeUtc.Ticks);
                foreach (var dll in new DirectoryInfo(game.Dir).EnumerateFiles("*.dll"))
                {
                    if (!IsOwnDll(dll.Name))
                        text.Append('|').Append(dll.Name).Append('/').Append(dll.Length).Append('/').Append(dll.LastWriteTimeUtc.Ticks);
                }
            }
            catch (Exception) { }
            return text.ToString();
        }

        // Which of the names (lower case ASCII) occur in the file, in any
        // case, as ASCII or UTF-16 (wide string literals): one pass over the
        // file in large blocks, without decoding it.
        public static IEnumerable<string> NamesIn(string path, IList<string> names)
        {
            var found = new List<string>();
            if (names.Count == 0)
                return found;
            // Each name twice: its ASCII bytes, and with a zero byte after
            // each (UTF-16LE). Letters match in either case (bit 0x20),
            // digits, dots and the zero bytes exactly.
            var patterns = new List<(int name, byte[] bytes, byte[] fold)>();
            for (int n = 0; n < names.Count; n++)
            {
                byte[] ascii = Encoding.ASCII.GetBytes(names[n].ToLowerInvariant());
                patterns.Add((n, ascii, ascii.Select(b => (byte)(b >= 'a' && b <= 'z' ? 0x20 : 0)).ToArray()));
                byte[] wide = Encoding.Unicode.GetBytes(names[n].ToLowerInvariant());
                patterns.Add((n, wide, wide.Select(b => (byte)(b >= 'a' && b <= 'z' ? 0x20 : 0)).ToArray()));
            }
            var done = new bool[names.Count];
            int longest = patterns.Max(pattern => pattern.bytes.Length);
            // The bytes a name can start with.
            var starts = new bool[256];
            foreach (var (_, bytes, fold) in patterns)
            {
                starts[bytes[0]] = true;
                starts[bytes[0] & ~fold[0]] = true;
            }
            try
            {
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                    1 << 16, FileOptions.SequentialScan))
                {
                    var buffer = new byte[(4 << 20) + longest];
                    int kept = 0;
                    long remaining = stream.Length;
                    while (remaining > 0 && found.Count < names.Count)
                    {
                        int read = stream.Read(buffer, kept, buffer.Length - kept);
                        if (read <= 0)
                            break;
                        remaining -= read;
                        int end = kept + read;
                        // A name across the block's end is found in the next block.
                        int scanEnd = remaining > 0 ? Math.Max(0, end - longest + 1) : end;
                        for (int i = 0; i < scanEnd; i++)
                        {
                            if (!starts[buffer[i]])
                                continue;
                            foreach (var (name, pattern, fold) in patterns)
                            {
                                if (done[name] || (buffer[i] | fold[0]) != pattern[0] || i + pattern.Length > end)
                                    continue;
                                int j = 1;
                                while (j < pattern.Length && (buffer[i + j] | fold[j]) == pattern[j])
                                    j++;
                                if (j == pattern.Length)
                                {
                                    done[name] = true;
                                    found.Add(names[name]);
                                }
                            }
                        }
                        kept = end - scanEnd;
                        Buffer.BlockCopy(buffer, scanEnd, buffer, 0, kept);
                    }
                }
            }
            catch (Exception) { }
            return found;
        }

        // The DLLs for the game's executable: 32-bit games load 32-bit DLLs.
        public string SourceDir(GameEntry game)
        {
            string exe = string.IsNullOrEmpty(game.Exe) || !game.ValidPath ? null : game.ExePath;
            var pe = exe != null && File.Exists(exe) ? PeFile.Read(exe) : null;
            return pe != null && pe.Is32Bit ? m_install.BinDir32 : m_install.BinDir;
        }

        // A dxup.conf in the game's folder overrides games.cfg.
        public static bool HasLocalConfig(GameEntry game) => game.ValidPath && File.Exists(Path.Combine(game.Dir, "dxup.conf"));

        public static bool FolderExists(GameEntry game) => game.ValidPath && Directory.Exists(game.Dir);

        // Copies DXUP's DLLs into the game's folder. Returns false and an
        // error text when that failed.
        public bool Install(GameEntry game, out string error)
        {
            error = null;
            if (!FolderExists(game))
            {
                error = Loc.F("ErrFolderMissing", game.Dir);
                return false;
            }
            // The executable first (locked while the game runs): when it
            // cannot change, nothing else does.
            bool laaSet = false;
            try
            {
                if (LargeAddressAware.Wanted(game) && LargeAddressAware.Get(game.ExePath) == false)
                {
                    LargeAddressAware.Enable(game.ExePath);
                    laaSet = true;
                }
            }
            catch (Exception e)
            {
                error = e.Message;
                return false;
            }
            try
            {
                foreach (string dll in DllsFor(game))
                {
                    string source = Path.Combine(SourceDir(game), dll);
                    if (!File.Exists(source))
                        continue;
                    string target = Path.Combine(game.Dir, dll);
                    if (File.Exists(target))
                    {
                        if (DxupInstall.IsDxupFile(target))
                        {
                            if (DxupInstall.Hash(target) == DxupInstall.Hash(source))
                                continue;
                            MoveAside(target);
                        }
                        else
                        {
                            // Another program's DLL: it is kept. One backed up
                            // before (the program was updated over DXUP's DLL
                            // meanwhile) keeps a name of its own; the newest
                            // comes back when DXUP goes.
                            string backup = target + BackupSuffix;
                            if (File.Exists(backup))
                            {
                                if (LargeAddressAware.SameContent(target, backup))
                                    MoveAside(target);
                                else
                                    File.Move(backup, OlderBackupName(target));
                            }
                            if (File.Exists(target))
                                File.Move(target, backup);
                        }
                    }
                    File.Copy(source, target, false);
                }
                return true;
            }
            catch (Exception e)
            {
                error = e.Message;
                // DXUP did not get into the folder: the executable as it was.
                if (laaSet)
                {
                    try { LargeAddressAware.Disable(game.ExePath); } catch (Exception) { }
                }
                return false;
            }
        }

        // Takes DXUP's DLLs out of the game's folder unless another set-up
        // game uses the same folder, and puts backups back.
        // uninstalling: the DLLs go even when the executable cannot be
        // restored (the game runs); it keeps the bit and its backup, and the
        // result is false with the reason.
        public bool Remove(GameEntry game, IEnumerable<GameEntry> allGames, out string error, bool uninstalling = false)
        {
            error = null;
            string exeError = null;
            // The executable as the game brought it (the bit is this game's own).
            if (!string.IsNullOrEmpty(game.Exe))
            {
                try { LargeAddressAware.Disable(game.ExePath); }
                catch (Exception e)
                {
                    if (!uninstalling)
                    {
                        error = e.Message;
                        return false;
                    }
                    exeError = e.Message + " (" + game.ExePath + LargeAddressAware.BackupSuffix + ")";
                }
            }
            if (!RemoveDlls(game, allGames, out error))
            {
                if (exeError != null)
                    error = exeError + "; " + error;
                return false;
            }
            error = exeError;
            return exeError == null;
        }

        // A name for an older backup of another program's DLL: *.dxup-backup.1, .2, ...
        static string OlderBackupName(string target)
        {
            for (int number = 1; ; number++)
            {
                string name = target + BackupSuffix + "." + number.ToString(CultureInfo.InvariantCulture);
                if (!File.Exists(name))
                    return name;
            }
        }

        bool RemoveDlls(GameEntry game, IEnumerable<GameEntry> allGames, out string error)
        {
            error = null;
            // Nothing to take out of a folder that is gone (or on a drive
            // that is not connected): the game can still be deactivated and
            // removed from the list.
            if (!FolderExists(game))
                return true;
            if (allGames.Any(other => other != game && other.Enabled && SameFolder(other.Dir, game.Dir)))
                return true;
            try
            {
                foreach (string dll in DxupInstall.Dlls.Concat(DxupInstall.LegacyDlls))
                {
                    string target = Path.Combine(game.Dir, dll);
                    if (DxupInstall.IsDxupFile(target))
                        MoveAside(target);
                    string backup = target + BackupSuffix;
                    if (File.Exists(backup) && !File.Exists(target))
                        File.Move(backup, target);
                }
                // What DXUP left in the folder: its pipeline cache and logs.
                try { Directory.Delete(Path.Combine(game.Dir, "dxup_pso_cache"), true); } catch (Exception) { }
                foreach (string log in Directory.GetFiles(game.Dir, "logdxup*.log").Concat(Directory.GetFiles(game.Dir, "dxup_frame_*.bmp"))
                    .Concat(Directory.GetFiles(game.Dir, "dxup_glitch_*.bmp")))
                {
                    try { File.Delete(log); } catch (Exception) { }
                }
                DiagnosticMarkers.ClearAll(game.Dir);
                return true;
            }
            catch (Exception e)
            {
                error = e.Message;
                return false;
            }
        }

        // Deletes a file, or renames it when it is in use (a loaded DLL or a
        // running program can be renamed, not deleted) and deletes it at the
        // next restart. Returns the new name, or null when it is gone.
        public static string MoveAside(string path)
        {
            if (!File.Exists(path))
                return null;
            try
            {
                var attributes = File.GetAttributes(path);
                if ((attributes & FileAttributes.ReadOnly) != 0)
                    File.SetAttributes(path, attributes & ~FileAttributes.ReadOnly);
                File.Delete(path);
                return null;
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            string old = path + OldSuffix + DateTime.Now.Ticks.ToString(CultureInfo.InvariantCulture);
            File.Move(path, old);
            DeleteAtRestart(old);
            return old;
        }

        // Windows deletes the file (or empty folder) when it starts next.
        // Replaced in the tests: it writes to the registry.
        internal static Action<string> DeleteAtRestart = path =>
            NativeMethods.MoveFileEx(path, null, NativeMethods.MOVEFILE_DELAY_UNTIL_REBOOT);

        // Leftovers of DLLs that were in use when DXUP replaced them.
        public static void CleanOldFiles(string dir)
        {
            try
            {
                foreach (string old in Directory.GetFiles(dir, "*" + OldSuffix + "*"))
                {
                    try { File.Delete(old); } catch (Exception) { }
                }
            }
            catch (Exception) { }
        }

        public static bool SameFolder(string a, string b) =>
            string.Equals(Normalize(a), Normalize(b), StringComparison.OrdinalIgnoreCase);

        static string Normalize(string path)
        {
            try { path = Path.GetFullPath(path); } catch (Exception) { }
            return path.Replace('/', '\\').TrimEnd('\\');
        }

        // Whether a program from the game's folder runs (its DLLs are in use).
        public static bool IsRunning(GameEntry game)
        {
            foreach (var process in Process.GetProcesses())
            {
                try
                {
                    IntPtr handle = NativeMethods.OpenProcess(NativeMethods.PROCESS_QUERY_LIMITED_INFORMATION, false, process.Id);
                    if (handle == IntPtr.Zero)
                        continue;
                    var name = new StringBuilder(1024);
                    int size = name.Capacity;
                    bool ok = NativeMethods.QueryFullProcessImageName(handle, 0, name, ref size);
                    NativeMethods.CloseHandle(handle);
                    if (ok && SameFolder(Path.GetDirectoryName(name.ToString()), game.Dir))
                        return true;
                }
                catch (Exception) { }
                finally { process.Dispose(); }
            }
            return false;
        }

        public const string BattleNetAntiCheat = "Battle.net";

        // Folders of anti-cheat software (in the game's folder or up to two
        // levels above it): DXUP's DLLs may get the game flagged. Battle.net
        // games (marked by Battle.net's .build.info) bring their own;
        // BattleNetAntiCheat stands for it.
        public static string AntiCheat(GameEntry game)
        {
            if (!game.ValidPath)
                return null;
            string[] markers = { "EasyAntiCheat", "BattlEye", "EAAntiCheat", "vgc", "PunkBuster" };
            string dir = game.Dir;
            for (int level = 0; level < 3 && !string.IsNullOrEmpty(dir); level++, dir = Path.GetDirectoryName(dir))
            {
                if (string.Equals(Path.GetFileName(dir), "common", StringComparison.OrdinalIgnoreCase))
                    break;
                if (File.Exists(Path.Combine(dir, ".build.info")) && File.Exists(Path.Combine(dir, ".product.db")))
                    return BattleNetAntiCheat;
                foreach (string marker in markers)
                {
                    if (Directory.Exists(Path.Combine(dir, marker)))
                        return marker;
                }
            }
            return null;
        }
    }
}
