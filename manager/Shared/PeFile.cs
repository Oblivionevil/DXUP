using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Dxup.Shared
{
    // What the headers of an executable tell: its machine and the DLLs it
    // imports (normal and delay-loaded).
    public sealed class PeFile
    {
        public const ushort MachineX64 = 0x8664;
        public const ushort MachineX86 = 0x014c;
        public const ushort MachineArm64 = 0xaa64;

        public ushort Machine { get; private set; }
        public List<string> Imports { get; } = new List<string>();

        public bool Is64Bit => Machine == MachineX64;
        public bool Is32Bit => Machine == MachineX86;
        public bool ImportsDirect3D11 => Imports.Exists(name => IsAny(name, "d3d11.dll", "dxgi.dll"));
        public bool ImportsD3D11Dll => Imports.Exists(name => IsAny(name, "d3d11.dll"));
        // Direct3D 10 runs on DXUP's D3D11 (Windows' d3d10.dll creates its devices there).
        public bool ImportsD3D10Dll => Imports.Exists(name => IsAny(name, "d3d10.dll", "d3d10_1.dll"));
        public bool ImportsD3D12Dll => Imports.Exists(name => IsAny(name, "d3d12.dll"));
        public bool ImportsDll(string dll) => Imports.Exists(name => IsAny(name, dll));
        // Direct3D 7 to 9.
        public bool ImportsLegacyDirect3D => Imports.Exists(name => IsAny(name, "ddraw.dll", "d3d8.dll", "d3d9.dll"));
        public bool ImportsDirect3D12Only => Imports.Exists(name => IsAny(name, "d3d12.dll")) && !ImportsDirect3D11;

        static bool IsAny(string name, params string[] names)
        {
            foreach (string candidate in names)
            {
                if (string.Equals(name, candidate, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            return false;
        }

        // Null when the file is not a PE executable.
        public static PeFile Read(string path)
        {
            try
            {
                using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var reader = new BinaryReader(stream))
                    return Read(reader);
            }
            catch (Exception)
            {
                return null;
            }
        }

        public static PeFile Read(BinaryReader reader)
        {
            var stream = reader.BaseStream;
            if (stream.Length < 0x40 || reader.ReadUInt16() != 0x5a4d)
                return null;
            stream.Position = 0x3c;
            uint peOffset = reader.ReadUInt32();
            if (peOffset + 24 > stream.Length)
                return null;
            stream.Position = peOffset;
            if (reader.ReadUInt32() != 0x00004550)
                return null;
            var pe = new PeFile { Machine = reader.ReadUInt16() };
            ushort sectionCount = reader.ReadUInt16();
            stream.Position += 12;
            ushort optionalSize = reader.ReadUInt16();
            stream.Position += 2;
            long optionalStart = stream.Position;
            ushort magic = reader.ReadUInt16();
            bool pe32Plus = magic == 0x20b;
            if (magic != 0x10b && !pe32Plus)
                return pe;
            long directories = optionalStart + (pe32Plus ? 112 : 96);
            stream.Position = directories - 4;
            uint directoryCount = reader.ReadUInt32();
            var dirs = new uint[16 * 2];
            for (int i = 0; i < Math.Min(16u, directoryCount) * 2; i++)
                dirs[i] = reader.ReadUInt32();

            // Sections map relative addresses to file offsets.
            stream.Position = optionalStart + optionalSize;
            var sections = new List<(uint va, uint size, uint raw)>();
            for (int i = 0; i < sectionCount; i++)
            {
                stream.Position += 8;
                uint virtualSize = reader.ReadUInt32();
                uint virtualAddress = reader.ReadUInt32();
                uint rawSize = reader.ReadUInt32();
                uint rawPointer = reader.ReadUInt32();
                stream.Position += 16;
                sections.Add((virtualAddress, Math.Max(virtualSize, rawSize), rawPointer));
            }
            long Offset(uint rva)
            {
                foreach (var section in sections)
                {
                    if (rva >= section.va && rva < section.va + section.size)
                        return rva - section.va + section.raw;
                }
                return -1;
            }
            string Name(uint rva)
            {
                long offset = Offset(rva);
                if (offset < 0 || offset >= stream.Length)
                    return null;
                stream.Position = offset;
                var bytes = new List<byte>();
                for (int b; bytes.Count < 256 && (b = stream.ReadByte()) > 0;)
                    bytes.Add((byte)b);
                return Encoding.ASCII.GetString(bytes.ToArray());
            }

            // Import descriptors (20 bytes, name at +12) and delay-load
            // descriptors (32 bytes, name at +4).
            void Walk(uint rva, int size, int nameOffset)
            {
                long offset = Offset(rva);
                for (int i = 0; offset >= 0 && i < 1024; i++)
                {
                    stream.Position = offset + i * size + nameOffset;
                    if (stream.Position + 4 > stream.Length)
                        break;
                    uint nameRva = reader.ReadUInt32();
                    if (nameRva == 0)
                        break;
                    string name = Name(nameRva);
                    if (!string.IsNullOrEmpty(name))
                        pe.Imports.Add(name);
                }
            }
            if (dirs[1 * 2] != 0)
                Walk(dirs[1 * 2], 20, 12);
            if (dirs[13 * 2] != 0)
                Walk(dirs[13 * 2], 32, 4);
            return pe;
        }
    }
}
