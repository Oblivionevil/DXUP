using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
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
        public double? RenderScale;      // percent of the asked back buffer size (33-99.99), null = all
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

        // Reads one setting as DXUP names it, its value without a comment
        // (GamesConfig.Parse, Import); false for keys it does not know.
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
                case "d3d12.lazytargets":
                    // As DXUP reads it: anything else is its automatic choice.
                    switch (value.Trim().ToLowerInvariant())
                    {
                        case "true": case "on": case "1": LazyTargets = true; break;
                        case "false": case "off": case "0": LazyTargets = false; break;
                        default: LazyTargets = null; break;
                    }
                    return true;
                case "dxgi.renderscale":
                    float? scale = ParseFloat(value);
                    // As DXUP reads it (at least 0.33), to a hundredth of a percent.
                    RenderScale = scale.HasValue && scale.Value < 1.0f ? (double?)Math.Max(33.0, Math.Round(scale.Value * 100.0, 2)) : null;
                    return true;
                default: return false;
            }
        }

        // The settings that differ from the game's own behaviour, as DXUP reads them.
        public IEnumerable<KeyValuePair<string, string>> Entries()
        {
            if (Anisotropy.HasValue) yield return Entry("d3d11.samplerAnisotropy", Anisotropy.Value.ToString(CultureInfo.InvariantCulture));
            // Floats with the digits they were written with (-0.333 stays so).
            if (LodBias.HasValue && LodBias.Value != 0.0f) yield return Entry("d3d11.samplerLodBias", LodBias.Value.ToString("0.#####", CultureInfo.InvariantCulture));
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
                yield return Entry("dxgi.renderScale", (RenderScale.Value / 100.0).ToString("0.######", CultureInfo.InvariantCulture));
            if (!string.IsNullOrEmpty(Adapter)) yield return Entry("dxgi.adapter", Adapter);
            if (!string.IsNullOrEmpty(SpoofVendor)) yield return Entry("dxgi.spoofVendor", SpoofVendor);
            if (SpatialSharpness.HasValue)
                yield return Entry("dxgi.upscaleSharpness", SpatialSharpness.Value.ToString("0.#####", CultureInfo.InvariantCulture));
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

        // DXUP reads a value up to a '#' (a comment): what a game sees.
        public static string BeforeComment(string value)
        {
            int comment = value.IndexOf('#');
            return (comment >= 0 ? value.Substring(0, comment) : value).Trim();
        }

        static int? ParseInt(string value) =>
            int.TryParse(value.Trim(), NumberStyles.Integer, CultureInfo.InvariantCulture, out int parsed) ? parsed : (int?)null;

        static float? ParseFloat(string value) =>
            float.TryParse(value.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out float parsed) ? parsed : (float?)null;

        // A GPU id as DXUP reads it: hex digits (0x allowed), the low 16 bits
        // (strtoul: too many digits give all ones), in lower case; null for
        // none or 0.
        public static string ParseHex(string value)
        {
            string hex = value.Trim().ToLowerInvariant();
            if (hex.StartsWith("0x", StringComparison.Ordinal))
                hex = hex.Substring(2);
            if (hex.Length == 0 || !hex.All(Uri.IsHexDigit))
                return null;
            ulong id = ulong.TryParse(hex, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out ulong parsed)
                && parsed <= uint.MaxValue ? parsed : uint.MaxValue;
            id &= 0xffff;
            return id != 0 ? id.ToString("x", CultureInfo.InvariantCulture) : null;
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
        // Comments and other lines of the block without a setting, as written.
        public List<string> Lines = new List<string>();

        // Hand-edited files may hold characters paths cannot.
        public string ExePath => Dir.TrimEnd('\\', '/') + "\\" + Exe;

        // Whether dir and exe can be used as paths: a hand-edited games.cfg
        // may hold characters they cannot (" < > | and control characters),
        // which the file functions refuse with an exception.
        public bool ValidPath =>
            Dir.Length != 0 && Dir.IndexOfAny(Path.GetInvalidPathChars()) < 0
            && Exe.IndexOfAny(Path.GetInvalidFileNameChars()) < 0;
    }

    // What games.cfg holds besides the games' blocks, kept as written when
    // DXUP Manager saves it: lines before the first section (the user's
    // comments), other sections and [game] blocks without a dir.
    public sealed class GamesDocument
    {
        public List<GameEntry> Games = new List<GameEntry>();
        public List<string> Head = new List<string>();
        public List<string> Tail = new List<string>();
    }

    // games.cfg in the DXUP installation: one [game] block per game, read
    // by DXUP's DLLs (the block whose dir is the folder they are in). UTF-8
    // without a byte order mark; '#' starts a comment only at the start of
    // a line, so paths may contain it.
    public static class GamesConfig
    {
        public const string FileName = "games.cfg";

        // The comment Format starts the file with (not kept as the user's).
        static readonly string[] s_header =
        {
            "# DXUP settings per game, written by DXUP Manager.",
            "# DXUP's DLLs use the block whose dir is the folder they were copied to.",
        };

        public static List<GameEntry> Parse(string text) => ParseDocument(text).Games;

        public static GamesDocument ParseDocument(string text)
        {
            var document = new GamesDocument();
            GameEntry current = null;
            List<string> block = null;     // the [game] block as written
            bool section = false;          // past the first section header
            foreach (string raw in text.Replace("\r\n", "\n").Split('\n'))
            {
                string written = raw.TrimEnd('\r').TrimStart((char)0xFEFF);
                string line = raw.Trim().TrimStart((char)0xFEFF).Trim();
                if (line.Length != 0 && line[0] == '[')
                {
                    EndBlock(document, current, block);
                    current = null;
                    block = null;
                    section = true;
                    if (line.Equals("[game]", StringComparison.OrdinalIgnoreCase))
                    {
                        current = new GameEntry();
                        block = new List<string> { written };
                    }
                    else
                    {
                        // Another program's section: DXUP's DLLs skip it.
                        document.Tail.Add(written);
                    }
                    continue;
                }
                if (!section)
                {
                    if (!s_header.Contains(line))
                        document.Head.Add(written);
                    continue;
                }
                if (current == null)
                {
                    document.Tail.Add(written);
                    continue;
                }
                block.Add(written);
                if (line.Length == 0)
                    continue;
                int equals = line.IndexOf('=');
                if (line[0] == '#' || equals < 0)
                {
                    current.Lines.Add(line);
                    continue;
                }
                string key = line.Substring(0, equals).Trim();
                string value = line.Substring(equals + 1).Trim();
                switch (key.ToLowerInvariant())
                {
                    case "name": current.Name = value; break;
                    case "dir": current.Dir = value; break;
                    case "exe": current.Exe = value; break;
                    case "appid": current.AppId = value; break;
                    case "enabled": current.Enabled = IsTrue(GameSettings.BeforeComment(value)); break;
                    case "largeaddressaware": current.LargeAddressAware = IsTrue(GameSettings.BeforeComment(value)); break;
                    default:
                        // DXUP reads the settings up to a '#' (name, dir and
                        // exe are taken whole). Unknown lines stay as written.
                        if (!current.Settings.Set(key, GameSettings.BeforeComment(value)))
                            current.Extra.Add(new KeyValuePair<string, string>(key, value));
                        break;
                }
            }
            EndBlock(document, current, block);
            Trim(document.Head, true);
            Trim(document.Tail, false);
            return document;
        }

        // A block with a dir is a game; one without stays as written.
        static void EndBlock(GamesDocument document, GameEntry game, List<string> block)
        {
            if (game == null)
                return;
            if (game.Dir.Length != 0)
                document.Games.Add(game);
            else
                document.Tail.AddRange(block);
        }

        // Without the blank lines Format puts between blocks.
        static void Trim(List<string> lines, bool start)
        {
            while (lines.Count != 0 && lines[lines.Count - 1].Trim().Length == 0)
                lines.RemoveAt(lines.Count - 1);
            while (start && lines.Count != 0 && lines[0].Trim().Length == 0)
                lines.RemoveAt(0);
        }

        static bool IsTrue(string value) => value.Equals("true", StringComparison.OrdinalIgnoreCase) || value == "1";

        // The games and, from the document they were read with, the lines
        // the manager does not know.
        public static string Format(IEnumerable<GameEntry> games, GamesDocument kept = null)
        {
            var text = new StringBuilder();
            foreach (string line in s_header)
                text.Append(line).Append('\n');
            foreach (string line in kept?.Head ?? Enumerable.Empty<string>())
                text.Append(line).Append('\n');
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
                foreach (string line in game.Lines)
                    text.Append(line.Replace("\r", "").Replace("\n", " ")).Append('\n');
            }
            if (kept != null && kept.Tail.Count != 0)
            {
                text.Append('\n');
                foreach (string line in kept.Tail)
                    text.Append(line).Append('\n');
            }
            return text.ToString();
        }

        static void Line(StringBuilder text, string key, string value)
        {
            // Values are single lines.
            text.Append(key).Append(" = ").Append(value.Replace("\r", "").Replace("\n", " ")).Append('\n');
        }

        public static List<GameEntry> Load(string path) => LoadDocument(path).Games;

        public static GamesDocument LoadDocument(string path)
        {
            if (!File.Exists(path))
                return new GamesDocument();
            return ParseDocument(File.ReadAllText(path, Encoding.UTF8));
        }

        // Written to a temporary file and swapped in, so a game starting
        // meanwhile reads either the old or the new file. kept: the
        // document the games were loaded with, whose other lines stay.
        public static void Save(string path, IEnumerable<GameEntry> games, GamesDocument kept = null)
        {
            string temp = path + ".new";
            File.WriteAllText(temp, Format(games, kept), new UTF8Encoding(false));
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
