using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace Dxup.Shared
{
    // Settings DXUP applies to one game; null means "as the game asks".
    public sealed class GameSettings
    {
        public int? Anisotropy;          // 0 = off, 1-16
        public float? LodBias;
        public int? MsaaSamples;         // 2, 4, 8, 16
        public bool MsaaForce;
        public bool MsaaScopeAll;        // false = geometry
        public string Sgssaa = "off";    // off, true, alpha
        public int? SyncInterval;        // 0 = VSync off, 1-4 on
        public int? MaxFrameLatency;     // 1-16
        public bool AsyncShaders;        // pipelines compile in the background
        public bool ShaderPrecompile = true; // pipelines of earlier runs compile at load
        public string Upscaler = "off";  // off, dlss, fsr, xess: in place of the game's TAA
        public bool UpscalerAuto;        // find the game's TAA pass without a profile
        public bool UpscalerInject;      // DXUP's own temporal anti-aliasing for games without TAA
        public bool SpatialUpscale;      // scale a back buffer smaller than the window up at Present
        public int? RenderScale;         // percent of the asked back buffer size (33-99), null = all
        public string Adapter;           // part of the GPU's name in lower case, "warp"; null = the game's choice
        public string SpoofVendor;       // nvidia, amd, intel: the GPU the game sees; null = the real one
        public float? SpatialSharpness;  // sharpening of DXUP's upscale at Present (0-1); null = DXUP's 0.2
        public string CustomVendorId;    // hex ids and name the game sees, over SpoofVendor's; null = none
        public string CustomDeviceId;
        public string CustomDeviceDesc;
        public bool DirectBackBuffer = true; // flip model games draw into the real swap chain buffers
        public string FloatEmulation;    // Direct3D 7-9 shaders' 0 * inf: strict, off; null = auto
        public string NanDepth;          // Direct3D 7-9 vertices with NaN depth: discard, zero; null = auto
        public bool FxcCompiler;         // Direct3D 7-9 shaders through Microsoft's compiler instead of DXUP's
        public bool SubmitThread;        // command lists go to the GPU on a thread of their own
        public bool RecordThread;        // command lists are recorded on a second thread
        public bool PixelOutputMasks = true; // pixel shader outputs no target takes leave the shader
        public bool BarrierHoisting = true;  // barriers that wait for nothing move earlier
        public bool? RelaxedUavBarriers; // no barriers between draws of a pipeline into its UAVs; null = DXUP's choice
        public bool? LazyTargets;        // render targets get memory when first written; null = on NVIDIA

        public GameSettings Clone() => (GameSettings)MemberwiseClone();

        // Reads one setting as DXUP names it; false for keys it does not know.
        public bool Set(string key, string value)
        {
            switch (key.ToLowerInvariant())
            {
                case "d3d11.sampleranisotropy": Anisotropy = ParseInt(value); return true;
                case "d3d11.samplerlodbias": LodBias = ParseFloat(value); return true;
                case "d3d11.msaasamples": MsaaSamples = ParseInt(value); return true;
                case "d3d11.msaaforce": MsaaForce = ParseBool(value) ?? false; return true;
                case "d3d11.msaascope": MsaaScopeAll = value.Trim().Equals("all", StringComparison.OrdinalIgnoreCase); return true;
                case "d3d11.sgssaa":
                    if (value.Trim().Equals("alpha", StringComparison.OrdinalIgnoreCase))
                        Sgssaa = "alpha";
                    else
                        Sgssaa = ParseBool(value) == true ? "true" : "off";
                    return true;
                case "dxgi.syncinterval": SyncInterval = ParseInt(value); return true;
                case "dxgi.maxframelatency": MaxFrameLatency = ParseInt(value); return true;
                case "d3d11.asyncshaders": AsyncShaders = ParseBool(value) ?? false; return true;
                case "d3d11.shaderprecompile": ShaderPrecompile = ParseBool(value) ?? true; return true;
                case "d3d11.upscaler":
                    string kind = value.Trim().ToLowerInvariant();
                    Upscaler = kind == "dlss" || kind == "fsr" || kind == "xess" ? kind : "off";
                    return true;
                case "d3d11.upscalerauto": UpscalerAuto = ParseBool(value) ?? false; return true;
                case "d3d11.upscalerinject": UpscalerInject = ParseBool(value) ?? false; return true;
                case "dxgi.upscale": SpatialUpscale = value.Trim().Equals("spatial", StringComparison.OrdinalIgnoreCase); return true;
                case "dxgi.adapter":
                    string adapter = value.Trim().ToLowerInvariant();
                    Adapter = adapter.Length > 0 ? adapter : null;
                    return true;
                case "dxgi.spoofvendor":
                    string vendor = value.Trim().ToLowerInvariant();
                    SpoofVendor = vendor == "nvidia" || vendor == "amd" || vendor == "intel" ? vendor : null;
                    return true;
                case "dxgi.upscalesharpness":
                    float? sharpness = ParseFloat(value);
                    SpatialSharpness = sharpness.HasValue ? (float?)Math.Min(Math.Max(sharpness.Value, 0.0f), 1.0f) : null;
                    return true;
                case "dxgi.customvendorid": CustomVendorId = ParseHex(value); return true;
                case "dxgi.customdeviceid": CustomDeviceId = ParseHex(value); return true;
                case "dxgi.customdevicedesc":
                    CustomDeviceDesc = value.Trim().Length > 0 ? value.Trim() : null;
                    return true;
                case "dxgi.directbackbuffer": DirectBackBuffer = ParseBool(value) ?? true; return true;
                case "d3d9.floatemulation":
                    switch (value.Trim().ToLowerInvariant())
                    {
                        case "strict": case "true": case "on": case "1": FloatEmulation = "strict"; break;
                        case "false": case "off": case "0": FloatEmulation = "off"; break;
                        default: FloatEmulation = null; break;
                    }
                    return true;
                case "d3d9.nandepth":
                    switch (value.Trim().ToLowerInvariant())
                    {
                        case "zero": case "true": case "1": NanDepth = "zero"; break;
                        case "discard": case "false": case "0": NanDepth = "discard"; break;
                        default: NanDepth = null; break;
                    }
                    return true;
                case "d3d9.shadercompiler": FxcCompiler = value.Trim().Equals("fxc", StringComparison.OrdinalIgnoreCase); return true;
                case "d3d12.submitthread": SubmitThread = ParseBool(value) ?? false; return true;
                case "d3d12.recordthread": RecordThread = ParseBool(value) ?? false; return true;
                case "d3d12.pixeloutputmasks": PixelOutputMasks = ParseBool(value) ?? true; return true;
                case "d3d12.barrierhoisting": BarrierHoisting = ParseBool(value) ?? true; return true;
                case "d3d11.relaxeduavbarriers": RelaxedUavBarriers = ParseBool(value); return true;
                case "d3d12.lazytargets": LazyTargets = ParseBool(value); return true;
                case "dxgi.renderscale":
                    float? scale = ParseFloat(value);
                    RenderScale = scale.HasValue && scale.Value < 1.0f ? (int?)Math.Max(33, (int)Math.Round(scale.Value * 100.0f)) : null;
                    return true;
                default: return false;
            }
        }

        // The settings that differ from the game's own behaviour, as DXUP reads them.
        public IEnumerable<KeyValuePair<string, string>> Entries()
        {
            if (Anisotropy.HasValue) yield return Entry("d3d11.samplerAnisotropy", Anisotropy.Value.ToString(CultureInfo.InvariantCulture));
            if (LodBias.HasValue && LodBias.Value != 0.0f) yield return Entry("d3d11.samplerLodBias", LodBias.Value.ToString("0.##", CultureInfo.InvariantCulture));
            if (MsaaSamples.HasValue) yield return Entry("d3d11.msaaSamples", MsaaSamples.Value.ToString(CultureInfo.InvariantCulture));
            if (MsaaForce) yield return Entry("d3d11.msaaForce", "true");
            if (MsaaScopeAll) yield return Entry("d3d11.msaaScope", "all");
            if (Sgssaa != "off") yield return Entry("d3d11.sgssaa", Sgssaa);
            if (SyncInterval.HasValue) yield return Entry("dxgi.syncInterval", SyncInterval.Value.ToString(CultureInfo.InvariantCulture));
            if (MaxFrameLatency.HasValue) yield return Entry("dxgi.maxFrameLatency", MaxFrameLatency.Value.ToString(CultureInfo.InvariantCulture));
            if (AsyncShaders) yield return Entry("d3d11.asyncShaders", "true");
            if (!ShaderPrecompile) yield return Entry("d3d11.shaderPrecompile", "false");
            if (Upscaler != "off") yield return Entry("d3d11.upscaler", Upscaler);
            if (UpscalerAuto) yield return Entry("d3d11.upscalerAuto", "true");
            if (UpscalerInject) yield return Entry("d3d11.upscalerInject", "true");
            if (SpatialUpscale) yield return Entry("dxgi.upscale", "spatial");
            if (RenderScale.HasValue && RenderScale.Value < 100)
                yield return Entry("dxgi.renderScale", (RenderScale.Value / 100.0f).ToString("0.##", CultureInfo.InvariantCulture));
            if (!string.IsNullOrEmpty(Adapter)) yield return Entry("dxgi.adapter", Adapter);
            if (!string.IsNullOrEmpty(SpoofVendor)) yield return Entry("dxgi.spoofVendor", SpoofVendor);
            if (SpatialSharpness.HasValue)
                yield return Entry("dxgi.upscaleSharpness", SpatialSharpness.Value.ToString("0.##", CultureInfo.InvariantCulture));
            if (!string.IsNullOrEmpty(CustomVendorId)) yield return Entry("dxgi.customVendorId", CustomVendorId);
            if (!string.IsNullOrEmpty(CustomDeviceId)) yield return Entry("dxgi.customDeviceId", CustomDeviceId);
            if (!string.IsNullOrEmpty(CustomDeviceDesc)) yield return Entry("dxgi.customDeviceDesc", CustomDeviceDesc);
            if (!DirectBackBuffer) yield return Entry("dxgi.directBackBuffer", "false");
            if (!string.IsNullOrEmpty(FloatEmulation)) yield return Entry("d3d9.floatEmulation", FloatEmulation);
            if (!string.IsNullOrEmpty(NanDepth)) yield return Entry("d3d9.nanDepth", NanDepth);
            if (FxcCompiler) yield return Entry("d3d9.shaderCompiler", "fxc");
            if (SubmitThread) yield return Entry("d3d12.submitThread", "true");
            if (RecordThread) yield return Entry("d3d12.recordThread", "true");
            if (!PixelOutputMasks) yield return Entry("d3d12.pixelOutputMasks", "false");
            if (!BarrierHoisting) yield return Entry("d3d12.barrierHoisting", "false");
            if (RelaxedUavBarriers.HasValue) yield return Entry("d3d11.relaxedUavBarriers", RelaxedUavBarriers.Value ? "true" : "false");
            if (LazyTargets.HasValue) yield return Entry("d3d12.lazyTargets", LazyTargets.Value ? "true" : "false");
        }

        static KeyValuePair<string, string> Entry(string key, string value) => new KeyValuePair<string, string>(key, value);

        static int? ParseInt(string value) =>
            int.TryParse(value.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed) ? parsed : (int?)null;

        static float? ParseFloat(string value) =>
            float.TryParse(value.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float parsed) ? parsed : (float?)null;

        // 1-4 hex digits (0x allowed) in lower case, as DXUP reads them; null otherwise.
        public static string ParseHex(string value)
        {
            string hex = value.Trim().ToLowerInvariant();
            if (hex.StartsWith("0x", StringComparison.Ordinal))
                hex = hex.Substring(2);
            if (hex.Length == 0 || hex.Length > 4 || !int.TryParse(hex, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out int id) || id == 0)
                return null;
            return hex;
        }

        static bool? ParseBool(string value)
        {
            switch (value.Trim().ToLowerInvariant())
            {
                case "true": case "yes": case "on": case "1": return true;
                case "false": case "no": case "off": case "0": return false;
                default: return null;
            }
        }
    }

    // A game DXUP is set up for: its executable, whether DXUP's DLLs are in
    // its folder, and its settings.
    public sealed class GameEntry
    {
        public string Name = "";
        public string Dir = "";
        public string Exe = "";
        public string AppId = "";
        public bool Enabled;
        // 32-bit games: the executable large address aware (4 GB) while DXUP
        // is in the folder (see LargeAddressAware).
        public bool LargeAddressAware;
        public GameSettings Settings = new GameSettings();
        // Lines DXUP or a newer manager knows, kept as they are.
        public List<KeyValuePair<string, string>> Extra = new List<KeyValuePair<string, string>>();

        // Hand-edited files may hold characters paths cannot.
        public string ExePath => Dir.TrimEnd('\\', '/') + "\\" + Exe;
    }

    // games.cfg in the DXUP installation: one [game] block per game, read
    // by DXUP's DLLs (the block whose dir is the folder they are in). UTF-8
    // without a byte order mark; '#' starts a comment only at the start of
    // a line, so paths may contain it.
    public static class GamesConfig
    {
        public const string FileName = "games.cfg";

        public static List<GameEntry> Parse(string text)
        {
            var games = new List<GameEntry>();
            GameEntry current = null;
            foreach (string raw in text.Replace("\r\n", "\n").Split('\n'))
            {
                string line = raw.Trim().TrimStart((char)0xFEFF);
                if (line.Length == 0 || line[0] == '#')
                    continue;
                if (line[0] == '[')
                {
                    if (line.Equals("[game]", StringComparison.OrdinalIgnoreCase))
                        games.Add(current = new GameEntry());
                    else
                        current = null;
                    continue;
                }
                int equals = line.IndexOf('=');
                if (current == null || equals < 0)
                    continue;
                string key = line.Substring(0, equals).Trim();
                string value = line.Substring(equals + 1).Trim();
                switch (key.ToLowerInvariant())
                {
                    case "name": current.Name = value; break;
                    case "dir": current.Dir = value; break;
                    case "exe": current.Exe = value; break;
                    case "appid": current.AppId = value; break;
                    case "enabled": current.Enabled = value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1"; break;
                    case "largeaddressaware": current.LargeAddressAware = value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1"; break;
                    default:
                        if (!current.Settings.Set(key, value))
                            current.Extra.Add(new KeyValuePair<string, string>(key, value));
                        break;
                }
            }
            games.RemoveAll(game => game.Dir.Length == 0);
            return games;
        }

        public static string Format(IEnumerable<GameEntry> games)
        {
            var text = new StringBuilder();
            text.Append("# DXUP settings per game, written by DXUP Manager.\n");
            text.Append("# DXUP's DLLs use the block whose dir is the folder they were copied to.\n");
            foreach (var game in games)
            {
                text.Append("\n[game]\n");
                Line(text, "name", game.Name);
                Line(text, "dir", game.Dir);
                Line(text, "exe", game.Exe);
                if (game.AppId.Length != 0)
                    Line(text, "appid", game.AppId);
                Line(text, "enabled", game.Enabled ? "true" : "false");
                if (game.LargeAddressAware)
                    Line(text, "largeAddressAware", "true");
                foreach (var entry in game.Settings.Entries())
                    Line(text, entry.Key, entry.Value);
                foreach (var entry in game.Extra)
                    Line(text, entry.Key, entry.Value);
            }
            return text.ToString();
        }

        static void Line(StringBuilder text, string key, string value)
        {
            // Values are single lines.
            text.Append(key).Append(" = ").Append(value.Replace("\r", "").Replace("\n", " ")).Append('\n');
        }

        public static List<GameEntry> Load(string path)
        {
            if (!File.Exists(path))
                return new List<GameEntry>();
            return Parse(File.ReadAllText(path, Encoding.UTF8));
        }

        // Written to a temporary file and swapped in, so a game starting
        // meanwhile reads either the old or the new file.
        public static void Save(string path, IEnumerable<GameEntry> games)
        {
            string temp = path + ".new";
            File.WriteAllText(temp, Format(games), new UTF8Encoding(false));
            if (File.Exists(path))
                File.Replace(temp, path, null);
            else
                File.Move(temp, path);
        }

        // A dxup.conf of the game's folder (it would take precedence over
        // games.cfg): its settings and lines DXUP knows otherwise.
        public static void Import(GameEntry game, string confText)
        {
            foreach (string raw in confText.Replace("\r\n", "\n").Split('\n'))
            {
                string line = raw;
                int comment = line.IndexOf('#');
                if (comment >= 0)
                    line = line.Substring(0, comment);
                int equals = line.IndexOf('=');
                if (equals < 0)
                    continue;
                string key = line.Substring(0, equals).Trim();
                string value = line.Substring(equals + 1).Trim();
                if (key.Length == 0 || value.Length == 0)
                    continue;
                if (!game.Settings.Set(key, value))
                    game.Extra.Add(new KeyValuePair<string, string>(key, value));
            }
        }
    }
}
