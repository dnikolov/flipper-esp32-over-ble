using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace AgpsGen
{
    sealed class Options
    {
        public string Out;
        public string Cache;
        public int Days = 7;
        public string NowIso;
        public bool TruthCheck;
        public bool Offline;
        public string Source = "auto";
        public string BkgBase, WuhanBase, CompareNav, CompareBin;
        public string Csv;
        public string DumpDir;
        public double Step = 30.0;
        public double FitDays = 2.0;
        public double SvRmsLimit = 60.0;
        public double WinRmsLimit = 20.0;
        public int MinSvPerWindow = 8;
        public List<int> Svs;
        public string SelfTestProp;
        public string SelfTestBc;
        public string TrajIn;
    }

    sealed class SvResult
    {
        public int Sv;
        public string Excluded;
        public byte[][] Payload;
        public BcRec[] Rec;
        public double[] WinRms, WinMax;
        public string[] WinNote;
        public OrbitResult Orb;
        public double Seconds;
    }

    static class Program
    {
        static readonly object LogLock = new object();
        static Stopwatch clock = Stopwatch.StartNew();

        static void Log(string s)
        {
            lock (LogLock) { Console.Out.WriteLine(s); Console.Out.Flush(); }
        }

        static int Usage(string err)
        {
            if (err != null) Console.Error.WriteLine("agps_gen: " + err);
            Console.Error.WriteLine("usage: agps_gen.exe --out <path> [--days 7] [--cache <dir>] [--offline] [--source auto|bkg|wuhan]");
            Console.Error.WriteLine("       developer: [--bkg-base <url>] [--wuhan-base <url>] [--now <iso utc>] [--truth-check] [--csv <file>] [--dump-dir <dir>] [--svs 1,2,..]");
            Console.Error.WriteLine("                  [--step <s>] [--selftest-prop <file>] [--selftest-bc <sv>:<doy>]");
            return 2;
        }

        static int Main(string[] args)
        {
            System.Globalization.CultureInfo.DefaultThreadCurrentCulture = CultureInfo.InvariantCulture;
            System.Threading.Thread.CurrentThread.CurrentCulture = CultureInfo.InvariantCulture;
            Options o = new Options();
            try
            {
                for (int i = 0; i < args.Length; i++)
                {
                    string a = args[i];
                    Func<string> val = delegate() { if (i + 1 >= args.Length) throw new ArgumentException("missing value for " + a); i++; return args[i]; };
                    if (a == "--out") o.Out = val();
                    else if (a == "--cache") o.Cache = val();
                    else if (a == "--days") o.Days = int.Parse(val(), CultureInfo.InvariantCulture);
                    else if (a == "--now") o.NowIso = val();
                    else if (a == "--truth-check") o.TruthCheck = true;
                    else if (a == "--offline") o.Offline = true;
                    else if (a == "--source") { o.Source = val(); if (o.Source != "auto" && o.Source != "bkg" && o.Source != "wuhan") throw new ArgumentException("--source must be auto, bkg or wuhan"); }
                    else if (a == "--compare-nav") o.CompareNav = val();
                    else if (a == "--compare-bin") o.CompareBin = val();
                    else if (a == "--bkg-base") o.BkgBase = val();
                    else if (a == "--wuhan-base") o.WuhanBase = val();
                    else if (a == "--csv") o.Csv = val();
                    else if (a == "--dump-dir") o.DumpDir = val();
                    else if (a == "--step") o.Step = double.Parse(val(), CultureInfo.InvariantCulture);
                    else if (a == "--fit-days") o.FitDays = double.Parse(val(), CultureInfo.InvariantCulture);
                    else if (a == "--sv-rms-limit") o.SvRmsLimit = double.Parse(val(), CultureInfo.InvariantCulture);
                    else if (a == "--svs") o.Svs = val().Split(',').Select(delegate(string s) { return int.Parse(s, CultureInfo.InvariantCulture); }).ToList();
                    else if (a == "--selftest-prop") o.SelfTestProp = val();
                    else if (a == "--selftest-bc") o.SelfTestBc = val();
                    else if (a == "--traj-in") o.TrajIn = val();
                    else if (a == "--help" || a == "-h" || a == "/?") return Usage(null);
                    else return Usage("unknown argument " + a);
                }
            }
            catch (Exception ex) { return Usage(ex.Message); }

            try
            {
                if (o.SelfTestProp != null) return SelfTest.RunProp(o);
                if (o.SelfTestBc != null) return SelfTest.RunBc(o);
                if (o.CompareBin != null) return SelfTest.RunCompareBin(o);
                if (o.CompareNav != null)
                {
                    if (o.Cache == null) o.Cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "agps_gen", "cache");
                    return SelfTest.RunCompare(o);
                }
                if (o.Out == null && !o.TruthCheck) return Usage("--out is required");
                if (o.Days < 1 || o.Days > 14) return Usage("--days must be 1..14");
                if (o.Cache == null)
                    o.Cache = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "agps_gen", "cache");
                return Generate(o);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("agps_gen: error: " + ex.Message);
                if (Environment.GetEnvironmentVariable("AGPS_GEN_TRACE") == "1") Console.Error.WriteLine(ex.ToString());
                return 1;
            }
        }

        public static DateTime UtcFromUnix(long u) { return new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddSeconds(u); }
        public static long UnixFromUtc(DateTime d) { return (long)Math.Floor((d - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds); }
        public static string Iso(long u) { return UtcFromUnix(u).ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture); }

        static DateTime DayOf(long unix) { return UtcFromUnix(unix).Date; }

        static double Percentile(List<double> v, double p)
        {
            if (v.Count == 0) return double.NaN;
            double[] a = v.OrderBy(delegate(double x) { return x; }).ToArray();
            double pos = (a.Length - 1) * p / 100.0;
            int lo = (int)Math.Floor(pos), hi = (int)Math.Ceiling(pos);
            return a[lo] + (a[hi] - a[lo]) * (pos - lo);
        }

        static int Generate(Options o)
        {
            Stopwatch total = Stopwatch.StartNew();
            long nowUnix;
            if (o.NowIso != null)
            {
                DateTime d;
                if (!DateTime.TryParseExact(o.NowIso, new string[] { "yyyy-MM-dd'T'HH:mm:ss'Z'", "yyyy-MM-dd'T'HH:mm:ss", "yyyy-MM-dd HH:mm:ss", "yyyy-MM-dd'T'HH:mm'Z'" },
                    CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out d))
                    throw new ArgumentException("cannot parse --now " + o.NowIso);
                nowUnix = UnixFromUtc(d);
            }
            else nowUnix = UnixFromUtc(DateTime.UtcNow);
            double nowGps = (double)(nowUnix - K.GPS_UNIX0 + K.LEAP);
            const double WinSec = 14400.0;
            double k0 = Math.Floor(nowGps / WinSec);
            double T0 = WinSec * k0;
            int nWin = o.Days * 6;
            double fitW = Math.Round(o.FitDays * 86400.0 / 900.0) * 900.0;
            Log("now " + Iso(nowUnix) + " (GPS " + nowGps.ToString("0", CultureInfo.InvariantCulture) + " s); first window starts at GPS " +
                T0.ToString("0", CultureInfo.InvariantCulture) + " = " + Iso((long)T0 + K.GPS_UNIX0 - K.LEAP) + ", " + nWin + " windows");

            // ------------------------------------------------------------------ data
            NavSource src = new NavSource(o.Cache, o.Offline, Log);
            src.Mode = o.Source;
            if (o.BkgBase != null) src.BkgBase = o.BkgBase;
            if (o.WuhanBase != null) src.WuhanBase = o.WuhanBase;
            DateTime firstDay = DayOf((long)(T0 - fitW - 3 * 3600.0) + K.GPS_UNIX0 - K.LEAP);
            DateTime lastDay = DayOf(nowUnix);
            DateTime lastTruthDay = o.TruthCheck ? DayOf((long)(T0 + nWin * WinSec + 7200) + K.GPS_UNIX0 - K.LEAP) : lastDay;
            if (lastTruthDay < lastDay) lastTruthDay = lastDay;
            List<DateTime> days = new List<DateTime>();
            for (DateTime d = firstDay; d <= lastTruthDay; d = d.AddDays(1)) days.Add(d);
            Log("[1/5] navigation data: " + days.Count + " UTC days " + firstDay.ToString("yyyy-MM-dd") + " .. " + lastTruthDay.ToString("yyyy-MM-dd"));
            List<BcRec>[] perDay = new List<BcRec>[days.Count];
            string[] dayErr = new string[days.Count];
            src.LoadDays(days, lastDay, UtcFromUnix(nowUnix), out perDay, out dayErr);
            for (int i = 0; i < days.Count; i++)
            {
                if (perDay[i] != null) continue;
                Log("  " + days[i].ToString("yyyy-MM-dd") + ": not available (" + dayErr[i] + ") -- continuing without it");
            }
            Dictionary<string, int> index = new Dictionary<string, int>();
            List<BcRec> merged = new List<BcRec>();
            for (int i = 0; i < days.Count; i++)
            {
                if (perDay[i] == null) continue;
                foreach (BcRec r in perDay[i])
                {
                    string key = r.Svid + "|" + ((long)r.ToeAbs) + "|" + ((long)r.Iodc);
                    int at;
                    if (index.TryGetValue(key, out at)) merged[at] = r; else { index[key] = merged.Count; merged.Add(r); }
                }
            }
            SortedDictionary<int, List<BcRec>> bySv = new SortedDictionary<int, List<BcRec>>();
            foreach (BcRec r in merged)
            {
                List<BcRec> l;
                if (!bySv.TryGetValue(r.Svid, out l)) { l = new List<BcRec>(); bySv[r.Svid] = l; }
                l.Add(r);
            }
            foreach (int s in bySv.Keys.ToList()) bySv[s] = bySv[s].OrderBy(delegate(BcRec x) { return x.ToeAbs; }).ToList();

            Iono iono = null;
            string ionoNote = "";
            try { iono = src.LoadIono(lastDay, 10); ionoNote = iono.Source; }
            catch (NavException ex)
            {
                if (!o.TruthCheck || o.Out != null) throw;
                ionoNote = "none (" + ex.Message + ")";
            }
            Log("  ionosphere from " + ionoNote);

            // ------------------------------------------------------------------ orbit fit + window fit
            double horizon = nWin * WinSec + 7200.0;
            double step = o.Step;
            if (Math.Abs(900.0 / (step / 2.0) - Math.Round(900.0 / (step / 2.0))) > 1e-9) throw new ArgumentException("--step must divide 1800 s");
            double gridBase = T0 - fitW - 900.0;
            double gridDt = step / 2.0;
            int gridN = (int)Math.Ceiling((T0 + horizon - gridBase) / gridDt) + 4;
            Log("[2/5] Sun/Moon grid " + gridN + " nodes, RK4 step " + step + " s");
            SunMoonGrid grid = new SunMoonGrid(gridBase, gridDt, gridN);

            List<int> svList = new List<int>();
            foreach (int s in bySv.Keys) if (s >= 1 && s <= 32 && (o.Svs == null || o.Svs.Contains(s))) svList.Add(s);
            Log("[3/5] fitting " + svList.Count + " satellites (" + Environment.ProcessorCount + " threads)");
            SvResult[] results = new SvResult[svList.Count];
            Stopwatch fitClock = Stopwatch.StartNew();
            int done = 0;
            Parallel.For(0, svList.Count, delegate(int idx)
            {
                SvResult r = FitSv(svList[idx], bySv[svList[idx]], T0, nowGps, nWin, fitW, step, grid, o);
                results[idx] = r;
                int n = System.Threading.Interlocked.Increment(ref done);
                int okw = r.Payload == null ? 0 : r.Payload.Count(delegate(byte[] b) { return b != null; });
                Log(string.Format(CultureInfo.InvariantCulture, "  G{0:00} done ({1}/{2}) {3:0.0}s: {4}", r.Sv, n, svList.Count, r.Seconds,
                    r.Excluded != null ? "EXCLUDED - " + r.Excluded : (okw + "/" + nWin + " windows, fit rms " + r.Orb.FitRmsM.ToString("0.00", CultureInfo.InvariantCulture) + " m, arc " + (r.Orb.ArcSeconds / 3600.0).ToString("0") + " h, " + r.Orb.Iterations + " LM iters")));
            });
            fitClock.Stop();

            // ------------------------------------------------------------------ assemble
            Log("[4/5] assembling " + nWin + " windows");
            List<SvResult> used = results.Where(delegate(SvResult r) { return r.Excluded == null; }).ToList();
            List<string> excl = new List<string>();
            foreach (SvResult r in results) if (r.Excluded != null) excl.Add("G" + r.Sv.ToString("00") + ": " + r.Excluded);
            byte[] ionFrame = iono == null ? new byte[0] : Casic.Frame(0x08, 0x06, Casic.EncodeIon(iono.A, iono.B));
            List<byte[]> winBytes = new List<byte[]>();
            int minSv = int.MaxValue, maxSv = 0;
            for (int k = 0; k < nWin; k++)
            {
                MemoryStream ms = new MemoryStream();
                int cnt = 0;
                foreach (SvResult r in used)
                {
                    if (r.Payload[k] == null) continue;
                    byte[] f = Casic.Frame(0x08, 0x07, r.Payload[k]);
                    ms.Write(f, 0, f.Length); cnt++;
                }
                ms.Write(ionFrame, 0, ionFrame.Length);
                minSv = Math.Min(minSv, cnt); maxSv = Math.Max(maxSv, cnt);
                if (ms.Length > 4096) throw new InvalidOperationException("window " + k + " is " + ms.Length + " bytes (> 4096)");
                winBytes.Add(ms.ToArray());
            }
            Log("  satellites per window: min " + minSv + ", max " + maxSv);
            foreach (string e in excl) Log("  excluded " + e);
            int nwinFail = 0;
            foreach (SvResult r in used) for (int k = 0; k < nWin; k++) if (r.Payload[k] == null) { nwinFail++; Log("  G" + r.Sv.ToString("00") + " window " + k + " dropped: " + r.WinNote[k]); }

            if (o.DumpDir != null) DumpDebug(o.DumpDir, results, T0, nWin);

            if (o.TruthCheck) Truth.Report(results, bySv, T0, nWin, o);

            if (o.Out != null)
            {
                if (minSv < o.MinSvPerWindow) throw new InvalidOperationException("only " + minSv + " satellites in the sparsest window (need " + o.MinSvPerWindow + "); not writing the output file");
                if (iono == null) throw new InvalidOperationException("no ionosphere data");
                byte[] file = BuildFile(winBytes, T0, nWin, nowUnix);
                string err = ValidateBin(file);
                if (err != null) throw new InvalidOperationException("self-check failed: " + err);
                string full = Path.GetFullPath(o.Out);
                string dir = Path.GetDirectoryName(full);
                if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
                string tmp = full + ".tmp";
                File.WriteAllBytes(tmp, file);
                if (File.Exists(full)) File.Replace(tmp, full, null); else File.Move(tmp, full);
                Log("[5/5] wrote " + full + " (" + file.Length + " bytes, " + nWin + " windows, " + used.Count + " satellites, last window ends " + Iso((long)(T0 + nWin * WinSec) + K.GPS_UNIX0 - K.LEAP) + ")");
                using (var sha = System.Security.Cryptography.SHA256.Create())
                    Log("  output sha256 " + BitConverter.ToString(sha.ComputeHash(file)).Replace("-", "").ToLowerInvariant());
            }
            Log(string.Format(CultureInfo.InvariantCulture, "done in {0:0.0} s (fit {1:0.0} s); {2} satellites used, {3} excluded, {4} sat-windows dropped",
                total.Elapsed.TotalSeconds, fitClock.Elapsed.TotalSeconds, used.Count, excl.Count, nwinFail));
            return 0;
        }

        static SvResult FitSv(int sv, List<BcRec> all, double T0, double nowGps, int nWin, double fitW, double step, SunMoonGrid grid, Options o)
        {
            Stopwatch sw = Stopwatch.StartNew();
            SvResult res = new SvResult();
            res.Sv = sv;
            try
            {
                BcRec newest = null;
                foreach (BcRec r in all) if (r.ToeAbs <= nowGps && (newest == null || r.ToeAbs >= newest.ToeAbs)) newest = r;
                List<BcRec> healthy = all.Where(delegate(BcRec r) { return r.ToeAbs <= T0 && r.Health == 0; }).ToList();
                if (newest == null) { res.Excluded = "no broadcast data"; return res; }
                if (newest.Health != 0) { res.Excluded = "newest broadcast record unhealthy (health=" + (int)newest.Health + ")"; return res; }
                if (healthy.Count < 8) { res.Excluded = "too few healthy records (" + healthy.Count + ")"; return res; }
                BcRec clk = healthy[healthy.Count - 1];
                if (T0 - clk.ToeAbs > 8 * 3600.0) { res.Excluded = "stale broadcast data (newest healthy toe " + ((T0 - clk.ToeAbs) / 3600.0).ToString("0.0", CultureInfo.InvariantCulture) + " h before the first window)"; return res; }
                double horizon = nWin * 14400.0 + 7200.0;
                OrbitResult orb = o.TrajIn != null ? LoadTraj(o.TrajIn, sv) : OrbitFitter.Fit(healthy, T0, fitW, step, grid, horizon, 12 * 3600.0);
                res.Orb = orb;
                if (orb.Times == null) { res.Excluded = orb.Note; return res; }
                if (orb.FitRmsM > o.SvRmsLimit) { res.Excluded = "orbit fit residual too large (rms " + orb.FitRmsM.ToString("0.0", CultureInfo.InvariantCulture) + " m)"; return res; }
                for (int j = 0; j < orb.Times.Length; j++)
                {
                    double rr = Math.Sqrt(orb.Ecef[3 * j] * orb.Ecef[3 * j] + orb.Ecef[3 * j + 1] * orb.Ecef[3 * j + 1] + orb.Ecef[3 * j + 2] * orb.Ecef[3 * j + 2]);
                    if (!(rr > 25.0e6 && rr < 28.0e6)) { res.Excluded = "predicted orbit diverged"; return res; }
                }
                res.Payload = new byte[nWin][];
                res.Rec = new BcRec[nWin];
                res.WinRms = new double[nWin]; res.WinMax = new double[nWin]; res.WinNote = new string[nWin];
                int week0 = 0;
                for (int k = 0; k < nWin; k++)
                {
                    double toeAbs = T0 + 14400.0 * k + 7200.0;
                    int first = (int)Math.Round((toeAbs - 7200.0 - T0) / OrbitFitter.SampleDt);
                    int n = 49;
                    double[] ts = new double[n], pos = new double[3 * n];
                    for (int i = 0; i < n; i++)
                    {
                        ts[i] = orb.Times[first + i];
                        pos[3 * i] = orb.Ecef[3 * (first + i)]; pos[3 * i + 1] = orb.Ecef[3 * (first + i) + 1]; pos[3 * i + 2] = orb.Ecef[3 * (first + i) + 2];
                    }
                    WinFit wf = WindowFitter.Fit(ts, pos, n, toeAbs);
                    res.WinRms[k] = wf.Rms; res.WinMax[k] = wf.Max;
                    week0 = (int)Math.Floor(toeAbs / 604800.0);
                    BcRec rec = WindowFitter.ToCasicRec(wf.Rec, clk, week0);
                    res.Rec[k] = rec;
                    if (wf.Rms > o.WinRmsLimit) { res.WinNote[k] = "window fit rms " + wf.Rms.ToString("0.0", CultureInfo.InvariantCulture) + " m"; continue; }
                    try { res.Payload[k] = Casic.EncodeEph(rec); }
                    catch (EncodeException ex) { res.WinNote[k] = "encode: " + ex.Message; }
                }
                if (res.Payload.All(delegate(byte[] b) { return b == null; })) res.Excluded = "no window could be encoded";
            }
            catch (Exception ex) { res.Excluded = "internal error: " + ex.Message; }
            finally { res.Seconds = sw.Elapsed.TotalSeconds; }
            return res;
        }

        static OrbitResult LoadTraj(string dir, int sv)
        {
            string[] ls = File.ReadAllLines(Path.Combine(dir, "traj_" + sv + ".txt"));
            OrbitResult r = new OrbitResult();
            r.Times = new double[ls.Length]; r.Ecef = new double[3 * ls.Length]; r.ArcSeconds = 172800;
            for (int i = 0; i < ls.Length; i++)
            {
                string[] f = ls[i].Split(' ');
                r.Times[i] = double.Parse(f[0], CultureInfo.InvariantCulture);
                for (int k = 0; k < 3; k++) r.Ecef[3 * i + k] = double.Parse(f[1 + k], CultureInfo.InvariantCulture);
            }
            return r;
        }

        static byte[] BuildFile(List<byte[]> wins, double T0, int nWin, long nowUnix)
        {
            MemoryStream ms = new MemoryStream();
            BinaryWriter w = new BinaryWriter(ms);
            w.Write(new byte[] { (byte)'F', (byte)'A', (byte)'G', (byte)'P' });
            w.Write((byte)1); w.Write((byte)1);
            w.Write((ushort)nWin);
            w.Write((uint)nowUnix);
            w.Write((uint)0);
            int off = 16 + 16 * nWin;
            for (int k = 0; k < nWin; k++)
            {
                long from = (long)(T0 + 14400.0 * k) + K.GPS_UNIX0 - K.LEAP;   // toe - 2 h, GPS -> UTC
                long to = from + 14400;
                w.Write((uint)from); w.Write((uint)to);
                w.Write((uint)off); w.Write((ushort)wins[k].Length); w.Write((ushort)0);
                off += wins[k].Length;
            }
            for (int k = 0; k < nWin; k++) w.Write(wins[k]);
            w.Flush();
            return ms.ToArray();
        }

        // Mirrors scripts/publish_wardriving.ps1 Get-AgpsBinInfo, plus a frame-level check of every payload.
        public static string ValidateBin(byte[] b)
        {
            if (b.Length < 16) return "file shorter than header";
            if (b.Length >= 204800) return "file too large";
            if (b[0] != 0x46 || b[1] != 0x41 || b[2] != 0x47 || b[3] != 0x50) return "bad magic";
            if (b[4] != 1) return "unsupported version";
            int count = BitConverter.ToUInt16(b, 6);
            if (count == 0) return "no windows";
            int tableEnd = 16 + 16 * count;
            if (b.Length < tableEnd) return "window table truncated";
            uint prevFrom = 0;
            for (int i = 0; i < count; i++)
            {
                int e = 16 + 16 * i;
                uint from = BitConverter.ToUInt32(b, e), to = BitConverter.ToUInt32(b, e + 4);
                long off = BitConverter.ToUInt32(b, e + 8);
                long len = BitConverter.ToUInt16(b, e + 12);
                if (to <= from) return "window " + i + " has an empty range";
                if (i > 0 && from < prevFrom) return "windows not sorted";
                prevFrom = from;
                if (len == 0 || len > 4096) return "window " + i + " bad length";
                if (off < tableEnd || off + len > b.Length) return "window " + i + " outside file";
                long p = off;
                int eph = 0, ion = 0;
                while (p < off + len)
                {
                    if (p + 10 > off + len || b[p] != 0xBA || b[p + 1] != 0xCE) return "window " + i + " bad frame sync";
                    int pl = BitConverter.ToUInt16(b, (int)p + 2);
                    if (p + 10 + pl > off + len) return "window " + i + " frame overruns";
                    byte[] payload = new byte[pl];
                    Array.Copy(b, p + 6, payload, 0, pl);
                    if (BitConverter.ToUInt32(b, (int)p + 6 + pl) != Casic.Checksum(b[p + 4], b[p + 5], payload)) return "window " + i + " bad checksum";
                    if (b[p + 4] == 8 && b[p + 5] == 7) eph++; else if (b[p + 4] == 8 && b[p + 5] == 6) ion++; else return "window " + i + " unexpected message";
                    p += 10 + pl;
                }
                if (eph > 32 || ion != 1) return "window " + i + " bad frame mix";
            }
            return null;
        }

        static void DumpDebug(string dir, SvResult[] results, double T0, int nWin)
        {
            Directory.CreateDirectory(dir);
            StringBuilder sb = new StringBuilder();
            sb.AppendLine("sv,k,toe_abs,rms,max,payload_hex");
            foreach (SvResult r in results)
            {
                if (r.Orb != null && r.Orb.Times != null)
                {
                    StringBuilder tb = new StringBuilder();
                    for (int j = 0; j < r.Orb.Times.Length; j++)
                        tb.AppendLine(r.Orb.Times[j].ToString("0", CultureInfo.InvariantCulture) + " " + r.Orb.Ecef[3 * j].ToString("R", CultureInfo.InvariantCulture) + " " +
                            r.Orb.Ecef[3 * j + 1].ToString("R", CultureInfo.InvariantCulture) + " " + r.Orb.Ecef[3 * j + 2].ToString("R", CultureInfo.InvariantCulture));
                    File.WriteAllText(Path.Combine(dir, "traj_" + r.Sv + ".txt"), tb.ToString());
                }
                if (r.Payload == null) continue;
                for (int k = 0; k < nWin; k++)
                    sb.AppendLine(r.Sv + "," + k + "," + (T0 + 14400.0 * k + 7200.0).ToString("0", CultureInfo.InvariantCulture) + "," +
                        r.WinRms[k].ToString("R", CultureInfo.InvariantCulture) + "," + r.WinMax[k].ToString("R", CultureInfo.InvariantCulture) + "," +
                        (r.Payload[k] == null ? "" : BitConverter.ToString(r.Payload[k]).Replace("-", "")));
            }
            File.WriteAllText(Path.Combine(dir, "windows.csv"), sb.ToString());
        }
    }

    static class Truth
    {
        static double Percentile(List<double> v, double p)
        {
            if (v.Count == 0) return double.NaN;
            double[] a = v.OrderBy(delegate(double x) { return x; }).ToArray();
            double pos = (a.Length - 1) * p / 100.0;
            int lo = (int)Math.Floor(pos), hi = (int)Math.Ceiling(pos);
            return a[lo] + (a[hi] - a[lo]) * (pos - lo);
        }

        // Evaluate the quantised (decoded) window records against nearest-toe broadcast truth.
        public static void Report(SvResult[] results, SortedDictionary<int, List<BcRec>> bySv, double T0, int nWin, Options o)
        {
            Log("== truth check: window records (decoded from the encoded bytes) vs later broadcast ephemerides ==");
            int days = nWin / 6;
            List<double>[] wmed = new List<double>[days], wmax = new List<double>[days], pooled = new List<double>[days];
            for (int d = 0; d < days; d++) { wmed[d] = new List<double>(); wmax[d] = new List<double>(); pooled[d] = new List<double>(); }
            StringBuilder csv = new StringBuilder("sv,window,day,toe_abs,median_err_m,max_err_m,fit_rms_m\n");
            Dictionary<int, List<double>> d7 = new Dictionary<int, List<double>>();
            int noTruth = 0;
            double[] a = new double[3], b = new double[3];
            foreach (SvResult r in results)
            {
                if (r.Excluded != null || r.Payload == null) continue;
                List<BcRec> truth = bySv[r.Sv].Where(delegate(BcRec x) { return x.Health == 0; }).ToList();
                for (int k = 0; k < nWin; k++)
                {
                    if (r.Payload[k] == null) continue;
                    double toe = T0 + 14400.0 * k + 7200.0;
                    BcRec dec = Casic.DecodeEph(r.Payload[k]);
                    dec.ToeAbs = Math.Floor(toe / 604800.0) * 604800.0 + dec.Toe;
                    List<double> errs = new List<double>();
                    for (double t = toe - 7200; t <= toe + 7200 + 1e-6; t += 600)
                    {
                        BcRec best = null; double bd = double.MaxValue;
                        foreach (BcRec x in truth) { double dd = Math.Abs(x.ToeAbs - t); if (dd < bd) { bd = dd; best = x; } }
                        if (best == null || bd > 3 * 3600.0) continue;
                        Bc.Pos(dec, t, a); Bc.Pos(best, t, b);
                        errs.Add(Math.Sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])));
                    }
                    if (errs.Count < 20) { noTruth++; continue; }
                    int d = k / 6;
                    double med = Percentile(errs, 50), mx = errs.Max();
                    wmed[d].Add(med); wmax[d].Add(mx); pooled[d].AddRange(errs);
                    csv.AppendLine(r.Sv + "," + k + "," + (d + 1) + "," + toe.ToString("0", CultureInfo.InvariantCulture) + "," + med.ToString("0.0", CultureInfo.InvariantCulture) + "," +
                        mx.ToString("0.0", CultureInfo.InvariantCulture) + "," + r.WinRms[k].ToString("0.00", CultureInfo.InvariantCulture));
                    if (d == days - 1) { List<double> l; if (!d7.TryGetValue(r.Sv, out l)) { l = new List<double>(); d7[r.Sv] = l; } l.Add(med); }
                }
            }
            if (o.Csv != null) File.WriteAllText(o.Csv, csv.ToString());
            Log("day   n(sat-win)  window-median-err(m): median   p95      max  | all-samples p95  max | window-max-err p95");
            for (int d = 0; d < days; d++)
            {
                if (wmed[d].Count == 0) { Log(string.Format(CultureInfo.InvariantCulture, "{0,3}   no truth available", d + 1)); continue; }
                Log(string.Format(CultureInfo.InvariantCulture, "{0,3}   {1,6}             {2,10:0.0} {3,8:0.0} {4,9:0.0}  | {5,8:0.0} {6,9:0.0} | {7,8:0.0}",
                    d + 1, wmed[d].Count, Percentile(wmed[d], 50), Percentile(wmed[d], 95), wmed[d].Max(), Percentile(pooled[d], 95), pooled[d].Max(), Percentile(wmax[d], 95)));
            }
            if (noTruth > 0) Log("  (" + noTruth + " sat-windows had no truth coverage)");
            List<KeyValuePair<int, double>> worst = d7.Select(delegate(KeyValuePair<int, List<double>> kv) { return new KeyValuePair<int, double>(kv.Key, kv.Value.Max()); })
                .OrderByDescending(delegate(KeyValuePair<int, double> kv) { return kv.Value; }).Take(6).ToList();
            Log("  worst last-day windows: " + string.Join(", ", worst.Select(delegate(KeyValuePair<int, double> kv) { return "G" + kv.Key.ToString("00") + "=" + kv.Value.ToString("0", CultureInfo.InvariantCulture); }).ToArray()));
        }

        static void Log(string s) { Console.Out.WriteLine(s); Console.Out.Flush(); }
    }

    static class SelfTest
    {
        // Developer: compare cached BKG (RINEX 3) and Wuhan (RINEX 2) records of one 2026 day, matched on (PRN, toe, IODC).
        public static int RunCompare(Options o)
        {
            int doy = int.Parse(o.CompareNav, CultureInfo.InvariantCulture);
            DateTime day = NavSource.DayStart(2026, doy);
            NavSource src = new NavSource(o.Cache ?? ".", true, delegate(string s) { });
            src.Active = "bkg";
            List<BcRec> a = src.LoadNavDay(day);
            src.Active = "wuhan";
            List<BcRec> b = src.LoadNavDay(day);
            Dictionary<string, BcRec> bm = new Dictionary<string, BcRec>();
            foreach (BcRec r in b) bm[r.Svid + "|" + (long)r.ToeAbs + "|" + (long)r.Iodc] = r;
            Dictionary<string, BcRec> am = new Dictionary<string, BcRec>();
            foreach (BcRec r in a) am[r.Svid + "|" + (long)r.ToeAbs + "|" + (long)r.Iodc] = r;
            int matched = 0, onlyA = 0, onlyB = 0, uraDiff = 0; string uraEx = "";
            double dPos = 0, dPosAt1h = 0, mxRel = 0; string mxField = "";
            foreach (KeyValuePair<string, BcRec> kv in am)
            {
                BcRec x = kv.Value, y;
                if (!bm.TryGetValue(kv.Key, out y)) { onlyA++; continue; }
                matched++;
                double[] fx = new double[] { x.Af0, x.Af1, x.Af2, x.Iode, x.Crs, x.DeltaN, x.M0, x.Cuc, x.E, x.Cus, x.SqrtA, x.Toe, x.Cic, x.Omega0, x.Cis, x.I0, x.Crc, x.Omega, x.OmegaDot, x.Idot, x.Week, x.UraM, x.Health, x.Tgd, x.Iodc, x.Toc };
                double[] fy = new double[] { y.Af0, y.Af1, y.Af2, y.Iode, y.Crs, y.DeltaN, y.M0, y.Cuc, y.E, y.Cus, y.SqrtA, y.Toe, y.Cic, y.Omega0, y.Cis, y.I0, y.Crc, y.Omega, y.OmegaDot, y.Idot, y.Week, y.UraM, y.Health, y.Tgd, y.Iodc, y.Toc };
                string[] nm = new string[] { "af0", "af1", "af2", "iode", "crs", "deltan", "m0", "cuc", "e", "cus", "sqrta", "toe", "cic", "omega0", "cis", "i0", "crc", "omega", "omegadot", "idot", "week", "ura", "health", "tgd", "iodc", "toc" };
                for (int i = 0; i < fx.Length; i++)
                {
                    double rel = Math.Abs(fx[i] - fy[i]) / Math.Max(1e-300, Math.Max(Math.Abs(fx[i]), Math.Abs(fy[i])));
                    if (fx[i] == fy[i]) rel = 0;
                    if (nm[i] == "ura") { if (rel > 0) { uraDiff++; if (uraEx == "") uraEx = "G" + x.Svid + " bkg " + fx[i] + " wuhan " + fy[i]; } } else if (rel > mxRel) { mxRel = rel; mxField = nm[i] + " G" + x.Svid + " toe " + x.ToeAbs; }
                }
                double[] p = new double[3], q = new double[3];
                foreach (double dt in new double[] { 0, 3600 })
                {
                    Bc.Pos(x, x.ToeAbs + dt, p); Bc.Pos(y, x.ToeAbs + dt, q);
                    double d = Math.Sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
                    if (dt == 0) dPos = Math.Max(dPos, d); else dPosAt1h = Math.Max(dPosAt1h, d);
                }
            }
            foreach (string k in bm.Keys) if (!am.ContainsKey(k)) onlyB++;
            Console.WriteLine("doy " + doy + ": bkg " + a.Count + " records (" + am.Count + " distinct), wuhan " + b.Count + " (" + bm.Count + " distinct); matched " + matched + ", only-bkg " + onlyA + ", only-wuhan " + onlyB);
            Console.WriteLine("  ura differs in " + uraDiff + " matched records (" + uraEx + ")");
            Console.WriteLine("  max relative field difference (excl. ura) over matched: " + mxRel.ToString("0.0e+00", CultureInfo.InvariantCulture) + " (" + mxField + ")");
            Console.WriteLine("  max position difference at toe: " + dPos.ToString("0.000000", CultureInfo.InvariantCulture) + " m, at toe+1h: " + dPosAt1h.ToString("0.000000", CultureInfo.InvariantCulture) + " m");
            return 0;
        }

        // Developer: decode two agps.bin files and compare the per-SV window ephemerides (position over each window).
        public static int RunCompareBin(Options o)
        {
            string[] p = o.CompareBin.Split(',');
            byte[] a = File.ReadAllBytes(p[0]), b = File.ReadAllBytes(p[1]);
            int na = BitConverter.ToUInt16(a, 6), nb = BitConverter.ToUInt16(b, 6);
            if (na != nb) { Console.WriteLine("window counts differ: " + na + " vs " + nb); return 1; }
            List<double> errs = new List<double>();
            int frames = 0, missing = 0, ionDiff = 0;
            for (int k = 0; k < na; k++)
            {
                Dictionary<int, byte[]> fa = Frames(a, k), fb = Frames(b, k);
                byte[] ia, ib;
                bool hasIa = fa.TryGetValue(-1, out ia), hasIb = fb.TryGetValue(-1, out ib);
                if (hasIa != hasIb) ionDiff++;
                else if (hasIa) { ib = fb[-1]; for (int i = 0; i < ia.Length; i++) if (ia[i] != ib[i]) { ionDiff++; break; } }
                foreach (KeyValuePair<int, byte[]> kv in fa)
                {
                    if (kv.Key < 0) continue;
                    byte[] y;
                    if (!fb.TryGetValue(kv.Key, out y)) { missing++; continue; }
                    BcRec x1 = Casic.DecodeEph(kv.Value), y1 = Casic.DecodeEph(y);
                    uint from = BitConverter.ToUInt32(a, 16 + 16 * k);
                    double toeAbsGps = (double)from + K.LEAP - K.GPS_UNIX0 + 7200.0;
                    x1.ToeAbs = Math.Floor(toeAbsGps / 604800.0) * 604800.0 + x1.Toe; y1.ToeAbs = Math.Floor(toeAbsGps / 604800.0) * 604800.0 + y1.Toe;
                    double[] u = new double[3], v = new double[3];
                    for (double t = toeAbsGps - 7200; t <= toeAbsGps + 7200 + 1e-6; t += 600)
                    {
                        Bc.Pos(x1, t, u); Bc.Pos(y1, t, v);
                        errs.Add(Math.Sqrt((u[0] - v[0]) * (u[0] - v[0]) + (u[1] - v[1]) * (u[1] - v[1]) + (u[2] - v[2]) * (u[2] - v[2])));
                    }
                    frames++;
                }
            }
            errs.Sort();
            Console.WriteLine("windows " + na + ", eph frames compared " + frames + ", missing in 2nd " + missing + ", windows with ION frame differing " + ionDiff);
            if (errs.Count > 0)
                Console.WriteLine("position difference over each window (m): median " + errs[errs.Count / 2].ToString("0.000", CultureInfo.InvariantCulture) + ", p95 " + errs[(int)(errs.Count * 0.95)].ToString("0.000", CultureInfo.InvariantCulture) + ", max " + errs[errs.Count - 1].ToString("0.000", CultureInfo.InvariantCulture));
            return 0;
        }

        static Dictionary<int, byte[]> Frames(byte[] f, int k)
        {
            Dictionary<int, byte[]> d = new Dictionary<int, byte[]>();
            int e = 16 + 16 * k;
            int off = (int)BitConverter.ToUInt32(f, e + 8), len = BitConverter.ToUInt16(f, e + 12);
            int p = off;
            while (p < off + len)
            {
                int pl = BitConverter.ToUInt16(f, p + 2);
                byte[] pay = new byte[pl];
                Array.Copy(f, p + 6, pay, 0, pl);
                if (f[p + 4] == 8 && f[p + 5] == 7) d[Casic.DecodeEph(pay).Svid] = pay; else d[-1] = pay;
                p += 10 + pl;
            }
            return d;
        }

        public static int RunBc(Options o)
        {
            string[] p = o.SelfTestBc.Split(':');
            int sv = int.Parse(p[0]);
            int doy = int.Parse(p[1]);
            NavSource src = new NavSource(o.Cache ?? ".", true, delegate(string s) { });
            List<BcRec> recs = src.LoadNavDay(NavSource.DayStart(2026, doy)).Where(delegate(BcRec r) { return r.Svid == sv; }).OrderBy(delegate(BcRec r) { return r.ToeAbs; }).ToList();
            BcRec rec = recs[5];
            Console.WriteLine("rec toe_abs=" + rec.ToeAbs.ToString("0", CultureInfo.InvariantCulture) + " sqrtA=" + rec.SqrtA.ToString("R", CultureInfo.InvariantCulture));
            double[] q = new double[3];
            foreach (double dt in new double[] { 0, 1800, 3600, 7200, 20000, -5000 })
            {
                Bc.Pos(rec, rec.ToeAbs + dt, q);
                Console.WriteLine(dt.ToString("0") + " " + q[0].ToString("0.000000", CultureInfo.InvariantCulture) + " " + q[1].ToString("0.000000", CultureInfo.InvariantCulture) + " " + q[2].ToString("0.000000", CultureInfo.InvariantCulture));
            }
            return 0;
        }

        // file: line1 "T0 y0[6] p[7] nSteps"; following lines: state samples every 300 s (ECI_F, 6 values), incl. step 0
        public static int RunProp(Options o)
        {
            string[] lines = File.ReadAllLines(o.SelfTestProp);
            string[] h = lines[0].Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
            double t0 = double.Parse(h[0], CultureInfo.InvariantCulture);
            double[] y0 = new double[6], p = new double[7];
            for (int i = 0; i < 6; i++) y0[i] = double.Parse(h[1 + i], CultureInfo.InvariantCulture);
            for (int i = 0; i < 7; i++) p[i] = double.Parse(h[7 + i], CultureInfo.InvariantCulture);
            double span = double.Parse(h[14], CultureInfo.InvariantCulture);
            double step = o.Step;
            int nSteps = (int)Math.Round(span / step);
            SunMoonGrid grid = new SunMoonGrid(t0, step / 2, nSteps * 2 + 8);
            Stopwatch sw = Stopwatch.StartNew();
            Prop pr = new Prop(grid, t0, step);
            double[] st = pr.Integrate(y0, p, nSteps, (int)Math.Round(300.0 / step));
            double ms = sw.Elapsed.TotalMilliseconds;
            int n = lines.Length - 1;
            double mx = 0;
            Console.WriteLine("RK4 step " + step + " s, " + nSteps + " steps in " + ms.ToString("0") + " ms");
            Console.WriteLine("  day   max |dr| over that day (m)");
            double dayMax = 0; int day = 1;
            for (int i = 0; i < n; i++)
            {
                string[] f = lines[1 + i].Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                double dx = st[6 * i] - double.Parse(f[0], CultureInfo.InvariantCulture), dy = st[6 * i + 1] - double.Parse(f[1], CultureInfo.InvariantCulture), dz = st[6 * i + 2] - double.Parse(f[2], CultureInfo.InvariantCulture);
                double d = Math.Sqrt(dx * dx + dy * dy + dz * dz);
                mx = Math.Max(mx, d); dayMax = Math.Max(dayMax, d);
                if ((i + 1) % 288 == 0 || i == n - 1) { Console.WriteLine("  " + day + "   " + dayMax.ToString("0.000000")); day++; dayMax = 0; }
            }
            Console.WriteLine("max position difference over " + (n * 300.0 / 86400.0).ToString("0.00") + " days: " + mx.ToString("0.000000") + " m");
            return 0;
        }
    }
}
