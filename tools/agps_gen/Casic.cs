using System;
using System.Collections.Generic;
using System.IO;

namespace AgpsGen
{
    sealed class EncodeException : Exception
    {
        public EncodeException(string m) : base(m) { }
    }

    // Byte-exact port of gen_epo.py (encode_eph / decode_eph / encode_ion / casic_frame).
    static class Casic
    {
        public static readonly double[] UraLimits = new double[] { 2.4, 3.4, 4.85, 6.85, 9.65, 13.65, 24, 48, 96, 192, 384, 768, 1536, 3072, 6144 };

        // round half away from zero
        public static long Rnd(double x)
        {
            double v = Math.Floor(Math.Abs(x) + 0.5);
            if (double.IsNaN(v) || double.IsInfinity(v) || v > 9e15) throw new EncodeException("value out of range");
            return x >= 0 ? (long)v : -(long)v;
        }

        static int UraIndex(double m)
        {
            for (int n = 0; n < UraLimits.Length; n++) if (m <= UraLimits[n]) return n;
            return 15;
        }

        static void CheckS(long v, int bits, string name)
        {
            long lim = 1L << (bits - 1);
            if (v < -lim || v >= lim) throw new EncodeException(name + " does not fit in i" + bits);
        }

        static void CheckU(long v, int bits, string name)
        {
            if (v < 0 || v >= (1L << bits)) throw new EncodeException(name + " does not fit in u" + bits);
        }

        // 32-bit angular field (semicircles): a value that rounds to +2^31 is the same angle as -2^31
        static int Ang32(long v, string name)
        {
            if (v == (1L << 31)) v = -(1L << 31);
            CheckS(v, 32, name);
            return (int)v;
        }

        static double FloorMod(double a, double m) { return a - m * Math.Floor(a / m); }

        public static byte[] EncodeEph(BcRec r)
        {
            double sc = 1.0 / K.GPS_PI;
            MemoryStream ms = new MemoryStream(72);
            BinaryWriter w = new BinaryWriter(ms);
            long v;
            w.Write((uint)0);
            v = Rnd(r.SqrtA * Math.Pow(2, 19)); CheckU(v, 32, "sqrtA"); w.Write((uint)v);
            v = Rnd(r.E * Math.Pow(2, 33)); CheckU(v, 32, "e"); w.Write((uint)v);
            w.Write(Ang32(Rnd(r.Omega * sc * Math.Pow(2, 31)), "omega"));
            w.Write(Ang32(Rnd(r.M0 * sc * Math.Pow(2, 31)), "M0"));
            w.Write(Ang32(Rnd(r.I0 * sc * Math.Pow(2, 31)), "i0"));
            w.Write(Ang32(Rnd(r.Omega0 * sc * Math.Pow(2, 31)), "Omega0"));
            v = Rnd(r.OmegaDot * sc * Math.Pow(2, 43)); CheckS(v, 32, "OmegaDot"); w.Write((int)v);
            v = Rnd(r.DeltaN * sc * Math.Pow(2, 43)); CheckS(v, 16, "DeltaN"); w.Write((short)v);
            v = Rnd(r.Idot * sc * Math.Pow(2, 43)); CheckS(v, 16, "IDOT"); w.Write((short)v);
            v = Rnd(r.Cuc * Math.Pow(2, 29)); CheckS(v, 16, "Cuc"); w.Write((short)v);
            v = Rnd(r.Cus * Math.Pow(2, 29)); CheckS(v, 16, "Cus"); w.Write((short)v);
            v = Rnd(r.Crc * Math.Pow(2, 5)); CheckS(v, 16, "Crc"); w.Write((short)v);
            v = Rnd(r.Crs * Math.Pow(2, 5)); CheckS(v, 16, "Crs"); w.Write((short)v);
            v = Rnd(r.Cic * Math.Pow(2, 29)); CheckS(v, 16, "Cic"); w.Write((short)v);
            v = Rnd(r.Cis * Math.Pow(2, 29)); CheckS(v, 16, "Cis"); w.Write((short)v);
            v = Rnd(r.Toe / 16); CheckU(v, 16, "toe"); w.Write((ushort)v);
            w.Write((ushort)(((long)r.Week) % 1024));
            v = Rnd(r.Toc / 16); CheckU(v, 32, "toc"); w.Write((uint)v);
            v = Rnd(r.Af0 * Math.Pow(2, 31)); CheckS(v, 32, "af0"); w.Write((int)v);
            v = Rnd(r.Af1 * Math.Pow(2, 43)); CheckS(v, 16, "af1"); w.Write((short)v);
            v = Rnd(r.Af2 * Math.Pow(2, 55)); CheckS(v, 8, "af2"); w.Write((sbyte)v);
            v = Rnd(r.Tgd * Math.Pow(2, 31)); CheckS(v, 8, "tgd"); w.Write((sbyte)v);
            v = (long)r.Iodc; CheckU(v, 16, "iodc"); w.Write((ushort)v);
            w.Write((byte)UraIndex(r.UraM));
            w.Write((byte)(int)r.Health);
            w.Write((byte)r.Svid);
            w.Write((byte)3);
            w.Write((ushort)0);
            w.Flush();
            byte[] b = ms.ToArray();
            if (b.Length != 72) throw new InvalidOperationException("EPH payload is not 72 bytes");
            return b;
        }

        // payload -> physical values; ToeAbs is left 0 (caller supplies the full week)
        public static BcRec DecodeEph(byte[] pl)
        {
            BinaryReader rd = new BinaryReader(new MemoryStream(pl));
            BcRec r = new BcRec();
            double pi = K.GPS_PI;
            rd.ReadUInt32();
            r.SqrtA = rd.ReadUInt32() / Math.Pow(2, 19);
            r.E = rd.ReadUInt32() / Math.Pow(2, 33);
            r.Omega = rd.ReadInt32() / Math.Pow(2, 31) * pi;
            r.M0 = rd.ReadInt32() / Math.Pow(2, 31) * pi;
            r.I0 = rd.ReadInt32() / Math.Pow(2, 31) * pi;
            r.Omega0 = rd.ReadInt32() / Math.Pow(2, 31) * pi;
            r.OmegaDot = rd.ReadInt32() / Math.Pow(2, 43) * pi;
            r.DeltaN = rd.ReadInt16() / Math.Pow(2, 43) * pi;
            r.Idot = rd.ReadInt16() / Math.Pow(2, 43) * pi;
            r.Cuc = rd.ReadInt16() / Math.Pow(2, 29);
            r.Cus = rd.ReadInt16() / Math.Pow(2, 29);
            r.Crc = rd.ReadInt16() / Math.Pow(2, 5);
            r.Crs = rd.ReadInt16() / Math.Pow(2, 5);
            r.Cic = rd.ReadInt16() / Math.Pow(2, 29);
            r.Cis = rd.ReadInt16() / Math.Pow(2, 29);
            r.Toe = rd.ReadUInt16() * 16.0;
            r.Week = rd.ReadUInt16();
            r.Toc = rd.ReadUInt32() * 16.0;
            r.Af0 = rd.ReadInt32() / Math.Pow(2, 31);
            r.Af1 = rd.ReadInt16() / Math.Pow(2, 43);
            r.Af2 = rd.ReadSByte() / Math.Pow(2, 55);
            r.Tgd = rd.ReadSByte() / Math.Pow(2, 31);
            r.Iodc = rd.ReadUInt16();
            int ura = rd.ReadByte();
            r.UraM = UraLimits[Math.Min(ura, 14)];
            r.Health = rd.ReadByte();
            r.Svid = rd.ReadByte();
            return r;
        }

        static void CheckS8(double x, int bits, string name)
        {
            long v = Rnd(x);
            CheckS(v, bits, name);
        }

        public static byte[] EncodeIon(double[] a, double[] b)
        {
            MemoryStream ms = new MemoryStream(16);
            BinaryWriter w = new BinaryWriter(ms);
            w.Write((uint)0);
            double[] raw = new double[] {
                a[0] * Math.Pow(2, 30), a[1] * Math.Pow(2, 27), a[2] * Math.Pow(2, 24), a[3] * Math.Pow(2, 24),
                b[0] / Math.Pow(2, 11), b[1] / Math.Pow(2, 14), b[2] / Math.Pow(2, 16), b[3] / Math.Pow(2, 16) };
            for (int i = 0; i < 8; i++)
            {
                long v = Rnd(raw[i]);
                CheckS(v, 8, "ionosphere term " + i);
                w.Write((sbyte)v);
            }
            w.Write((byte)3);
            w.Write((byte)0);
            w.Write((ushort)0);
            w.Flush();
            return ms.ToArray();
        }

        public static uint Checksum(int cls, int mid, byte[] payload)
        {
            uint s = (uint)((mid << 24) + (cls << 16) + payload.Length);
            for (int i = 0; i < payload.Length; i += 4) s += BitConverter.ToUInt32(payload, i);
            return s;
        }

        public static byte[] Frame(int cls, int mid, byte[] payload)
        {
            if (payload.Length % 4 != 0) throw new InvalidOperationException("payload not a multiple of 4");
            byte[] f = new byte[payload.Length + 10];
            f[0] = 0xBA; f[1] = 0xCE;
            f[2] = (byte)(payload.Length & 0xFF); f[3] = (byte)(payload.Length >> 8);
            f[4] = (byte)cls; f[5] = (byte)mid;
            Array.Copy(payload, 0, f, 6, payload.Length);
            uint ck = Checksum(cls, mid, payload);
            f[6 + payload.Length] = (byte)ck; f[7 + payload.Length] = (byte)(ck >> 8);
            f[8 + payload.Length] = (byte)(ck >> 16); f[9 + payload.Length] = (byte)(ck >> 24);
            return f;
        }

        public static double WrapPi(double a) { return FloorMod(a + Math.PI, 2 * Math.PI) - Math.PI; }
    }
}
