using System;
using System.IO;
using System.Linq;

namespace Dxup.Shared
{
    // Empty files next to DXUP's DLLs that turn on its diagnostics; DXUP looks
    // for them when the game starts.
    public static class DiagnosticMarkers
    {
        public const string Log = "dxup_info_log.txt";                 // detailed log (slow)
        public const string DebugLayer = "dxup_debug.txt";             // D3D12 debug layer
        public const string GpuValidation = "dxup_gpu_validation.txt"; // GPU-based validation (very slow, with the debug layer)
        public const string Dred = "dxup_dred.txt";                    // breadcrumbs of a GPU hang or crash
        public const string Exceptions = "dxup_exception_trace.txt";   // every exception of the game with a call stack
        public const string FrameDump = "dxup_frame_dump.txt";         // a few frames as images, one frame's calls in the log

        public static readonly string[] All = { Log, DebugLayer, GpuValidation, Dred, Exceptions, FrameDump };

        public static bool IsOn(string dir, string marker) => File.Exists(Path.Combine(dir, marker));

        public static bool AnyOn(string dir) => All.Any(marker => IsOn(dir, marker));

        public static void Set(string dir, string marker, bool on)
        {
            string path = Path.Combine(dir, marker);
            if (on && !File.Exists(path))
                File.WriteAllText(path, "");
            else if (!on && File.Exists(path))
                File.Delete(path);
        }

        public static void ClearAll(string dir)
        {
            foreach (string marker in All)
            {
                try { Set(dir, marker, false); } catch (Exception) { }
            }
        }

        // The frames DXUP saved (dxup_frame_<n>.bmp, about 33 MB each at 4K).
        public static void DeleteFrames(string dir)
        {
            try
            {
                foreach (string frame in Directory.GetFiles(dir, "dxup_frame_*.bmp"))
                {
                    try { File.Delete(frame); } catch (Exception) { }
                }
            }
            catch (Exception) { }
        }

        // The debug layer is an optional Windows feature ("Graphics Tools").
        public static bool DebugLayerInstalled =>
            File.Exists(Path.Combine(Environment.SystemDirectory, "d3d12SDKLayers.dll"));
    }
}
