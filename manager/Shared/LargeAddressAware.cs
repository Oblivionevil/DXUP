using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Dxup.Shared
{
    // A 32-bit program gets 2 GB of address space unless its executable is
    // marked large address aware (IMAGE_FILE_LARGE_ADDRESS_AWARE in the file
    // header): then 4 GB on 64-bit Windows. Direct3D 12 takes more of it than
    // Direct3D 9 did (its runtime and the driver), so a game that came close
    // to 2 GB with Windows' Direct3D 9 can run out with DXUP: textures and
    // buffers fail. DXUP Manager sets the bit on request. The original stays
    // next to the executable as *.dxup-laa-backup while the bit is DXUP's:
    // without that file the executable is as the game brought it.
    public static class LargeAddressAware
    {
        public const string BackupSuffix = ".dxup-laa-backup";
        const ushort Flag = 0x0020;

        // Whether the 32-bit executable has the bit; null when it is no
        // 32-bit executable (or cannot be read).
        public static bool? Get(string exe)
        {
            try
            {
                using (var stream = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    var header = Header.Read(stream);
                    return header != null && header.Machine == PeFile.MachineX86 ? (header.Characteristics & Flag) != 0 : (bool?)null;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        // Whether DXUP set the bit (its backup of the original is there).
        public static bool SetByDxup(string exe) => File.Exists(exe + BackupSuffix);

        // Steam's DRM wrapper (SteamStub, its .bind section) refuses to start
        // a changed executable: "Application load error 3:0000065432"
        // (Overlord II).
        public static bool SteamDrm(string exe)
        {
            try
            {
                using (var stream = new FileStream(exe, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                    return Header.Read(stream)?.Sections.Contains(".bind") == true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // Whether the game gets the bit from DXUP: asked for, and possible.
        public static bool Wanted(GameEntry game) =>
            game.LargeAddressAware && !string.IsNullOrEmpty(game.Exe) && !SteamDrm(game.ExePath);

        // Sets the bit; the executable as it was goes to the backup first.
        // Nothing to do when the game has it itself.
        public static void Enable(string exe)
        {
            if (Get(exe) != false)
                return;
            if (SteamDrm(exe))
                throw new InvalidOperationException("Steam DRM: the game would not start with a changed executable");
            string backup = exe + BackupSuffix;
            // An update of the game replaced the executable: that is the original now.
            File.Copy(exe, backup, true);
            try
            {
                Write(exe, true);
            }
            catch (Exception)
            {
                File.Delete(backup);
                throw;
            }
        }

        // Undoes the bit DXUP set and removes the backup: the backup goes
        // back when the executable is still DXUP's change of it. Enable
        // backs up whatever executable it changes, so another executable
        // with the bit came with it (an update made it large address
        // aware): it stays as it is.
        public static void Disable(string exe)
        {
            string backup = exe + BackupSuffix;
            if (!File.Exists(backup))
                return;
            if (File.Exists(exe) && Get(exe) == true && Get(backup) != true && ChangedHeaderOnly(exe, backup))
                File.Copy(backup, exe, true);
            File.Delete(backup);
        }

        // Whether the files differ in the header's characteristics and
        // checksum at most.
        static bool ChangedHeaderOnly(string exe, string backup)
        {
            byte[] a = File.ReadAllBytes(exe), b = File.ReadAllBytes(backup);
            if (a.Length != b.Length)
                return false;
            Header header;
            using (var memory = new MemoryStream(b))
                header = Header.Read(memory);
            if (header == null)
                return false;
            for (int i = 0; i < a.Length; i++)
            {
                bool field = (i >= header.CharacteristicsOffset && i < header.CharacteristicsOffset + 2)
                    || (i >= header.CheckSumOffset && i < header.CheckSumOffset + 4);
                if (a[i] != b[i] && !field)
                    return false;
            }
            return true;
        }

        // Sets or clears the bit, and the header's checksum when it was right
        // (Windows checks it for drivers only; signed files can carry one
        // computed before signing, which stays as it is).
        static void Write(string exe, bool on)
        {
            byte[] bytes = File.ReadAllBytes(exe);
            using (var memory = new MemoryStream(bytes))
            {
                var header = Header.Read(memory);
                if (header == null || header.Machine != PeFile.MachineX86)
                    throw new InvalidDataException(exe + ": no 32-bit executable");
                uint stored = BitConverter.ToUInt32(bytes, header.CheckSumOffset);
                bool valid = stored != 0 && stored == CheckSum(bytes, header.CheckSumOffset);
                ushort characteristics = (ushort)(on ? header.Characteristics | Flag : header.Characteristics & ~Flag);
                BitConverter.GetBytes(characteristics).CopyTo(bytes, header.CharacteristicsOffset);
                if (valid)
                    BitConverter.GetBytes(CheckSum(bytes, header.CheckSumOffset)).CopyTo(bytes, header.CheckSumOffset);
            }
            using (var stream = new FileStream(exe, FileMode.Open, FileAccess.Write, FileShare.Read))
                stream.Write(bytes, 0, bytes.Length);
        }

        // CheckSumMappedFile's sum: 16-bit words with the carry folded back,
        // the checksum field counted as 0, plus the file's length.
        public static uint CheckSum(byte[] bytes, int checkSumOffset)
        {
            ulong sum = 0;
            int length = bytes.Length;
            for (int i = 0; i + 1 < length; i += 2)
            {
                if (i == checkSumOffset || i == checkSumOffset + 2)
                    continue;
                sum += (uint)(bytes[i] | bytes[i + 1] << 8);
                sum = (sum & 0xffff) + (sum >> 16);
            }
            if ((length & 1) != 0)
            {
                sum += bytes[length - 1];
                sum = (sum & 0xffff) + (sum >> 16);
            }
            sum = (sum & 0xffff) + (sum >> 16);
            return (uint)(sum + (ulong)length);
        }

        // Whether both files hold the same bytes.
        public static bool SameContent(string a, string b)
        {
            var infoA = new FileInfo(a);
            var infoB = new FileInfo(b);
            return infoA.Exists && infoB.Exists && infoA.Length == infoB.Length
                && File.ReadAllBytes(a).SequenceEqual(File.ReadAllBytes(b));
        }

        sealed class Header
        {
            public ushort Machine;
            public ushort Characteristics;
            public int CharacteristicsOffset;
            public int CheckSumOffset;
            public readonly List<string> Sections = new List<string>();

            public static Header Read(Stream stream)
            {
                var reader = new BinaryReader(stream);
                if (stream.Length < 0x40 || reader.ReadUInt16() != 0x5a4d)
                    return null;
                stream.Position = 0x3c;
                uint peOffset = reader.ReadUInt32();
                // Signature, file header (20 bytes), the optional header up to its checksum.
                if (peOffset + 24 + 68 > stream.Length)
                    return null;
                stream.Position = peOffset;
                if (reader.ReadUInt32() != 0x00004550)
                    return null;
                var header = new Header { Machine = reader.ReadUInt16() };
                header.CharacteristicsOffset = (int)peOffset + 22;
                header.CheckSumOffset = (int)peOffset + 24 + 64;
                ushort sectionCount = reader.ReadUInt16();
                stream.Position = peOffset + 20;
                ushort optionalSize = reader.ReadUInt16();
                header.Characteristics = reader.ReadUInt16();
                long sections = peOffset + 24 + optionalSize;
                for (int i = 0; i < sectionCount && sections + 40 * (i + 1) <= stream.Length; i++)
                {
                    stream.Position = sections + 40 * i;
                    header.Sections.Add(Encoding.ASCII.GetString(reader.ReadBytes(8)).TrimEnd('\0'));
                }
                return header;
            }
        }
    }
}
