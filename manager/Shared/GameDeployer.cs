using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;

namespace Dxup.Shared
{
    public enum GameState
    {
        Inactive,      // DXUP is not in the game's folder
        Active,        // DXUP's current DLLs are in the folder
        Outdated,      // DXUP's DLLs in the folder differ from the installed ones
        Missing,       // set up, but DXUP's DLLs are gone (e.g. the launcher repaired the game)
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
            if (!game.Enabled)
                return GameState.Inactive;
            if (!any)
                return GameState.Missing;
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
                // LoadLibrary("d3d9.dll"): the name is in the file.
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
        // case: one pass over the file in large blocks, without decoding it.
        static IEnumerable<string> NamesIn(string path, IList<string> names)
        {
            var found = new List<string>();
            if (names.Count == 0)
                return found;
            var patterns = names.Select(name => Encoding.ASCII.GetBytes(name.ToLowerInvariant())).ToArray();
            var done = new bool[patterns.Length];
            int longest = patterns.Max(pattern => pattern.Length);
            try
            {
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete,
                    1 << 16, FileOptions.SequentialScan))
                {
                    var buffer = new byte[(4 << 20) + longest];
                    int kept = 0;
                    long remaining = stream.Length;
                    while (remaining > 0 && found.Count < patterns.Length)
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
                            // Bit 0x20 folds ASCII letters to lower case; the names'
                            // digits and dots already have it.
                            int first = buffer[i] | 0x20;
                            for (int p = 0; p < patterns.Length; p++)
                            {
                                var pattern = patterns[p];
                                if (done[p] || first != pattern[0] || i + pattern.Length > end)
                                    continue;
                                int j = 1;
                                while (j < pattern.Length && (buffer[i + j] | 0x20) == pattern[j])
                                    j++;
                                if (j == pattern.Length)
                                {
                                    done[p] = true;
                                    found.Add(names[p]);
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
            string exe = string.IsNullOrEmpty(game.Exe) ? null : Path.Combine(game.Dir, game.Exe);
            var pe = exe != null && File.Exists(exe) ? PeFile.Read(exe) : null;
            return pe != null && pe.Is32Bit ? m_install.BinDir32 : m_install.BinDir;
        }

        // A dxup.conf in the game's folder overrides games.cfg.
        public static bool HasLocalConfig(GameEntry game) => File.Exists(Path.Combine(game.Dir, "dxup.conf"));

        // Copies DXUP's DLLs into the game's folder. Returns false and an
        // error text when that failed.
        public bool Install(GameEntry game, out string error)
        {
            error = null;
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
                            // Another program's DLL: keep it, never overwrite an older backup.
                            string backup = target + BackupSuffix;
                            if (File.Exists(backup))
                                MoveAside(target);
                            else
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
                return false;
            }
        }

        // Takes DXUP's DLLs out of the game's folder unless another set-up
        // game uses the same folder, and puts backups back.
        public bool Remove(GameEntry game, IEnumerable<GameEntry> allGames, out string error)
        {
            error = null;
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

        // Deletes a file, or renames it when it is in use and deletes it at
        // the next restart.
        static void MoveAside(string path)
        {
            try
            {
                File.Delete(path);
                return;
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
            string old = path + OldSuffix + DateTime.Now.Ticks.ToString();
            File.Move(path, old);
            NativeMethods.MoveFileEx(old, null, NativeMethods.MOVEFILE_DELAY_UNTIL_REBOOT);
        }

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
