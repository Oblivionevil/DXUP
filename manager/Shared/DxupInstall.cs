using System;
using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using Microsoft.Win32;

namespace Dxup.Shared
{
    // The DXUP installation: where it is, its DLLs and games.cfg, and the
    // registry entries the setup writes (64-bit view, as DXUP's DLLs read it).
    public sealed class DxupInstall
    {
        public const string RegistryKey = @"SOFTWARE\DXUP";
        public const string UninstallKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\DXUP";
        public const string ManagerExe = "DXUP Manager.exe";
        public const string UninstallExe = "uninstall.exe";
        public const string ManagerMutex = @"Global\DXUP.Manager";
        // Every game gets these; the legacy APIs run on them.
        public static readonly string[] Dlls = { "d3d11.dll", "dxgi.dll" };
        // Games get these when they use the API (Direct3D 7, 8, 9).
        public static readonly string[] LegacyDlls = { "ddraw.dll", "d3d8.dll", "d3d9.dll" };

        public string Dir { get; }
        public string BinDir => Path.Combine(Dir, "bin");
        // DXUP's DLLs for 32-bit games.
        public string BinDir32 => Path.Combine(Dir, "bin32");
        public string GamesConfigPath => Path.Combine(Dir, GamesConfig.FileName);

        public DxupInstall(string dir) { Dir = dir.TrimEnd('\\', '/'); }

        public static string DefaultDir =>
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "DXUP");

        static RegistryKey LocalMachine() => Microsoft.Win32.RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);

        // The installation the setup registered, or null.
        public static string RegisteredDir()
        {
            using (var root = LocalMachine())
            using (var key = root.OpenSubKey(RegistryKey))
                return key?.GetValue("InstallDir") as string;
        }

        public static string RegisteredVersion()
        {
            using (var root = LocalMachine())
            using (var key = root.OpenSubKey(RegistryKey))
                return key?.GetValue("Version") as string;
        }

        public static void Register(string dir, string version)
        {
            using (var root = LocalMachine())
            using (var key = root.CreateSubKey(RegistryKey))
            {
                key.SetValue("InstallDir", dir);
                key.SetValue("Version", version);
            }
        }

        public static void Unregister()
        {
            using (var root = LocalMachine())
            {
                root.DeleteSubKeyTree(RegistryKey, false);
                root.DeleteSubKeyTree(UninstallKey, false);
            }
        }

        // Whether a file is one of DXUP's DLLs: by its version resource, or,
        // for builds from before it had one, by the name of its log marker.
        public static bool IsDxupFile(string path)
        {
            try
            {
                if (!File.Exists(path))
                    return false;
                if (FileVersionInfo.GetVersionInfo(path).ProductName == "DXUP")
                    return true;
                var info = new FileInfo(path);
                if (info.Length > 64L << 20)
                    return false;
                return Contains(File.ReadAllBytes(path), s_logMarker);
            }
            catch (Exception)
            {
                return false;
            }
        }

        static readonly byte[] s_logMarker = System.Text.Encoding.ASCII.GetBytes("dxup_info_log.txt");

        static bool Contains(byte[] data, byte[] pattern)
        {
            for (int i = 0; i + pattern.Length <= data.Length; i++)
            {
                if (data[i] != pattern[0])
                    continue;
                int j = 1;
                while (j < pattern.Length && data[i + j] == pattern[j])
                    j++;
                if (j == pattern.Length)
                    return true;
            }
            return false;
        }

        public static string FileVersion(string path)
        {
            try
            {
                return File.Exists(path) ? FileVersionInfo.GetVersionInfo(path).FileVersion ?? "" : "";
            }
            catch (Exception)
            {
                return "";
            }
        }

        public static string Hash(string path)
        {
            try
            {
                using (var sha = SHA256.Create())
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    return BitConverter.ToString(sha.ComputeHash(stream));
            }
            catch (Exception)
            {
                return "";
            }
        }

        public string DllVersion => FileVersion(Path.Combine(BinDir, Dlls[0]));
    }
}
