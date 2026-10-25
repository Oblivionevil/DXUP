using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using Dxup.Manager;
using Dxup.Shared;

namespace Dxup.Tests
{
    static class Program
    {
        static int s_failed;

        static void Check(bool condition, string what)
        {
            Console.WriteLine((condition ? "[PASS] " : "[FAIL] ") + what);
            if (!condition)
                s_failed++;
        }

        static int Main()
        {
            GamesConfigRoundTrip();
            GamesConfigImport();
            SteamFiles();
            BattleNet();
            PeHeaders();
            LargeAddress();
            Deployer();
            DiagnosticsChecks();
            Console.WriteLine(s_failed == 0 ? "All passed" : s_failed + " failed");
            return s_failed == 0 ? 0 : 1;
        }

        static void GamesConfigRoundTrip()
        {
            var game = new GameEntry
            {
                Name = "Foo [GOTY] #1 Über",
                Dir = @"D:\Games\Foo [GOTY] #1 Über\Binaries\Win64",
                Exe = "Foo-Win64-Shipping.exe",
                AppId = "123",
                Enabled = true,
            };
            game.Settings.Anisotropy = 16;
            game.Settings.LodBias = -0.5f;
            game.Settings.MsaaSamples = 8;
            game.Settings.MsaaForce = true;
            game.Settings.Sgssaa = "alpha";
            game.Settings.MaxFrameLatency = 1;
            game.Settings.AsyncShaders = true;
            game.Settings.ShaderPrecompile = false;
            game.Settings.Upscaler = "xess";
            game.Settings.UpscalerAuto = true;
            game.Settings.UpscalerInject = true;
            game.Settings.SpatialUpscale = true;
            game.Settings.RenderScale = 75;
            game.Settings.Adapter = "intel(r) uhd graphics 770";
            game.Settings.SpoofVendor = "amd";
            game.Settings.SpatialSharpness = 0.5f;
            game.Settings.CustomVendorId = GameSettings.ParseHex(" 0x10DE ");
            game.Settings.CustomDeviceId = "2684";
            game.Settings.CustomDeviceDesc = "NVIDIA GeForce RTX 4090";
            game.Settings.DirectBackBuffer = false;
            game.Settings.FloatEmulation = "strict";
            game.Settings.NanDepth = "zero";
            game.Settings.FxcCompiler = true;
            game.Settings.SubmitThread = true;
            game.Settings.RecordThread = true;
            game.Settings.PixelOutputMasks = false;
            game.Settings.BarrierHoisting = false;
            game.Settings.RelaxedUavBarriers = false;
            game.Settings.LazyTargets = true;
            game.Extra.Add(new KeyValuePair<string, string>("d3d11.future", "x"));
            string text = GamesConfig.Format(new[] { game, new GameEntry { Name = "Bar", Dir = @"C:\Bar", Exe = "bar.exe" } });
            var parsed = GamesConfig.Parse(text);
            Check(parsed.Count == 2, "games.cfg keeps two games");
            var back = parsed[0];
            Check(back.Name == game.Name && back.Dir == game.Dir && back.Exe == game.Exe && back.AppId == "123" && back.Enabled,
                "games.cfg keeps names and paths with [ ] # and umlauts");
            Check(back.Settings.Anisotropy == 16 && back.Settings.LodBias == -0.5f && back.Settings.MsaaSamples == 8
               && back.Settings.MsaaForce && back.Settings.Sgssaa == "alpha" && back.Settings.MaxFrameLatency == 1
               && back.Settings.AsyncShaders && !back.Settings.ShaderPrecompile && back.Settings.Upscaler == "xess"
               && back.Settings.UpscalerAuto && back.Settings.UpscalerInject && back.Settings.SpatialUpscale
               && back.Settings.RenderScale == 75 && back.Settings.Adapter == "intel(r) uhd graphics 770"
               && back.Settings.SpoofVendor == "amd" && !back.Settings.SyncInterval.HasValue && !back.Settings.MsaaScopeAll,
                "games.cfg keeps the settings");
            var more = back.Settings;
            Check(more.SpatialSharpness == 0.5f && more.CustomVendorId == "10de" && more.CustomDeviceId == "2684"
               && more.CustomDeviceDesc == "NVIDIA GeForce RTX 4090" && !more.DirectBackBuffer && more.FloatEmulation == "strict"
               && more.NanDepth == "zero" && more.FxcCompiler && more.SubmitThread && more.RecordThread && !more.PixelOutputMasks
               && !more.BarrierHoisting && more.RelaxedUavBarriers == false && more.LazyTargets == true,
                "games.cfg keeps the advanced and Direct3D 7-9 settings");
            Check(back.Extra.Count == 1 && back.Extra[0].Key == "d3d11.future", "games.cfg keeps unknown settings");
            Check(!parsed[1].Enabled && parsed[1].Settings.Entries().Count() == 0 && parsed[1].Settings.ShaderPrecompile,
                "a game without settings writes none and precompiles");
            Check(text.Contains("d3d11.sgssaa = alpha") && text.Contains("dxgi.maxFrameLatency = 1") && text.Contains("d3d11.asyncShaders = true")
               && text.Contains("d3d11.upscaler = xess") && text.Contains("d3d11.upscalerAuto = true")
               && text.Contains("dxgi.upscale = spatial") && text.Contains("dxgi.renderScale = 0.75")
               && text.Contains("dxgi.spoofVendor = amd") && text.Contains("dxgi.upscaleSharpness = 0.5")
               && text.Contains("dxgi.customVendorId = 10de") && text.Contains("dxgi.customDeviceDesc = NVIDIA GeForce RTX 4090")
               && text.Contains("dxgi.directBackBuffer = false") && text.Contains("d3d9.floatEmulation = strict")
               && text.Contains("d3d9.nanDepth = zero") && text.Contains("d3d9.shaderCompiler = fxc")
               && text.Contains("d3d12.submitThread = true") && text.Contains("d3d12.recordThread = true")
               && text.Contains("d3d12.pixelOutputMasks = false") && text.Contains("d3d12.barrierHoisting = false")
               && text.Contains("d3d11.relaxedUavBarriers = false") && text.Contains("d3d12.lazyTargets = true"),
                "settings use DXUP's keys");
            Check(GameSettings.ParseHex("xyz") == null && GameSettings.ParseHex("0") == null && GameSettings.ParseHex("0x10000") == null
               && GameSettings.ParseHex("0x8086") == "8086" && GameSettings.ParseHex("0x000010DE") == "10de"
               && GameSettings.ParseHex("12345") == "2345" && GameSettings.ParseHex("123456789abcdef01") == "ffff",
                "GPU ids as DXUP reads them (low 16 bits)");
            var lazy = new GameSettings();
            lazy.Set("d3d12.lazyTargets", "yes");
            Check(!lazy.LazyTargets.HasValue, "d3d12.lazyTargets = yes is DXUP's automatic choice");
            var named = new GameSettings();
            named.Set("dxgi.customDeviceDesc", "Radeon RX 7900 #2");
            Check(named.CustomDeviceDesc == "Radeon RX 7900", "a GPU name ends at '#' as DXUP reads it");

            string path = Path.Combine(Path.GetTempPath(), "dxup_test_games.cfg");
            GamesConfig.Save(path, parsed);
            GamesConfig.Save(path, parsed);
            byte[] bytes = File.ReadAllBytes(path);
            Check(bytes.Length > 3 && !(bytes[0] == 0xEF && bytes[1] == 0xBB), "games.cfg has no byte order mark");
            Check(GamesConfig.Load(path).Count == 2, "games.cfg saves and loads");
            File.Delete(path);
        }

        static void GamesConfigImport()
        {
            var game = new GameEntry { Dir = @"C:\X", Exe = "x.exe" };
            GamesConfig.Import(game, "d3d11.msaaSamples = 4  # comment\r\nd3d11.msaaForce = yes\r\ndxgi.syncInterval=0\r\nfoo.bar = 1\r\n");
            Check(game.Settings.MsaaSamples == 4 && game.Settings.MsaaForce && game.Settings.SyncInterval == 0 && game.Extra.Count == 1,
                "dxup.conf imports");
            var legacy = new GameEntry { Dir = @"C:\Y", Exe = "y.exe" };
            GamesConfig.Import(legacy, "d3d9.nanDepth = discard\nd3d9.floatEmulation = off\nd3d12.lazyTargets = auto\nd3d11.relaxedUavBarriers = on\n");
            Check(legacy.Settings.NanDepth == "discard" && legacy.Settings.FloatEmulation == "off" && !legacy.Settings.LazyTargets.HasValue
               && legacy.Settings.RelaxedUavBarriers == true && legacy.Extra.Count == 0, "dxup.conf imports the advanced settings");
        }

        static void SteamFiles()
        {
            string modern = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files (x86)\\\\Steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t\t\"label\"\t\t\"\"\n\t}\n}";
            var paths = SteamLibrary.Pairs(modern).Where(p => p.Key == "path").Select(p => p.Value).ToList();
            Check(paths.Count == 2 && paths[1] == @"D:\SteamLibrary", "libraryfolders.vdf (new format)");
            string old = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1\"\n\t\"1\"\t\t\"D:\\\\Steam\"\n}";
            var pairs = SteamLibrary.Pairs(old).ToList();
            Check(pairs.Any(p => p.Key == "1" && p.Value == @"D:\Steam"), "libraryfolders.vdf (old format)");
            string acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"225540\"\n\t\"name\"\t\t\"Just Cause 3\"\n\t\"installdir\"\t\t\"Just Cause 3\"\n}";
            var app = SteamLibrary.Pairs(acf).ToDictionary(p => p.Key, p => p.Value);
            Check(app["appid"] == "225540" && app["name"] == "Just Cause 3" && app["installdir"] == "Just Cause 3", "appmanifest");
        }

        static void BattleNet()
        {
            const string uninstall = @"""C:\ProgramData\Battle.net\Agent\Blizzard Uninstaller.exe"" --lang=enUS --uid=heroes --displayname=""Heroes of the Storm""";
            Check(BattleNetLibrary.Uid(uninstall) == "heroes", "Battle.net game id from the uninstall command");
            Check(BattleNetLibrary.Uid(@"""C:\Program Files\Foo\unins000.exe"" --uid=foo") == null, "other programs are no Battle.net games");

            // A game folder like Heroes of the Storm's: the executable
            // Battle.net names comes first.
            string root = Path.Combine(Path.GetTempPath(), "dxup_battlenet_test");
            if (Directory.Exists(root))
                Directory.Delete(root, true);
            string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
            string start = Path.Combine(root, "Support64", "Switcher_x64.exe");
            string game = Path.Combine(root, "Versions", "Base1", "Game_x64.exe");
            string launcher = Path.Combine(root, "Game Launcher.exe");
            foreach (string exe in new[] { start, game, launcher })
                Directory.CreateDirectory(Path.GetDirectoryName(exe));
            File.Copy(Path.Combine(system, "notepad.exe"), start);
            File.Copy(Path.Combine(system, "dxdiag.exe"), game);
            File.Copy(Path.Combine(system, "notepad.exe"), launcher);
            var found = BattleNetLibrary.Game(uninstall, "Heroes of the Storm", root + "\\", "\"" + start + "\",0");
            Check(found != null && found.Id == "battlenet:heroes" && found.Store == "Battle.net" && found.Dir == root
                  && found.Executables.Count == 3 && found.Executables[0] == start,
                "Battle.net game, the executable Battle.net starts first");
            found = BattleNetLibrary.Game(uninstall, "Heroes of the Storm", root, launcher);
            Check(found != null && found.Executables.Count == 3 && found.Executables[2] == launcher, "a launcher Battle.net starts is not preferred");
            Check(BattleNetLibrary.Game(uninstall.Replace("--uid=heroes", "--uid=battle.net"), "Battle.net", root, start) == null,
                "Battle.net itself is no game");
            Check(BattleNetLibrary.Game(uninstall, "Gone", Path.Combine(root, "missing"), null) == null, "uninstalled leftovers are skipped");
            Check(GameFinder.IsInside(root + @"\Support64", root) && !GameFinder.IsInside(root + @" PTR\Support64", root),
                "a game folder is not a prefix of another");
            Directory.Delete(root, true);
        }

        static void PeHeaders()
        {
            string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
            var notepad64 = PeFile.Read(Path.Combine(system, "notepad.exe"));
            Check(notepad64 != null && notepad64.Is64Bit, "64-bit executable");
            string wow = Environment.GetFolderPath(Environment.SpecialFolder.SystemX86);
            var x86 = PeFile.Read(Path.Combine(wow, "cmd.exe"));
            Check(x86 != null && x86.Machine == PeFile.MachineX86, "32-bit executable");
            var dxdiag = PeFile.Read(Path.Combine(system, "dxdiag.exe"));
            Check(dxdiag != null && dxdiag.Imports.Count > 3, "imports are read");
            Check(PeFile.Read(Path.Combine(system, "drivers", "etc", "hosts")) == null, "a text file is no executable");

            // DXUP builds from before the version resource: by their log marker.
            string dir = Path.Combine(Path.GetTempPath(), "dxup_manager_test_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            try
            {
                string old = Path.Combine(dir, "d3d11.dll");
                File.WriteAllBytes(old, System.Text.Encoding.ASCII.GetBytes("MZ....dxup_info_log.txt...."));
                Check(DxupInstall.IsDxupFile(old), "an old DXUP build is recognised by its log marker");
                Check(!DxupInstall.IsDxupFile(Path.Combine(system, "d3d11.dll")), "Windows' d3d11.dll is not DXUP's");
            }
            finally
            {
                Directory.Delete(dir, true);
            }
        }

        [System.Runtime.InteropServices.DllImport("imagehlp.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
        static extern uint MapFileAndCheckSumW(string file, out uint headerSum, out uint checkSum);

        // The large address aware bit: set with the checksum Windows computes,
        // cleared to the original bytes; a game's own bit stays.
        static void LargeAddress()
        {
            string dir = Path.Combine(Path.GetTempPath(), "dxup_laa_test_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            try
            {
                string wow = Environment.GetFolderPath(Environment.SpecialFolder.SystemX86);
                string original = Path.Combine(wow, "cmd.exe");
                string exe = Path.Combine(dir, "game.exe");
                File.Copy(original, exe);
                Check(LargeAddressAware.Get(exe) == false && !LargeAddressAware.SetByDxup(exe), "a 32-bit executable without the bit");
                Check(LargeAddressAware.Get(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "notepad.exe")) == null,
                    "64-bit executables have no choice");
                LargeAddressAware.Enable(exe);
                Check(LargeAddressAware.Get(exe) == true && LargeAddressAware.SetByDxup(exe)
                    && LargeAddressAware.SameContent(exe + LargeAddressAware.BackupSuffix, original), "the bit is set, the original kept");
                Check(MapFileAndCheckSumW(exe, out uint headerSum, out uint checkSum) == 0 && headerSum == checkSum && checkSum != 0,
                    "the header checksum is Windows' sum");
                LargeAddressAware.Enable(exe);
                Check(LargeAddressAware.SameContent(exe + LargeAddressAware.BackupSuffix, original), "setting it again keeps the original");
                LargeAddressAware.Disable(exe);
                Check(LargeAddressAware.Get(exe) == false && !LargeAddressAware.SetByDxup(exe) && LargeAddressAware.SameContent(exe, original),
                    "clearing it gives the original back");

                // A checksum that was wrong (signed after linking, as Just Cause 2's)
                // stays as it was; clearing in place (an update replaced the
                // executable) gives the original bytes as well.
                byte[] stale = File.ReadAllBytes(original);
                int peStale = BitConverter.ToInt32(stale, 0x3c);
                BitConverter.GetBytes(0x12345678u).CopyTo(stale, peStale + 24 + 64);
                string staleExe = Path.Combine(dir, "stale.exe");
                string staleOriginal = Path.Combine(dir, "stale_original.bin");
                File.WriteAllBytes(staleExe, stale);
                File.WriteAllBytes(staleOriginal, stale);
                LargeAddressAware.Enable(staleExe);
                bool kept = BitConverter.ToUInt32(File.ReadAllBytes(staleExe), peStale + 24 + 64) == 0x12345678u;
                LargeAddressAware.Disable(staleExe);
                bool exact = LargeAddressAware.SameContent(staleExe, staleOriginal);
                Check(kept && exact, "a wrong checksum stays; the original comes back exactly");
                // The backup is of an older version: an update brought an
                // executable with the bit of its own, which stays.
                LargeAddressAware.Enable(staleExe);
                byte[] updated = File.ReadAllBytes(staleExe).Concat(new byte[] { 1 }).ToArray();
                File.WriteAllBytes(staleExe, updated);
                LargeAddressAware.Disable(staleExe);
                Check(LargeAddressAware.Get(staleExe) == true && !LargeAddressAware.SetByDxup(staleExe)
                    && File.ReadAllBytes(staleExe).SequenceEqual(updated), "an update's own bit stays");

                // The game has the bit itself: no backup, and it stays.
                LargeAddressAware.Enable(exe);
                File.Delete(exe + LargeAddressAware.BackupSuffix);
                LargeAddressAware.Enable(exe);
                LargeAddressAware.Disable(exe);
                Check(LargeAddressAware.Get(exe) == true && !LargeAddressAware.SetByDxup(exe), "a game's own bit stays");

                // games.cfg keeps the choice; the deployer sets and clears the bit.
                var game = new GameEntry { Name = "Game", Dir = dir, Exe = "laa.exe", Enabled = true, LargeAddressAware = true };
                var parsed = GamesConfig.Parse(GamesConfig.Format(new[] { game }));
                Check(parsed.Count == 1 && parsed[0].LargeAddressAware, "games.cfg keeps largeAddressAware");
                File.Copy(original, game.ExePath);
                var deployer = new GameDeployer(new DxupInstall(Path.Combine(dir, "install")));
                Check(deployer.Install(game, out string error) && LargeAddressAware.Get(game.ExePath) == true,
                    "activating sets the bit " + error);
                Check(deployer.Remove(game, new[] { game }, out error) && LargeAddressAware.SameContent(game.ExePath, original)
                    && !LargeAddressAware.SetByDxup(game.ExePath), "removing DXUP restores the executable " + error);
                // The executable cannot change (the game runs): nothing does.
                foreach (string bin in new[] { "bin", "bin32" })
                {
                    Directory.CreateDirectory(Path.Combine(dir, "install", bin));
                    // Recognised as DXUP's by the marker name it holds.
                    File.WriteAllBytes(Path.Combine(dir, "install", bin, "d3d11.dll"), System.Text.Encoding.ASCII.GetBytes("dxup_info_log.txt"));
                }
                using (new FileStream(game.ExePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                    Check(!deployer.Install(game, out error) && !File.Exists(Path.Combine(dir, "d3d11.dll"))
                        && !LargeAddressAware.SetByDxup(game.ExePath), "a locked executable: nothing is installed");
                Check(deployer.Install(game, out error) && File.Exists(Path.Combine(dir, "d3d11.dll"))
                    && LargeAddressAware.Get(game.ExePath) == true, "then it installs " + error);
                // Removing while the game runs: refused; uninstalling takes the
                // DLLs and leaves the executable with its backup.
                using (new FileStream(game.ExePath, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    Check(!deployer.Remove(game, new[] { game }, out error) && File.Exists(Path.Combine(dir, "d3d11.dll")),
                        "a locked executable: DXUP stays");
                    Check(!deployer.Remove(game, new[] { game }, out error, uninstalling: true) && error != null
                        && !File.Exists(Path.Combine(dir, "d3d11.dll")) && LargeAddressAware.SetByDxup(game.ExePath),
                        "uninstalling with a locked executable: the DLLs go");
                }
                deployer.Remove(game, new[] { game }, out error);

                // Steam's DRM wrapper (a .bind section): the executable stays as it is.
                byte[] bytes = File.ReadAllBytes(original);
                int pe = BitConverter.ToInt32(bytes, 0x3c);
                int sections = pe + 24 + BitConverter.ToUInt16(bytes, pe + 20);
                int count = BitConverter.ToUInt16(bytes, pe + 6);
                System.Text.Encoding.ASCII.GetBytes(".bind\0\0\0").CopyTo(bytes, sections + 40 * (count - 1));
                string drm = Path.Combine(dir, "drm.exe");
                File.WriteAllBytes(drm, bytes);
                bool refused = false;
                try { LargeAddressAware.Enable(drm); } catch (InvalidOperationException) { refused = true; }
                var drmGame = new GameEntry { Name = "Drm", Dir = dir, Exe = "drm.exe", Enabled = true, LargeAddressAware = true };
                Check(LargeAddressAware.SteamDrm(drm) && refused && !LargeAddressAware.Wanted(drmGame) && deployer.Install(drmGame, out error)
                    && LargeAddressAware.Get(drm) == false && !LargeAddressAware.SetByDxup(drm), "Steam DRM: the executable stays " + error);
            }
            finally
            {
                Directory.Delete(dir, true);
            }
        }

        static void Deployer()
        {
            string root = Path.Combine(Path.GetTempPath(), "dxup_deployer_test");
            if (Directory.Exists(root))
                Directory.Delete(root, true);
            string bin = Path.Combine(root, "install", "bin");
            string gameDir = Path.Combine(root, "Game [GOTY] #1");
            Directory.CreateDirectory(bin);
            Directory.CreateDirectory(gameDir);
            // DXUP's DLLs from the build, or skip.
            string build = Path.GetFullPath(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, @"..\..\..\..\..\build_rel\src\d3d11"));
            if (!File.Exists(Path.Combine(build, "d3d11.dll")))
            {
                Console.WriteLine("[SKIP] deployer (no build_rel)");
                return;
            }
            foreach (string dll in DxupInstall.Dlls)
                File.Copy(Path.Combine(build, dll), Path.Combine(bin, dll));
            File.WriteAllText(Path.Combine(gameDir, "d3d11.dll"), "foreign");
            var install = new DxupInstall(Path.Combine(root, "install"));
            var deployer = new GameDeployer(install);
            var game = new GameEntry { Name = "Game", Dir = gameDir, Exe = "game.exe", Enabled = true };
            Check(deployer.State(game) == GameState.Missing, "state before install");
            Check(deployer.Install(game, out string error), "install " + error);
            Check(deployer.State(game) == GameState.Active, "state after install");
            Check(File.ReadAllText(Path.Combine(gameDir, "d3d11.dll" + GameDeployer.BackupSuffix)) == "foreign", "a foreign DLL is backed up");
            var other = new GameEntry { Name = "Other", Dir = gameDir, Exe = "other.exe", Enabled = true };
            Check(deployer.Remove(game, new[] { game, other }, out error) && deployer.State(other) == GameState.Active,
                "a shared folder keeps DXUP while another game uses it");
            DiagnosticMarkers.Set(gameDir, DiagnosticMarkers.Log, true);
            Check(deployer.Remove(game, new[] { game }, out error), "remove " + error);
            Check(!DiagnosticMarkers.AnyOn(gameDir), "remove turns the diagnostics off");
            Check(File.ReadAllText(Path.Combine(gameDir, "d3d11.dll")) == "foreign" && !File.Exists(Path.Combine(gameDir, "dxgi.dll")),
                "remove restores the foreign DLL");

            // A 32-bit game gets the DLLs of bin32.
            string build32 = Path.GetFullPath(Path.Combine(build, @"..\..\..\build_rel32\src\d3d11"));
            if (File.Exists(Path.Combine(build32, "d3d11.dll")))
            {
                string bin32 = Path.Combine(root, "install", "bin32");
                string game32Dir = Path.Combine(root, "Game32");
                Directory.CreateDirectory(bin32);
                Directory.CreateDirectory(game32Dir);
                foreach (string dll in DxupInstall.Dlls)
                    File.Copy(Path.Combine(build32, dll), Path.Combine(bin32, dll));
                string wow = Environment.GetFolderPath(Environment.SpecialFolder.SystemX86);
                File.Copy(Path.Combine(wow, "cmd.exe"), Path.Combine(game32Dir, "game32.exe"));
                var game32 = new GameEntry { Name = "Game32", Dir = game32Dir, Exe = "game32.exe", Enabled = true };
                Check(deployer.SourceDir(game32) == install.BinDir32 && deployer.Install(game32, out error)
                    && deployer.State(game32) == GameState.Active
                    && PeFile.Read(Path.Combine(game32Dir, "d3d11.dll"))?.Is32Bit == true,
                    "a 32-bit game gets the 32-bit DLLs " + error);
                Check(!GameDeployer.DllsFor(game32).Contains("ddraw.dll"), "a Direct3D 11 game gets no legacy DLLs");

                // A Direct3D 7 game (it imports ddraw.dll) gets DXUP's ddraw.dll too.
                string ddraw32 = Path.GetFullPath(Path.Combine(build32, @"..\ddraw\ddraw.dll"));
                string dx7Test = Path.GetFullPath(Path.Combine(build32, @"..\tests\dx7_test_app.exe"));
                if (File.Exists(ddraw32) && File.Exists(dx7Test))
                {
                    File.Copy(ddraw32, Path.Combine(bin32, "ddraw.dll"));
                    string dx7Dir = Path.Combine(root, "Game7");
                    Directory.CreateDirectory(dx7Dir);
                    File.Copy(dx7Test, Path.Combine(dx7Dir, "game7.exe"));
                    File.WriteAllText(Path.Combine(dx7Dir, "ddraw.dll"), "foreign");
                    var game7 = new GameEntry { Name = "Game7", Dir = dx7Dir, Exe = "game7.exe", Enabled = true };
                    Check(GameDeployer.DllsFor(game7).Contains("ddraw.dll") && deployer.Install(game7, out error)
                        && deployer.State(game7) == GameState.Active && DxupInstall.IsDxupFile(Path.Combine(dx7Dir, "ddraw.dll")),
                        "a Direct3D 7 game gets ddraw.dll " + error);
                    Check(deployer.Remove(game7, new[] { game7 }, out error)
                        && File.ReadAllText(Path.Combine(dx7Dir, "ddraw.dll")) == "foreign",
                        "removing restores the game's own ddraw.dll " + error);

                    // An engine DLL next to the executable imports ddraw.dll
                    // (Risen's Engine.dll imports d3d9.dll).
                    File.Copy(dx7Test, Path.Combine(game32Dir, "engine.dll"));
                    Check(GameDeployer.DllsFor(game32).Contains("ddraw.dll"), "a game whose engine DLL imports ddraw.dll gets it");
                    File.Delete(Path.Combine(game32Dir, "engine.dll"));

                    // DXUP's legacy DLL already in the folder is kept up to date.
                    File.Copy(ddraw32, Path.Combine(game32Dir, "ddraw.dll"));
                    Check(GameDeployer.DllsFor(game32).Contains("ddraw.dll"), "DXUP's ddraw.dll in the folder stays up to date");
                    File.Delete(Path.Combine(game32Dir, "ddraw.dll"));
                }
            }
            else
            {
                Console.WriteLine("[SKIP] 32-bit deployer (no build_rel32)");
            }
            Directory.Delete(root, true);
        }

        static void DiagnosticsChecks()
        {
            // Names of the user and the computer never leave in a package.
            string profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            string text = Diagnostics.Anonymize(profile + @"\Documents\a.txt; C:\Users\Someone Else\x.log; C:\Users\Public\y; d:/users/bob/z; "
                + Environment.UserName + " on " + Environment.MachineName);
            Check(text.Contains(@"%USERPROFILE%\Documents\a.txt") && text.Contains(@"C:\Users\<user>\x.log") && text.Contains(@"C:\Users\Public\y")
               && text.Contains("d:/users/<user>/z") && text.IndexOf(Environment.UserName, StringComparison.OrdinalIgnoreCase) < 0
               && text.IndexOf(Environment.MachineName, StringComparison.OrdinalIgnoreCase) < 0, "anonymized paths and names: " + text);

            string dir = Path.Combine(Path.GetTempPath(), "dxup_diag_test");
            if (Directory.Exists(dir))
                Directory.Delete(dir, true);
            Directory.CreateDirectory(dir);
            DiagnosticMarkers.Set(dir, DiagnosticMarkers.DebugLayer, true);
            DiagnosticMarkers.Set(dir, DiagnosticMarkers.FrameDump, true);
            Check(DiagnosticMarkers.IsOn(dir, DiagnosticMarkers.DebugLayer) && File.Exists(Path.Combine(dir, "dxup_debug.txt"))
               && DiagnosticMarkers.AnyOn(dir) && !DiagnosticMarkers.IsOn(dir, DiagnosticMarkers.Log), "diagnostic switches are files next to the DLLs");

            // A package with the log (anonymized), a frame as PNG, settings and system information.
            File.WriteAllText(Path.Combine(dir, "logdxup.log"), "[INFO] loaded from " + profile + @"\Games\x\d3d11.dll" + "\r\n");
            using (var bitmap = new System.Drawing.Bitmap(8, 8))
                bitmap.Save(Path.Combine(dir, "dxup_frame_60.bmp"), System.Drawing.Imaging.ImageFormat.Bmp);
            var game = new GameEntry { Name = "Test Game", Dir = dir, Exe = "test.exe", Enabled = true };
            game.Settings.MsaaSamples = 4;
            string zipPath = Path.Combine(Path.GetTempPath(), "dxup_diag_test.zip");
            string error = Diagnostics.CreatePackage(game, GamesConfig.Format(new[] { game }), "1.2.3", zipPath);
            Check(error == null, "diagnostic package " + error);
            if (error == null)
            {
                using (var zip = ZipFile.OpenRead(zipPath))
                {
                    var names = zip.Entries.Select(entry => entry.FullName).ToList();
                    string Read(string name)
                    {
                        using (var reader = new StreamReader(zip.GetEntry(name).Open()))
                            return reader.ReadToEnd();
                    }
                    Check(names.Contains("README.txt") && names.Contains("system.txt") && names.Contains("settings.txt") && names.Contains("crashes.txt")
                       && names.Contains("logs/logdxup.log") && names.Contains("frames/dxup_frame_60.png"), "package contents: " + string.Join(", ", names));
                    string log = names.Contains("logs/logdxup.log") ? Read("logs/logdxup.log") : "";
                    Check(log.Contains(@"%USERPROFILE%\Games\x\d3d11.dll"), "the log in the package is anonymized: " + log.Trim());
                    string system = Read("system.txt");
                    Check(system.Contains("DXUP 1.2.3") && system.Contains("GPU: ") && system.Contains("Diagnostics on: dxup_debug.txt, dxup_frame_dump.txt"),
                        "system.txt names DXUP, the GPU and the switches");
                    Check(Read("settings.txt").Contains("d3d11.msaaSamples = 4"), "settings.txt holds the game's settings");
                }
                File.Delete(zipPath);
            }
            DiagnosticMarkers.ClearAll(dir);
            Check(!DiagnosticMarkers.AnyOn(dir), "all switches off");
            Directory.Delete(dir, true);
        }
    }
}
