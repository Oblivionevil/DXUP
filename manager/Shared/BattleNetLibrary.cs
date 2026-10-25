using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace Dxup.Shared
{
    // Installed Battle.net games: Battle.net registers each one in Windows'
    // list of programs (removed by "Blizzard Uninstaller.exe --uid=<game>"),
    // with its folder and the executable it starts.
    public static class BattleNetLibrary
    {
        public const string Store = "Battle.net";
        public const string IdPrefix = "battlenet:";

        public static List<FoundGame> Games()
        {
            var games = new List<FoundGame>();
            // Battle.net's 32-bit agent writes the 32-bit view; read both.
            foreach (var view in new[] { RegistryView.Registry32, RegistryView.Registry64 })
            {
                try
                {
                    using (var root = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view))
                    using (var programs = root.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"))
                    {
                        if (programs == null)
                            continue;
                        foreach (string name in programs.GetSubKeyNames())
                        {
                            // One odd entry (a path with characters paths
                            // cannot hold) does not hide the others.
                            try
                            {
                                using (var key = programs.OpenSubKey(name))
                                {
                                    if (key == null)
                                        continue;
                                    var game = Game(key.GetValue("UninstallString") as string, key.GetValue("DisplayName") as string,
                                                    key.GetValue("InstallLocation") as string, key.GetValue("DisplayIcon") as string);
                                    if (game != null && !games.Any(known => known.Id == game.Id))
                                        games.Add(game);
                                }
                            }
                            catch (Exception) { }
                        }
                    }
                }
                catch (Exception) { }
            }
            return games;
        }

        // The game of one entry of the list of programs; null for other
        // programs, Battle.net itself and games no longer on disk.
        public static FoundGame Game(string uninstall, string name, string dir, string icon)
        {
            string uid = Uid(uninstall);
            if (uid == null || uid.Equals("battle.net", StringComparison.OrdinalIgnoreCase)
             || string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir))
                return null;
            var game = new FoundGame { Store = Store, Id = IdPrefix + uid, Name = name.Trim(), Dir = dir.TrimEnd('\\') };
            // The executable Battle.net starts goes first (Heroes of the Storm:
            // Support64\HeroesSwitcher_x64.exe, which outlives game updates),
            // unless it is a launcher.
            string start = IconPath(icon);
            if (start != null && GameFinder.IsLauncher(Path.GetFileNameWithoutExtension(start)))
                start = null;
            game.Executables = GameFinder.Executables(game.Dir, start);
            return game.Executables.Count != 0 ? game : null;
        }

        // "<agent>\Blizzard Uninstaller.exe" --lang=enUS --uid=heroes --displayname="..."
        public static string Uid(string uninstall)
        {
            if (uninstall == null || uninstall.IndexOf("Blizzard Uninstaller.exe", StringComparison.OrdinalIgnoreCase) < 0)
                return null;
            var match = Regex.Match(uninstall, "--uid=(\"[^\"]*\"|\\S+)");
            string uid = match.Success ? match.Groups[1].Value.Trim('"').Trim() : "";
            return uid.Length != 0 ? uid : null;
        }

        // DisplayIcon: a path, maybe quoted, maybe with ",<icon index>".
        static string IconPath(string icon)
        {
            if (string.IsNullOrWhiteSpace(icon))
                return null;
            string path = Regex.Replace(icon.Trim(), @",\s*-?\d+$", "").Trim().Trim('"');
            return path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? path : null;
        }
    }
}
