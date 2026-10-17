using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Diagnostics.Eventing.Reader;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Dxup.Shared;
using Microsoft.Win32;

namespace Dxup.Manager
{
    // Diagnostic packages for bug reports, and starting a game under a GPU
    // debugger (RenderDoc, PIX). No UI here.
    public static class Diagnostics
    {
        // Paths and names that identify the user: the profile folder, any
        // C:\Users\<name>, the user and computer name.
        public static string Anonymize(string text)
        {
            if (string.IsNullOrEmpty(text))
                return text;
            string profile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            if (!string.IsNullOrEmpty(profile))
                text = Regex.Replace(text, Regex.Escape(profile), "%USERPROFILE%", RegexOptions.IgnoreCase);
            text = Regex.Replace(text, @"(?i)([a-z]:[\\/]+Users[\\/]+)(?!Public\b|Default\b|All Users\b)[^\\/\r\n""]+", "$1<user>");
            foreach (string name in new[] { Environment.MachineName, Environment.UserName })
            {
                if (!string.IsNullOrEmpty(name) && name.Length >= 3)
                    text = Regex.Replace(text, @"(?<![\w])" + Regex.Escape(name) + @"(?![\w])",
                        name == Environment.UserName ? "<user>" : "<pc>", RegexOptions.IgnoreCase);
            }
            return text;
        }

        // A zip with what helps to find a problem: DXUP's logs and frame dumps
        // from the game folder, the game's settings, the system and recent
        // crashes of the game. Text is anonymized. Returns null or the error.
        public static string CreatePackage(GameEntry game, string games, string dxupVersion, string zipPath)
        {
            try
            {
                if (File.Exists(zipPath))
                    File.Delete(zipPath);
                using (var zip = ZipFile.Open(zipPath, ZipArchiveMode.Create))
                {
                    AddText(zip, "README.txt",
                        "DXUP diagnostic package\r\n" +
                        "Created " + DateTime.Now.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture) + " by DXUP Manager " + dxupVersion + ".\r\n" +
                        "system.txt: Windows, CPU, GPUs and drivers, the game's DLLs. settings.txt: DXUP's settings for the game.\r\n" +
                        "logs\\: DXUP's logs from the game folder. frames\\: images of the game (only with saved frames on).\r\n" +
                        "crashes.txt: Windows error reports of the game.\r\n" +
                        "The user folder, user and computer names are replaced by %USERPROFILE%, <user> and <pc>.\r\n");
                    AddText(zip, "system.txt", SystemInfo(game, dxupVersion));
                    AddText(zip, "settings.txt", games + LocalConfig(game));
                    if (Directory.Exists(game.Dir))
                    {
                        var logs = Directory.GetFiles(game.Dir, "logdxup*.log")
                            .Concat(Directory.GetFiles(game.Dir, "dxup_upscaler_*.txt"))
                            .Concat(Directory.GetFiles(game.Dir, "dxup_gpu_profile_out.txt"));
                        foreach (string log in logs)
                            AddText(zip, "logs/" + Path.GetFileName(log), ReadShared(log, 64 << 20));
                        // The newest frame dumps, as PNG.
                        var frames = new DirectoryInfo(game.Dir).GetFiles("dxup_frame_*.bmp")
                            .OrderByDescending(f => f.LastWriteTimeUtc).Take(4);
                        foreach (var frame in frames)
                            AddPng(zip, "frames/" + Path.GetFileNameWithoutExtension(frame.Name) + ".png", frame.FullName);
                        // Frames DXUP saw flicker (and the frame before each), the log names them.
                        var glitches = new DirectoryInfo(game.Dir).GetFiles("dxup_glitch_*.bmp")
                            .OrderBy(f => f.LastWriteTimeUtc).Take(8);
                        foreach (var frame in glitches)
                            AddPng(zip, "glitches/" + Path.GetFileNameWithoutExtension(frame.Name) + ".png", frame.FullName);
                    }
                    AddText(zip, "crashes.txt", Crashes(game.Exe));
                }
                return null;
            }
            catch (Exception e)
            {
                return e.Message;
            }
        }

        static void AddText(ZipArchive zip, string name, string text)
        {
            var entry = zip.CreateEntry(name, CompressionLevel.Optimal);
            using (var writer = new StreamWriter(entry.Open(), new UTF8Encoding(false)))
                writer.Write(Anonymize(text ?? ""));
        }

        static void AddPng(ZipArchive zip, string name, string bmpPath)
        {
            try
            {
                using (var image = System.Drawing.Image.FromFile(bmpPath))
                using (var stream = zip.CreateEntry(name, CompressionLevel.Optimal).Open())
                    image.Save(stream, System.Drawing.Imaging.ImageFormat.Png);
            }
            catch (Exception) { }
        }

        // Logs of a running game are open for writing.
        static string ReadShared(string path, long maxBytes)
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            {
                if (stream.Length > maxBytes)
                    stream.Seek(-maxBytes, SeekOrigin.End);
                using (var reader = new StreamReader(stream, Encoding.UTF8))
                    return reader.ReadToEnd();
            }
        }

        static string LocalConfig(GameEntry game)
        {
            string path = Path.Combine(game.Dir, "dxup.conf");
            return File.Exists(path) ? "\r\n# dxup.conf in the game folder (takes precedence):\r\n" + File.ReadAllText(path) : "";
        }

        static string SystemInfo(GameEntry game, string dxupVersion)
        {
            var text = new StringBuilder();
            text.AppendLine("DXUP " + dxupVersion);
            Wmi(text, "SELECT Caption, Version, BuildNumber, OSArchitecture FROM Win32_OperatingSystem", "Windows",
                "Caption", "Version", "BuildNumber", "OSArchitecture");
            Wmi(text, "SELECT Name, NumberOfCores, NumberOfLogicalProcessors FROM Win32_Processor", "CPU",
                "Name", "NumberOfCores", "NumberOfLogicalProcessors");
            Wmi(text, "SELECT TotalPhysicalMemory FROM Win32_ComputerSystem", "RAM (GB)", "TotalPhysicalMemory");
            Wmi(text, "SELECT Name, DriverVersion, DriverDate FROM Win32_VideoController", "GPU",
                "Name", "DriverVersion", "DriverDate");
            text.AppendLine("D3D12 debug layer installed: " + (DiagnosticMarkers.DebugLayerInstalled ? "yes" : "no"));
            text.AppendLine();
            text.AppendLine("Game: " + game.Name + (string.IsNullOrEmpty(game.AppId) ? "" : " (" + game.AppId + ")"));
            text.AppendLine("Program: " + game.ExePath);
            var pe = PeFile.Read(game.ExePath);
            if (pe != null)
                text.AppendLine("Bits: " + (pe.Is64Bit ? "64" : pe.Is32Bit ? "32" : "?") + ", imports: " +
                    string.Join(", ", pe.Imports.Where(i => i.StartsWith("d3d", StringComparison.OrdinalIgnoreCase)
                        || i.StartsWith("dxgi", StringComparison.OrdinalIgnoreCase) || i.StartsWith("ddraw", StringComparison.OrdinalIgnoreCase))));
            text.AppendLine("DXUP active: " + (game.Enabled ? "yes" : "no"));
            text.AppendLine("Diagnostics on: " + string.Join(", ", DiagnosticMarkers.All.Where(m => DiagnosticMarkers.IsOn(game.Dir, m))));
            text.AppendLine("DLLs in the game folder:");
            foreach (string dll in DxupInstall.Dlls.Concat(DxupInstall.LegacyDlls))
            {
                string path = Path.Combine(game.Dir, dll);
                if (File.Exists(path))
                    text.AppendLine("  " + dll + " " + DxupInstall.FileVersion(path) + (DxupInstall.IsDxupFile(path) ? " (DXUP)" : " (not DXUP)"));
            }
            return text.ToString();
        }

        static void Wmi(StringBuilder text, string query, string label, params string[] properties)
        {
            try
            {
                using (var searcher = new System.Management.ManagementObjectSearcher(query))
                {
                    foreach (var item in searcher.Get())
                        text.AppendLine(label + ": " + string.Join(", ", properties.Select(p => WmiValue(p, item[p]))));
                }
            }
            catch (Exception) { text.AppendLine(label + ": ?"); }
        }

        static string WmiValue(string property, object value)
        {
            string text = Convert.ToString(value, CultureInfo.InvariantCulture) ?? "";
            if (property == "TotalPhysicalMemory" && ulong.TryParse(text, out ulong bytes))
                return (bytes / (1024.0 * 1024 * 1024)).ToString("0.0", CultureInfo.InvariantCulture);
            // CIM dates: yyyyMMddHHmmss.ffffff+zzz
            if (property == "DriverDate" && text.Length >= 8)
                return text.Substring(0, 4) + "-" + text.Substring(4, 2) + "-" + text.Substring(6, 2);
            return text;
        }

        // Application errors and Windows error reports of the game's program
        // in the last two weeks.
        static string Crashes(string exe)
        {
            var text = new StringBuilder();
            try
            {
                string query = "*[System[(Provider[@Name='Application Error'] or Provider[@Name='Windows Error Reporting'] or Provider[@Name='Application Hang'])" +
                               " and TimeCreated[timediff(@SystemTime) <= 1209600000]]]";
                using (var reader = new EventLogReader(new EventLogQuery("Application", PathType.LogName, query) { ReverseDirection = true }))
                {
                    int found = 0;
                    for (EventRecord record = reader.ReadEvent(); record != null && found < 20; record = reader.ReadEvent())
                    {
                        using (record)
                        {
                            string message;
                            try { message = record.FormatDescription(); } catch (Exception) { message = null; }
                            if (message == null || message.IndexOf(exe, StringComparison.OrdinalIgnoreCase) < 0)
                                continue;
                            text.AppendLine(record.TimeCreated?.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + " " + record.ProviderName);
                            text.AppendLine(message.Trim());
                            text.AppendLine();
                            found++;
                        }
                    }
                }
            }
            catch (Exception e) { text.AppendLine("(event log not readable: " + e.Message + ")"); }
            return text.Length == 0 ? "No error reports of " + exe + " in the last two weeks.\r\n" : text.ToString();
        }

        // ---- GPU debuggers ----

        public static string RenderDocPath()
        {
            try
            {
                using (var key = Registry.ClassesRoot.OpenSubKey(@"RenderDoc.RDCCapture.1\shell\open\command"))
                {
                    var match = Regex.Match(key?.GetValue(null) as string ?? "", "^\"([^\"]+)\"");
                    if (match.Success && File.Exists(match.Groups[1].Value))
                        return match.Groups[1].Value;
                }
            }
            catch (Exception) { }
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "RenderDoc", "qrenderdoc.exe");
            return File.Exists(path) ? path : null;
        }

        // The newest PIX on Windows.
        public static string PixPath()
        {
            try
            {
                string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Microsoft PIX");
                return Directory.Exists(root)
                    ? Directory.GetDirectories(root).Select(d => Path.Combine(d, "WinPix.exe")).Where(File.Exists)
                        .OrderByDescending(p => File.GetLastWriteTimeUtc(p)).FirstOrDefault()
                    : null;
            }
            catch (Exception) { return null; }
        }

        // RenderDoc with the game filled in on its Launch Application page
        // (a capture settings file); child processes are hooked too, for games
        // that a launcher starts again. RenderDoc and PIX run without the
        // manager's administrator rights, as the games they start must.
        public static void StartRenderDoc(string qrenderdoc, GameEntry game)
        {
            string settings = Path.Combine(Path.GetTempPath(), "dxup_renderdoc.cap");
            // A Steam game started directly stops with "Application load
            // error 5:0000065434" unless it knows its Steam app ID.
            string environment = SteamAppId(game) is string appId
                ? "[\n      {\n        \"separator\": \"Platform style\",\n        \"type\": \"Set\",\n" +
                  "        \"value\": " + JsonString(appId) + ",\n        \"variable\": \"SteamAppId\"\n      }\n    ]"
                : "[]";
            string json = "{\n  \"rdocCaptureSettings\": 1,\n  \"settings\": {\n" +
                "    \"autoStart\": false,\n    \"commandLine\": \"\",\n    \"environment\": " + environment + ",\n" +
                "    \"executable\": " + JsonString(game.ExePath) + ",\n    \"inject\": false,\n    \"numQueuedFrames\": 0,\n" +
                "    \"options\": {\n      \"allowFullscreen\": true,\n      \"allowVSync\": true,\n      \"apiValidation\": false,\n" +
                "      \"captureAllCmdLists\": false,\n      \"captureCallstacks\": false,\n      \"captureCallstacksOnlyDraws\": false,\n" +
                "      \"debugOutputMute\": true,\n      \"delayForDebugger\": 0,\n      \"hookIntoChildren\": true,\n" +
                "      \"refAllResources\": false,\n      \"softMemoryLimit\": 0,\n      \"verifyBufferAccess\": false\n    },\n" +
                "    \"queuedFrameCap\": 0,\n    \"workingDir\": " + JsonString(game.Dir) + "\n  }\n}\n";
            File.WriteAllText(settings, json, new UTF8Encoding(false));
            Unelevated.Start(qrenderdoc, "\"" + settings + "\"", Path.GetDirectoryName(qrenderdoc));
        }

        public static void StartPix(string winpix, GameEntry game)
        {
            var environment = SteamAppId(game) is string appId
                ? new Dictionary<string, string> { ["SteamAppId"] = appId } : null;
            Unelevated.Start(winpix, "", Path.GetDirectoryName(winpix), environment);
        }

        // The Steam app ID of a game the manager found in Steam, or null.
        public static string SteamAppId(GameEntry game) =>
            game.AppId.Length != 0 && game.AppId.All(char.IsDigit) ? game.AppId : null;

        static string JsonString(string value) =>
            "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }
}
