using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using Dxup.Manager;
using Dxup.Setup;
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

        // Files the code under test would have Windows delete at the next
        // restart: recorded instead, as that writes the registry.
        static readonly List<string> s_restartDeletes = new List<string>();

        static int Main()
        {
            GameDeployer.DeleteAtRestart = path => s_restartDeletes.Add(path);
            GamesConfigRoundTrip();
            GamesConfigKeepsOtherLines();
            GamesConfigImport();
            SteamFiles();
            BattleNet();
            PeHeaders();
            LargeAddress();
            LargeAddressReadOnly();
            Deployer();
            DeployerFolders();
            ForeignBackups();
            NamesInFile();
            SettingOptions();
            InstallFolders();
            UninstallFiles();
            ReplaceInUse();
            DiagnosticsChecks();
            CrashReports();
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
            game.Settings.LowLatency = true;
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
            game.Settings.D3d9WorkerThread = true;
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
               && back.Settings.LowLatency
               && back.Settings.AsyncShaders && !back.Settings.ShaderPrecompile && back.Settings.Upscaler == "xess"
               && back.Settings.UpscalerAuto && back.Settings.UpscalerInject && back.Settings.SpatialUpscale
               && back.Settings.RenderScale == 75 && back.Settings.Adapter == "intel(r) uhd graphics 770"
               && back.Settings.SpoofVendor == "amd" && !back.Settings.SyncInterval.HasValue && !back.Settings.MsaaScopeAll,
                "games.cfg keeps the settings");
            var more = back.Settings;
            Check(more.SpatialSharpness == 0.5f && more.CustomVendorId == "10de" && more.CustomDeviceId == "2684"
               && more.CustomDeviceDesc == "NVIDIA GeForce RTX 4090" && !more.DirectBackBuffer && more.FloatEmulation == "strict"
               && more.NanDepth == "zero" && more.FxcCompiler && more.D3d9WorkerThread && more.SubmitThread && more.RecordThread && !more.PixelOutputMasks
               && !more.BarrierHoisting && more.RelaxedUavBarriers == false && more.LazyTargets == true,
                "games.cfg keeps the advanced and Direct3D 7-9 settings");
            Check(back.Extra.Count == 1 && back.Extra[0].Key == "d3d11.future", "games.cfg keeps unknown settings");
            Check(!parsed[1].Enabled && parsed[1].Settings.Entries().Count() == 0 && parsed[1].Settings.ShaderPrecompile,
                "a game without settings writes none and precompiles");
            Check(text.Contains("d3d11.sgssaa = alpha") && text.Contains("dxgi.maxFrameLatency = 1") && text.Contains("d3d11.asyncShaders = true")
               && text.Contains("dxgi.lowLatency = true")
               && text.Contains("d3d11.upscaler = xess") && text.Contains("d3d11.upscalerAuto = true")
               && text.Contains("dxgi.upscale = spatial") && text.Contains("dxgi.renderScale = 0.75")
               && text.Contains("dxgi.spoofVendor = amd") && text.Contains("dxgi.upscaleSharpness = 0.5")
               && text.Contains("dxgi.customVendorId = 10de") && text.Contains("dxgi.customDeviceDesc = NVIDIA GeForce RTX 4090")
               && text.Contains("dxgi.directBackBuffer = false") && text.Contains("d3d9.floatEmulation = strict")
               && text.Contains("d3d9.nanDepth = zero") && text.Contains("d3d9.shaderCompiler = fxc")
               && text.Contains("d3d9.workerThread = true")
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
            var commented = GamesConfig.Parse("[game]\ndir = C:\\Games\\#1\nname = Foo #1\nenabled = true # on\n"
                + "d3d11.samplerAnisotropy = 16 # max\ndxgi.customVendorId = 10de # nvidia\n"
                + "dxgi.customDeviceDesc = Radeon RX 7900 #2\nd3d11.future = x # kept\n");
            Check(commented.Count == 1 && commented[0].Dir == @"C:\Games\#1" && commented[0].Name == "Foo #1" && commented[0].Enabled
               && commented[0].Settings.Anisotropy == 16 && commented[0].Settings.CustomVendorId == "10de"
               && commented[0].Settings.CustomDeviceDesc == "Radeon RX 7900" && commented[0].Extra.Count == 1
               && commented[0].Extra[0].Value == "x # kept", "games.cfg settings end at '#' as DXUP reads them");

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

        static string TempDir(string name)
        {
            string dir = Path.Combine(Path.GetTempPath(), name + "_" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(dir);
            return dir;
        }

        // Deletes a test folder, read-only files too.
        static void DeleteTree(string dir)
        {
            if (!Directory.Exists(dir))
                return;
            foreach (string file in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
                File.SetAttributes(file, FileAttributes.Normal);
            Directory.Delete(dir, true);
        }

        // Comments, other sections, lines without '=' and [game] blocks
        // without a dir survive a save, and a second save changes nothing.
        static void GamesConfigKeepsOtherLines()
        {
            string hand = "# my games\n\n[game]\n# Trine 4, patched\nname = Trine\ndir = C:\\Games\\Trine\nexe = trine.exe\n"
                + "a line without equals\nd3d11.msaaSamples = 16\n\n[othertool]\nkey = value\n# tool comment\n\n"
                + "[game]\nname = No dir yet\nd3d11.msaaSamples = 4\n";
            var document = GamesConfig.ParseDocument(hand);
            Check(document.Games.Count == 1 && document.Games[0].Settings.MsaaSamples == 16, "games.cfg with other lines: one game");
            string saved = GamesConfig.Format(document.Games, document);
            Check(saved.Contains("# my games\n") && saved.Contains("# Trine 4, patched\n") && saved.Contains("a line without equals\n")
               && saved.Contains("[othertool]\nkey = value\n# tool comment\n") && saved.Contains("[game]\nname = No dir yet\nd3d11.msaaSamples = 4\n"),
                "games.cfg keeps comments, other sections and blocks without dir: " + saved);
            var again = GamesConfig.ParseDocument(saved);
            Check(GamesConfig.Format(again.Games, again) == saved && again.Games.Count == 1 && again.Games[0].Lines.Count == 2,
                "games.cfg with other lines saves the same twice");
            Check(saved.Split('\n').Count(line => line.StartsWith("# DXUP settings per game")) == 1, "the manager's header is written once");
            // The game's block still ends where DXUP's DLLs see it end.
            int game = saved.IndexOf("dir = C:\\Games\\Trine", StringComparison.Ordinal);
            int other = saved.IndexOf("[othertool]", StringComparison.Ordinal);
            Check(game >= 0 && other > game && saved.IndexOf("d3d11.msaaSamples = 16", StringComparison.Ordinal) < other,
                "other sections come after the games");
            string path = Path.Combine(Path.GetTempPath(), "dxup_test_keep_" + Guid.NewGuid().ToString("N") + ".cfg");
            File.WriteAllText(path, hand);
            var loaded = GamesConfig.LoadDocument(path);
            loaded.Games[0].Settings.MsaaSamples = 8;
            GamesConfig.Save(path, loaded.Games, loaded);
            string written = File.ReadAllText(path);
            Check(written.Contains("[othertool]") && written.Contains("# my games") && written.Contains("d3d11.msaaSamples = 8"),
                "saving a changed setting keeps the other lines");
            File.Delete(path);

            // Values beyond the manager's lists stay as written.
            var odd = GamesConfig.Parse("[game]\ndir = C:\\X\nd3d11.msaaSamples = 16\ndxgi.maxFrameLatency = 8\ndxgi.syncInterval = 2\n"
                + "d3d11.samplerAnisotropy = 1\ndxgi.renderScale = 0.875\nd3d11.samplerLodBias = -0.333\n");
            string oddText = GamesConfig.Format(odd);
            Check(oddText.Contains("d3d11.msaaSamples = 16") && oddText.Contains("dxgi.maxFrameLatency = 8") && oddText.Contains("dxgi.syncInterval = 2")
               && oddText.Contains("d3d11.samplerAnisotropy = 1") && oddText.Contains("dxgi.renderScale = 0.875")
               && oddText.Contains("d3d11.samplerLodBias = -0.333"), "values beyond the lists are written as read: " + oddText);
            var scale = GamesConfig.Parse("[game]\ndir = C:\\X\ndxgi.renderScale = 0.8\n")[0].Settings;
            Check(scale.RenderScale == 80.0, "render scale 0.8 is 80 %");

            // Characters paths cannot hold: the entry stays, flagged.
            var bad = GamesConfig.Parse("[game]\ndir = C:\\Games\\\"Bad|<Name>\nexe = game.exe\nenabled = true\n");
            Check(bad.Count == 1 && !bad[0].ValidPath && GamesConfig.Format(bad).Contains("Bad|<Name>"),
                "a dir with \" | < > is kept and flagged");
        }

        static void SettingOptions()
        {
            var presets = new[] { new Option("game", null), new Option("2x", 2), new Option("4x", 4), new Option("8x", 8) };
            Func<object, string> text = value => value + "x";
            var (options, selected) = Option.Including(presets, 4, text);
            Check(options == presets && selected == presets[2], "a preset value selects its choice");
            (options, selected) = Option.Including(presets, null, text);
            Check(options == presets && selected == presets[0], "no value selects the game's choice");
            (options, selected) = Option.Including(presets, 16, text);
            Check(options.Length == 5 && selected == options[4] && Equals(selected.Value, 16) && selected.Text == "16x",
                "a value beyond the presets is one more choice and keeps its value");
            (options, selected) = Option.Including(new[] { new Option("off", null), new Option("75 %", 75.0) }, 87.5, value => value + " %");
            Check(options.Length == 3 && Equals(selected.Value, 87.5), "a render scale beyond the presets is kept");
        }

        // A 32-bit executable that is read-only: the bit is set and cleared,
        // the executable stays read-only, the backup is never read-only.
        static void LargeAddressReadOnly()
        {
            string dir = TempDir("dxup_laa_ro_test");
            try
            {
                string original = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "cmd.exe");
                string exe = Path.Combine(dir, "game.exe");
                string backup = exe + LargeAddressAware.BackupSuffix;
                File.Copy(original, exe);
                File.SetAttributes(exe, FileAttributes.ReadOnly);
                bool enabled = false;
                try { LargeAddressAware.Enable(exe); enabled = true; } catch (Exception) { }
                Check(enabled && LargeAddressAware.Get(exe) == true && (File.GetAttributes(exe) & FileAttributes.ReadOnly) != 0
                    && (File.GetAttributes(backup) & FileAttributes.ReadOnly) == 0, "a read-only executable gets the bit and stays read-only");
                bool disabled = false;
                try { LargeAddressAware.Disable(exe); disabled = true; } catch (Exception) { }
                Check(disabled && LargeAddressAware.SameContent(exe, original) && !File.Exists(backup)
                    && (File.GetAttributes(exe) & FileAttributes.ReadOnly) != 0, "clearing it restores the read-only original");

                // A read-only backup left by an older manager: replaced, and
                // removed again.
                File.Copy(original, backup);
                File.SetAttributes(backup, FileAttributes.ReadOnly);
                enabled = disabled = false;
                try { LargeAddressAware.Enable(exe); enabled = true; } catch (Exception) { }
                try { LargeAddressAware.Disable(exe); disabled = true; } catch (Exception) { }
                Check(enabled && disabled && !File.Exists(backup) && LargeAddressAware.SameContent(exe, original),
                    "a read-only backup does not block setting and clearing the bit");

                // Writing fails (the game runs): no backup stays behind.
                using (new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.Read))
                {
                    try { LargeAddressAware.Enable(exe); } catch (Exception) { }
                }
                Check(!File.Exists(backup) && LargeAddressAware.Get(exe) == false, "a failed write leaves no backup");
            }
            finally
            {
                DeleteTree(dir);
            }
        }

        // Folders that are gone and paths a hand-edited games.cfg cannot use.
        static void DeployerFolders()
        {
            string root = TempDir("dxup_folders_test");
            try
            {
                var deployer = new GameDeployer(new DxupInstall(Path.Combine(root, "install")));
                var gone = new GameEntry { Name = "Gone", Dir = Path.Combine(root, "gone"), Exe = "game.exe", Enabled = true, LargeAddressAware = true };
                Check(deployer.State(gone) == GameState.FolderMissing, "a game whose folder is gone");
                Check(deployer.Remove(gone, new[] { gone }, out string error) && error == null, "a game whose folder is gone can be removed " + error);
                Check(!deployer.Install(gone, out error) && error != null, "a game whose folder is gone cannot be activated");

                var bad = new GameEntry { Name = "Bad", Dir = @"C:\Games\""Bad|<Name>", Exe = "ga|me.exe", Enabled = true };
                bool threw = false;
                GameState state = GameState.Active;
                try
                {
                    state = deployer.State(bad);
                    GameDeployer.DllsFor(bad);
                    GameDeployer.HasLocalConfig(bad);
                    GameDeployer.AntiCheat(bad);
                    deployer.SourceDir(bad);
                    Check(!deployer.Install(bad, out error) && deployer.Remove(bad, new[] { bad }, out error), "a bad path: install refused, remove allowed");
                }
                catch (Exception) { threw = true; }
                Check(!threw && state == GameState.FolderMissing, "a path with \" | < > throws nowhere");
            }
            finally
            {
                DeleteTree(root);
            }
        }

        // Another program's DLL updated over DXUP's (ReShade's dxgi.dll):
        // repairing keeps it as the newest backup, the older one under a
        // name of its own; removing DXUP brings back the newest.
        static void ForeignBackups()
        {
            string root = TempDir("dxup_backup_test");
            try
            {
                string bin = Path.Combine(root, "install", "bin");
                string gameDir = Path.Combine(root, "game");
                Directory.CreateDirectory(bin);
                Directory.CreateDirectory(gameDir);
                foreach (string dll in DxupInstall.Dlls)
                    File.WriteAllText(Path.Combine(bin, dll), "dxup_info_log.txt " + dll);
                string target = Path.Combine(gameDir, "dxgi.dll");
                string backup = target + GameDeployer.BackupSuffix;
                File.WriteAllText(target, "reshade 1");
                var deployer = new GameDeployer(new DxupInstall(Path.Combine(root, "install")));
                var game = new GameEntry { Name = "Game", Dir = gameDir, Exe = "game.exe", Enabled = true };
                Check(deployer.Install(game, out string error) && File.ReadAllText(backup) == "reshade 1", "the foreign DLL is backed up " + error);
                File.WriteAllText(target, "reshade 2");
                Check(deployer.State(game) == GameState.Outdated, "a foreign DLL over DXUP's: outdated");
                Check(deployer.Install(game, out error) && File.ReadAllText(backup) == "reshade 2"
                    && File.ReadAllText(backup + ".1") == "reshade 1" && DxupInstall.IsDxupFile(target),
                    "repairing keeps the newer foreign DLL and the older one " + error);
                // The same file again: it is a copy of the backup, nothing new.
                File.WriteAllText(target, "reshade 2");
                Check(deployer.Install(game, out error) && File.ReadAllText(backup) == "reshade 2" && !File.Exists(backup + ".2"),
                    "a copy of the backup is not backed up twice " + error);
                Check(deployer.Remove(game, new[] { game }, out error) && File.ReadAllText(target) == "reshade 2"
                    && !File.Exists(backup + ".1") && !File.Exists(backup), "removing restores the newest foreign DLL, the older ones go " + error);
            }
            finally
            {
                DeleteTree(root);
            }
        }

        // Names of DLLs in an executable, as ASCII or UTF-16 string literals.
        static void NamesInFile()
        {
            string dir = TempDir("dxup_names_test");
            try
            {
                string wide = Path.Combine(dir, "wide.exe");
                File.WriteAllBytes(wide, new byte[] { 1, 2, 3 }.Concat(System.Text.Encoding.Unicode.GetBytes("xx D3D9.DLL yy")).ToArray());
                string ascii = Path.Combine(dir, "ascii.exe");
                File.WriteAllBytes(ascii, System.Text.Encoding.ASCII.GetBytes("..DDraw.dll.."));
                string neither = Path.Combine(dir, "neither.exe");
                File.WriteAllBytes(neither, System.Text.Encoding.ASCII.GetBytes("d3d9_dll d\u00003d8 dd raw.dll d3d9.dl"));
                var names = DxupInstall.LegacyDlls.ToList();
                Check(GameDeployer.NamesIn(wide, names).SequenceEqual(new[] { "d3d9.dll" }), "a UTF-16 DLL name is found");
                Check(GameDeployer.NamesIn(ascii, names).SequenceEqual(new[] { "ddraw.dll" }), "an ASCII DLL name is found in any case");
                Check(!GameDeployer.NamesIn(neither, names).Any(), "other text is no DLL name");
            }
            finally
            {
                DeleteTree(dir);
            }
        }

        // Which folder the setup installs into, and which it uninstalls.
        static void InstallFolders()
        {
            Check(DxupInstall.TrimFolder(@"D:\") == @"D:\" && DxupInstall.TrimFolder(@"D:\Games\") == @"D:\Games"
               && new DxupInstall(@"D:\").GamesConfigPath == @"D:\games.cfg", "a drive's root keeps its separator");
            string root = TempDir("dxup_folder_test");
            try
            {
                string empty = Path.Combine(root, "empty");
                Directory.CreateDirectory(empty);
                Check(Installer.InstallFolder(empty, null) == empty, "an empty folder is used");
                Check(Installer.InstallFolder(Path.Combine(root, "new"), null) == Path.Combine(root, "new"), "a new folder is used");
                string games = Path.Combine(root, "Games");
                Directory.CreateDirectory(Path.Combine(games, "Some Game"));
                File.WriteAllText(Path.Combine(games, "notes.txt"), "mine");
                Check(Installer.InstallFolder(games + "\\", null) == Path.Combine(games, "DXUP"), "a folder with other files gets a DXUP folder");
                Check(Installer.InstallFolder(games, games) == games, "the registered folder is used");
                Directory.CreateDirectory(Path.Combine(games, "DXUP"));
                File.WriteAllText(Path.Combine(games, "DXUP", "other.txt"), "not DXUP");
                Check(Installer.InstallFolder(games, null) == null, "a DXUP folder with other files is refused");
                File.WriteAllText(Path.Combine(games, "DXUP", Installer.FileList), "README.txt\r\n");
                Check(Installer.InstallFolder(games, null) == null, "a file list without its header is not DXUP's");
                File.WriteAllText(Path.Combine(games, "DXUP", Installer.FileList), Installer.FileListHeader + "\r\nREADME.txt\r\n");
                Check(Installer.InstallFolder(games, null) == Path.Combine(games, "DXUP"), "an installation is upgraded");

                Check(!Installer.MayUninstall(games, null), "a folder that is no installation is not uninstalled");
                Check(Installer.MayUninstall(games, games + "\\"), "the registered installation is uninstalled");
                Check(Installer.MayUninstall(Path.Combine(games, "DXUP"), null), "a folder with the file list is an installation");
                string manager = Path.Combine(root, "manager");
                Directory.CreateDirectory(manager);
                File.Copy(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "notepad.exe"), Path.Combine(manager, DxupInstall.ManagerExe));
                Check(!Installer.MayUninstall(manager, null), "another program named DXUP Manager.exe is no installation");
                File.Copy(typeof(Program).Assembly.Location, Path.Combine(manager, DxupInstall.ManagerExe), true);
                Check(Installer.MayUninstall(manager, null), "DXUP's manager marks an installation");
            }
            finally
            {
                DeleteTree(root);
            }
        }

        // The uninstaller deletes DXUP's files and empty folders only.
        static void UninstallFiles()
        {
            string root = TempDir("dxup_uninstall_test");
            try
            {
                string dir = Path.Combine(root, "DXUP");
                string[] own =
                {
                    DxupInstall.ManagerExe, DxupInstall.ManagerExe + ".config", "README.txt", "LICENSE.txt", @"bin\d3d11.dll", @"bin\dxgi.dll",
                    @"bin32\d3d9.dll", @"bin\upscalers\libxess.dll", @"bin\upscalers\licenses\Intel-XeSS-LICENSE.txt", "games.cfg",
                    DxupInstall.UninstallExe, @"bin\listed.dll", @"bin\future.dll", @"bin\d3d11.dll.dxup-old123", "games.cfg.new",
                };
                string[] mine = { "notes.txt", @"bin\user.dll", @"Saves\save1.dat" };
                foreach (string name in own.Concat(mine))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(dir, name)));
                    File.WriteAllText(Path.Combine(dir, name), name);
                }
                File.SetAttributes(Path.Combine(dir, "README.txt"), FileAttributes.ReadOnly);
                File.WriteAllText(Path.Combine(root, "outside.txt"), "outside");
                File.WriteAllText(Path.Combine(root, "outside2.txt"), "outside");
                File.WriteAllLines(Path.Combine(dir, Installer.FileList), new[] { Installer.FileListHeader,
                    @"bin\listed.dll", @"..\outside.txt", Path.Combine(root, "outside2.txt"), "notes.txt\\..\\..\\outside.txt" });
                // Another program's list of the same name lists nothing of DXUP's.
                string foreign = Path.Combine(root, "Foreign");
                Directory.CreateDirectory(foreign);
                File.WriteAllText(Path.Combine(foreign, "mine.txt"), "mine");
                File.WriteAllText(Path.Combine(foreign, Installer.FileList), "mine.txt\r\n");
                Check(!Installer.MayUninstall(foreign, null) && Installer.ReadFileList(foreign).Count == 0
                    && Installer.OwnFiles(foreign, Enumerable.Empty<string>(), false).Count == 0, "a list without DXUP's header is ignored");

                var failed = Installer.RemoveFiles(dir, new[] { @"bin\future.dll" }, false, null);
                Check(failed.Count == 0 && own.All(name => !File.Exists(Path.Combine(dir, name))) && !File.Exists(Path.Combine(dir, Installer.FileList)),
                    "uninstalling deletes DXUP's files: " + string.Join(", ", own.Where(name => File.Exists(Path.Combine(dir, name)))));
                Check(mine.All(name => File.Exists(Path.Combine(dir, name))) && File.Exists(Path.Combine(root, "outside.txt"))
                    && File.Exists(Path.Combine(root, "outside2.txt")), "uninstalling keeps the user's files and everything outside");
                Check(!Directory.Exists(Path.Combine(dir, "bin32")) && !Directory.Exists(Path.Combine(dir, @"bin\upscalers"))
                    && Directory.Exists(Path.Combine(dir, "bin")) && Directory.Exists(dir), "empty DXUP folders go, others stay");

                // Settings kept: games.cfg and the list stay, the folder too.
                string kept = Path.Combine(root, "Kept");
                foreach (string name in new[] { "README.txt", "games.cfg", DxupInstall.UninstallExe, @"bin\d3d11.dll" })
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(kept, name)));
                    File.WriteAllText(Path.Combine(kept, name), name);
                }
                File.WriteAllText(Path.Combine(kept, Installer.FileList), Installer.FileListHeader + "\r\nREADME.txt\r\n");
                string self = Path.Combine(kept, DxupInstall.UninstallExe);
                Installer.RemoveFiles(kept, Enumerable.Empty<string>(), true, self);
                Check(File.Exists(Path.Combine(kept, "games.cfg")) && File.Exists(Path.Combine(kept, Installer.FileList)) && File.Exists(self)
                    && !File.Exists(Path.Combine(kept, "README.txt")) && !Directory.Exists(Path.Combine(kept, "bin"))
                    && Installer.InstallFolder(kept, null) == kept, "kept settings stay for the next installation; the running uninstaller stays");

                // Only DXUP in the folder: it goes as a whole.
                string only = Path.Combine(root, "Only");
                Directory.CreateDirectory(Path.Combine(only, "bin"));
                File.WriteAllText(Path.Combine(only, "bin", "d3d11.dll"), "x");
                File.WriteAllText(Path.Combine(only, DxupInstall.UninstallExe), "x");
                Installer.RemoveFiles(only, Enumerable.Empty<string>(), false, null);
                Check(!Directory.Exists(only), "an installation folder with nothing else goes");

                string command = Installer.SelfDeleteCommand(@"C:\DXUP\uninstall.exe.dxup-old1", @"C:\DXUP");
                Check(command.Contains("for /l") && command.Contains("del /f /q \"C:\\DXUP\\uninstall.exe.dxup-old1\"")
                    && command.Contains("rmdir \"C:\\DXUP\"") && !command.Contains("/s"), "the uninstaller deletes itself after it exited, the folder only when empty");
            }
            finally
            {
                DeleteTree(root);
            }
        }

        [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
        static extern IntPtr LoadLibraryEx(string file, IntPtr reserved, int flags);

        [System.Runtime.InteropServices.DllImport("kernel32.dll")]
        static extern bool FreeLibrary(IntPtr module);

        // A DLL a game loaded from the installation (an upscaler runtime):
        // an upgrade replaces it and an uninstall removes it, the old file
        // renamed and deleted at the next restart.
        static void ReplaceInUse()
        {
            string dir = TempDir("dxup_in_use_test");
            IntPtr module = IntPtr.Zero;
            try
            {
                string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
                string target = Path.Combine(dir, "libxess.dll");
                File.Copy(Path.Combine(system, "version.dll"), target);
                module = LoadLibraryEx(target, IntPtr.Zero, 0x20);   // LOAD_LIBRARY_AS_IMAGE_RESOURCE: mapped as an image
                bool locked = false;
                try { File.Delete(target); } catch (Exception) { locked = true; }
                if (!locked)
                {
                    Console.WriteLine("[SKIP] a DLL in use (could be deleted)");
                    return;
                }
                File.WriteAllText(target + ".new", "new version");
                s_restartDeletes.Clear();
                bool replaced = false;
                try { Installer.ReplaceFile(target + ".new", target); replaced = true; } catch (Exception) { }
                Check(replaced && File.ReadAllText(target) == "new version" && s_restartDeletes.Count == 1
                    && s_restartDeletes[0].StartsWith(target + GameDeployer.OldSuffix, StringComparison.OrdinalIgnoreCase) && File.Exists(s_restartDeletes[0]),
                    "a DLL in use is replaced, the old one goes at the next restart");
                FreeLibrary(module);
                module = IntPtr.Zero;

                // Uninstalling with the DLL in use.
                string inUse = Path.Combine(dir, "bin", "upscalers", "libxess.dll");
                Directory.CreateDirectory(Path.GetDirectoryName(inUse));
                File.Copy(Path.Combine(system, "version.dll"), inUse);
                module = LoadLibraryEx(inUse, IntPtr.Zero, 0x20);
                s_restartDeletes.Clear();
                var failed = Installer.RemoveFiles(dir, Enumerable.Empty<string>(), false, null);
                Check(failed.Count == 0 && !File.Exists(inUse) && s_restartDeletes.Count == 2
                    && s_restartDeletes[1] == Path.Combine(dir, "bin", "upscalers"), "uninstalling: a DLL in use goes at the next restart, its folder after it");
            }
            finally
            {
                if (module != IntPtr.Zero)
                    FreeLibrary(module);
                DeleteTree(dir);
            }
        }

        static void CrashReports()
        {
            string exe = @"D:\Games\Foo\game.exe";
            string own = "Faulting application name: game.exe, version: 1.0.0.0\r\nFaulting application path: D:\\Games\\Foo\\game.exe\r\n";
            string other = "Faulting application name: game.exe, version: 1.0.0.0\r\nFaulting application path: E:\\Other\\game.exe\r\n";
            Check(Diagnostics.IsAbout(own, exe, "game.exe"), "a crash report of the game's program");
            Check(!Diagnostics.IsAbout(other, exe, "game.exe"), "a program of the same name in another folder is left out");
            Check(Diagnostics.IsAbout("Problem signature:\r\nP1: game.exe\r\nP2: 1.0.0.0", exe, "game.exe"), "a report naming the program only");
            Check(!Diagnostics.IsAbout("Faulting application name: mygame.exe, version 1", exe, "game.exe")
               && !Diagnostics.IsAbout("game.exe.config could not be read", exe, "game.exe"), "the name as a whole word only");
            Check(!Diagnostics.IsAbout(own, @"D:\Games\Foo\", ""), "no program, no reports");
        }
    }
}
