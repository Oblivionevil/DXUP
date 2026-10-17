using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Dxup.Shared
{
    // Installed Steam games: Steam's folder from the registry, its libraries
    // from libraryfolders.vdf, the games from appmanifest_*.acf.
    public static class SteamLibrary
    {
        public const string Store = "Steam";

        public static string SteamDir()
        {
            try
            {
                using (var key = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam"))
                {
                    if (key?.GetValue("SteamPath") is string path && Directory.Exists(path))
                        return Path.GetFullPath(path.Replace('/', '\\'));
                }
                // The elevated manager may run as another account than the user.
                using (var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32))
                using (var key = root.OpenSubKey(@"SOFTWARE\Valve\Steam"))
                {
                    if (key?.GetValue("InstallPath") is string path && Directory.Exists(path))
                        return path;
                }
            }
            catch (Exception) { }
            return null;
        }

        // "key" "value" pairs of a Valve KeyValues text, in order.
        public static IEnumerable<KeyValuePair<string, string>> Pairs(string text)
        {
            foreach (Match match in Regex.Matches(text, "\"((?:[^\"\\\\]|\\\\.)*)\"\\s+\"((?:[^\"\\\\]|\\\\.)*)\""))
                yield return new KeyValuePair<string, string>(Unescape(match.Groups[1].Value), Unescape(match.Groups[2].Value));
        }

        static string Unescape(string value) => value.Replace("\\\\", "\\").Replace("\\\"", "\"");

        public static List<string> Libraries(string steamDir)
        {
            var libraries = new List<string> { steamDir };
            string vdf = Path.Combine(steamDir, "steamapps", "libraryfolders.vdf");
            if (File.Exists(vdf))
            {
                foreach (var pair in Pairs(File.ReadAllText(vdf)))
                {
                    // New format: "path" "D:\\Steam"; old format: "1" "D:\\Steam".
                    bool path = pair.Key.Equals("path", StringComparison.OrdinalIgnoreCase)
                             || (pair.Key.All(char.IsDigit) && pair.Value.Contains(":"));
                    if (path && Directory.Exists(pair.Value)
                     && !libraries.Any(known => GameDeployer.SameFolder(known, pair.Value)))
                        libraries.Add(pair.Value);
                }
            }
            return libraries;
        }

        public static List<FoundGame> Games()
        {
            var games = new List<FoundGame>();
            string steamDir = SteamDir();
            if (steamDir == null)
                return games;
            foreach (string library in Libraries(steamDir))
            {
                string apps = Path.Combine(library, "steamapps");
                if (!Directory.Exists(apps))
                    continue;
                foreach (string manifest in Directory.GetFiles(apps, "appmanifest_*.acf"))
                {
                    try
                    {
                        var game = new FoundGame { Store = Store };
                        string installDir = null;
                        foreach (var pair in Pairs(File.ReadAllText(manifest)))
                        {
                            if (pair.Key == "appid" && game.Id.Length == 0) game.Id = pair.Value;
                            else if (pair.Key == "name" && game.Name.Length == 0) game.Name = pair.Value;
                            else if (pair.Key == "installdir" && installDir == null) installDir = pair.Value;
                        }
                        if (installDir == null)
                            continue;
                        game.Dir = Path.Combine(apps, "common", installDir);
                        if (!Directory.Exists(game.Dir) || IsTool(game.Name))
                            continue;
                        game.Executables = GameFinder.Executables(game.Dir);
                        if (game.Executables.Count != 0)
                            games.Add(game);
                    }
                    catch (Exception) { }
                }
            }
            return games;
        }

        static bool IsTool(string name) =>
            Regex.IsMatch(name, @"^(Steamworks Common Redistributables|Proton|Steam Linux Runtime|SteamVR)", RegexOptions.IgnoreCase);
    }
}
