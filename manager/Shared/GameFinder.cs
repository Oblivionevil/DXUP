using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace Dxup.Shared
{
    // An installed game found in a launcher's library.
    public sealed class FoundGame
    {
        public string Store = "";        // "Steam", "Battle.net"
        public string Id = "";           // games.cfg's appid: Steam's app id, "battlenet:<uid>"
        public string Name = "";
        public string Dir = "";
        // 64-bit executables of the game, the likeliest one first.
        public List<string> Executables = new List<string>();
    }

    // Autofind: the installed games of Steam and Battle.net.
    public static class GameFinder
    {
        public static List<FoundGame> All()
        {
            var games = new List<FoundGame>();
            games.AddRange(SteamLibrary.Games());
            games.AddRange(BattleNetLibrary.Games());
            return games.OrderBy(game => game.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
        }

        static readonly Regex s_notGame = new Regex(
            @"(crash|unins|setup|redist|vc_?redist|dxsetup|report|helper|updater|cefprocess|webhelper|easyanticheat|battleye|eac|launcherpatcher|dotnet|ue4prereq|uecrash|unitycrashhandler|crashpad|7z|python|benchmark|installer|blizzardbrowser|blizzarderror|clientsdk)",
            RegexOptions.IgnoreCase);

        static readonly string[] s_skipFolders = { "_CommonRedist", "Redist", "redist", "DirectX", "vcredist", "__Installer", "EasyAntiCheat", "BattlEye", "Engine" };

        // x64 and x86 executables up to four folders deep, the likeliest game
        // first: the one the launcher names (preferred), Unreal's
        // *-Shipping.exe, executables that import Direct3D 11 or 10 or run
        // Unity, 64-bit before 32-bit, then the biggest.
        public static List<string> Executables(string dir, string preferred = null)
        {
            var found = new List<(string path, int score, long size)>();
            Collect(dir, 0, found);
            var executables = found.OrderByDescending(item => item.score).ThenByDescending(item => item.size).Select(item => item.path).ToList();
            var preferredPe = preferred != null && File.Exists(preferred) ? PeFile.Read(preferred) : null;
            if (preferredPe != null && (preferredPe.Is64Bit || preferredPe.Is32Bit))
            {
                executables.RemoveAll(path => string.Equals(path, preferred, StringComparison.OrdinalIgnoreCase));
                executables.Insert(0, preferred);
            }
            return executables;
        }

        static void Collect(string dir, int depth, List<(string, int, long)> found)
        {
            try
            {
                foreach (string exe in Directory.GetFiles(dir, "*.exe"))
                {
                    string name = Path.GetFileNameWithoutExtension(exe);
                    if (s_notGame.IsMatch(name))
                        continue;
                    var pe = PeFile.Read(exe);
                    if (pe == null || !(pe.Is64Bit || pe.Is32Bit))
                        continue;
                    int score = 0;
                    if (name.EndsWith("-Shipping", StringComparison.OrdinalIgnoreCase)) score += 4;
                    // Direct3D 11 itself beats DXGI alone; Direct3D 12 builds rank last.
                    if (pe.ImportsD3D11Dll || pe.ImportsD3D10Dll || pe.ImportsLegacyDirect3D || File.Exists(Path.Combine(dir, "UnityPlayer.dll"))) score += 3;
                    else if (pe.ImportsDirect3D11) score += 1;
                    if (pe.ImportsD3D12Dll) score -= 2;
                    if (IsLauncher(name)) score -= 2;
                    if (pe.Is32Bit) score -= 1;
                    found.Add((exe, score, new FileInfo(exe).Length));
                }
                if (depth >= 4)
                    return;
                foreach (string sub in Directory.GetDirectories(dir))
                {
                    if (!s_skipFolders.Contains(Path.GetFileName(sub), StringComparer.OrdinalIgnoreCase))
                        Collect(sub, depth + 1, found);
                }
            }
            catch (Exception) { }
        }

        public static bool IsLauncher(string name) => name.IndexOf("launcher", StringComparison.OrdinalIgnoreCase) >= 0;

        // Whether path is folder or inside it.
        public static bool IsInside(string path, string folder)
        {
            string inner = path.TrimEnd('\\', '/');
            string outer = folder.TrimEnd('\\', '/');
            return inner.Equals(outer, StringComparison.OrdinalIgnoreCase)
                || inner.StartsWith(outer + "\\", StringComparison.OrdinalIgnoreCase);
        }
    }
}
