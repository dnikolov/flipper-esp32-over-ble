using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace AgpsGen
{
    sealed class NavException : Exception
    {
        public NavException(string m) : base(m) { }
    }

    sealed class Iono
    {
        public double[] A, B;
        public string Source;
    }

    static class Rinex
    {
        static readonly DateTime GpsEpoch = new DateTime(1980, 1, 6, 0, 0, 0, DateTimeKind.Unspecified);

        public static double Fnum(string s)
        {
            s = s.Trim().Replace('D', 'e').Replace('d', 'e');
            if (s.Length == 0) return 0.0;
            return double.Parse(s, NumberStyles.Float, CultureInfo.InvariantCulture);
        }

        static string Sub(string s, int start, int len)
        {
            if (start >= s.Length) return "";
            if (start + len > s.Length) len = s.Length - start;
            return s.Substring(start, len);
        }

        public static string GzText(byte[] data)
        {
            using (GZipStream gz = new GZipStream(new MemoryStream(data), CompressionMode.Decompress))
            using (MemoryStream o = new MemoryStream())
            {
                byte[] buf = new byte[65536];
                int n;
                while ((n = gz.Read(buf, 0, buf.Length)) > 0) o.Write(buf, 0, n);
                return Encoding.ASCII.GetString(o.ToArray());
            }
        }

        // GPS LNAV records of a RINEX 3 nav text; iono = GPSA/GPSB header terms (or null).
        public static List<BcRec> Parse(string text, out Iono iono)
        {
            string[] lines = text.Split('\n');
            iono = null;
            double[] a = null, b = null;
            int i = 0;
            while (i < lines.Length && lines[i].IndexOf("END OF HEADER", StringComparison.Ordinal) < 0)
            {
                string l = lines[i];
                if (l.IndexOf("IONOSPHERIC CORR", StringComparison.Ordinal) >= 0 && l.Length >= 4)
                {
                    string tag = l.Substring(0, 4);
                    double[] v = new double[4];
                    for (int k = 0; k < 4; k++) v[k] = Fnum(Sub(l, 5 + 12 * k, 12));
                    if (tag == "GPSA") a = v; else if (tag == "GPSB") b = v;
                }
                i++;
            }
            if (a != null && b != null) { iono = new Iono(); iono.A = a; iono.B = b; }
            i++;
            List<BcRec> recs = new List<BcRec>();
            while (i < lines.Length)
            {
                string ln = lines[i];
                if (ln.Length >= 23 && ln[0] == 'G' && char.IsDigit(ln[1]) && char.IsDigit(ln[2]) && i + 7 < lines.Length)
                {
                    int row = i;
                    Func<int, int, double> f = delegate(int rw, int k) { return Fnum(Sub(lines[row + rw], 4 + 19 * k, 19)); };
                    DateTime ep = new DateTime(int.Parse(ln.Substring(4, 4)), int.Parse(ln.Substring(9, 2)), int.Parse(ln.Substring(12, 2)),
                        int.Parse(ln.Substring(15, 2)), int.Parse(ln.Substring(18, 2)), int.Parse(ln.Substring(21, 2)), DateTimeKind.Unspecified);
                    BcRec r = new BcRec();
                    r.Svid = int.Parse(ln.Substring(1, 2));
                    r.Af0 = Fnum(Sub(ln, 23, 19)); r.Af1 = Fnum(Sub(ln, 42, 19)); r.Af2 = Fnum(Sub(ln, 61, 19));
                    r.Iode = f(1, 0); r.Crs = f(1, 1); r.DeltaN = f(1, 2); r.M0 = f(1, 3);
                    r.Cuc = f(2, 0); r.E = f(2, 1); r.Cus = f(2, 2); r.SqrtA = f(2, 3);
                    r.Toe = f(3, 0); r.Cic = f(3, 1); r.Omega0 = f(3, 2); r.Cis = f(3, 3);
                    r.I0 = f(4, 0); r.Crc = f(4, 1); r.Omega = f(4, 2); r.OmegaDot = f(4, 3);
                    r.Idot = f(5, 0); r.Week = f(5, 2);
                    r.UraM = f(6, 0); r.Health = f(6, 1); r.Tgd = f(6, 2); r.Iodc = f(6, 3);
                    double gs = (ep - GpsEpoch).TotalSeconds;
                    r.Toc = gs - 604800.0 * Math.Floor(gs / 604800.0);
                    r.ToeAbs = (double)((long)r.Week) * 604800.0 + r.Toe;
                    recs.Add(r);
                    i += 8;
                }
                else i++;
            }
            return recs;
        }

        // ---- RINEX 2 GPS navigation (IGS combined brdcDDD0.YYn) ----

        // Lenient gunzip for a file that may still be being written: keeps whatever decoded before a truncation.
        public static string GzTextLenient(byte[] data)
        {
            MemoryStream o = new MemoryStream();
            try
            {
                using (GZipStream gz = new GZipStream(new MemoryStream(data), CompressionMode.Decompress))
                {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = gz.Read(buf, 0, buf.Length)) > 0) o.Write(buf, 0, n);
                }
            }
            catch (InvalidDataException) { }
            catch (EndOfStreamException) { }
            catch (IOException) { }
            if (o.Length == 0) throw new NavException("gzip stream has no decodable data");
            return Encoding.ASCII.GetString(o.ToArray());
        }

        static bool TryNum(string s, bool required, out double v)
        {
            s = s.Trim().Replace('D', 'e').Replace('d', 'e');
            v = 0.0;
            if (s.Length == 0) return !required;
            if (!double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out v)) return false;
            return !(double.IsNaN(v) || double.IsInfinity(v));
        }

        static bool TryInt(string s, out int v)
        {
            s = s.Trim();
            v = 0;
            return s.Length > 0 && int.TryParse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out v);
        }

        // Plausibility gate for a broadcast record. Applied to RINEX 2 input, which arrives over cleartext FTP.
        public static string Implausible(BcRec r)
        {
            if (r.Svid < 1 || r.Svid > 32) return "PRN";
            double a = r.SqrtA * r.SqrtA;
            if (!(r.SqrtA > 5000.0 && r.SqrtA < 5400.0)) return "sqrtA";
            if (!(r.E >= 0.0 && r.E < 0.1)) return "eccentricity";
            if (!(a * (1.0 - r.E) > 24.5e6 && a * (1.0 + r.E) < 29.0e6)) return "orbit radius";
            if (!(r.I0 > 0.8 && r.I0 < 1.2)) return "inclination";
            if (!(r.Toe >= 0.0 && r.Toe < 604800.0)) return "toe";
            if (!(r.Week >= 1000.0 && r.Week < 8192.0)) return "week";
            double dt = Math.Abs(r.Toe - r.Toc);
            if (dt > 86400.0 && Math.Abs(dt - 604800.0) > 86400.0) return "toe/toc mismatch";
            if (!(r.Health >= 0.0 && r.Health < 64.0)) return "health";
            if (!(Math.Abs(r.Af0) < 1e-2 && Math.Abs(r.Af1) < 1e-7 && Math.Abs(r.Af2) < 1e-10)) return "clock terms";
            if (!(Math.Abs(r.Tgd) < 1e-6)) return "tgd";
            if (!(Math.Abs(r.DeltaN) < 1e-7 && Math.Abs(r.Idot) < 1e-7 && Math.Abs(r.OmegaDot) < 1e-7)) return "rates";
            if (!(Math.Abs(r.Crs) < 5000.0 && Math.Abs(r.Crc) < 5000.0 && Math.Abs(r.Cuc) < 1e-3 && Math.Abs(r.Cus) < 1e-3 && Math.Abs(r.Cic) < 1e-3 && Math.Abs(r.Cis) < 1e-3)) return "harmonics";
            return null;
        }

        // GPS records of a RINEX 2 nav text. Junk/implausible/truncated records are skipped and counted; only a
        // wrong file type throws NavException. iono = ION ALPHA/BETA header terms (or null).
        public static List<BcRec> Parse2(string text, out Iono iono, out int skipped)
        {
            string[] lines = text.Split('\n');
            for (int k = 0; k < lines.Length; k++) lines[k] = lines[k].TrimEnd('\r');
            iono = null; skipped = 0;
            double[] a = null, b = null;
            int i = 0;
            if (lines.Length < 2 || lines[0].IndexOf("RINEX VERSION / TYPE", StringComparison.Ordinal) < 0 || lines[0].TrimStart().Length == 0 || lines[0].TrimStart()[0] != '2' ||
                lines[0].IndexOf("NAV", StringComparison.Ordinal) < 0)
                throw new NavException("not a RINEX 2 navigation file");
            bool endHeader = false;
            for (; i < lines.Length; i++)
            {
                string l = lines[i];
                if (l.IndexOf("END OF HEADER", StringComparison.Ordinal) >= 0) { endHeader = true; i++; break; }
                bool isA = l.IndexOf("ION ALPHA", StringComparison.Ordinal) >= 0, isB = l.IndexOf("ION BETA", StringComparison.Ordinal) >= 0;
                if (isA || isB)
                {
                    string[] tk = (l.Length > 60 ? l.Substring(0, 60) : l).Split(new char[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                    double[] v = new double[4];
                    bool ok = tk.Length >= 4;
                    for (int k = 0; ok && k < 4; k++) ok = TryNum(tk[k], true, out v[k]);
                    if (ok) { if (isA) a = v; else b = v; }
                }
            }
            if (!endHeader) throw new NavException("RINEX 2 header not terminated");
            if (a != null && b != null) { iono = new Iono(); iono.A = a; iono.B = b; }
            List<BcRec> recs = new List<BcRec>();
            while (i < lines.Length)
            {
                string ln = lines[i];
                int prn, yy, mo, dd, hh, mi;
                double ss;
                if (ln.Length < 79 || !TryInt(Sub(ln, 0, 2), out prn) || prn < 1 || prn > 32 ||
                    !TryInt(Sub(ln, 3, 2), out yy) || !TryInt(Sub(ln, 6, 2), out mo) || !TryInt(Sub(ln, 9, 2), out dd) ||
                    !TryInt(Sub(ln, 12, 2), out hh) || !TryInt(Sub(ln, 15, 2), out mi) || !TryNum(Sub(ln, 17, 5), true, out ss))
                { i++; continue; }
                // record start: line 1 plus 7 data lines; lines 1-7 must be complete (4 x D19.12), line 8 may be short/absent.
                if (i + 7 >= lines.Length) { skipped++; break; }
                bool bad = false;
                for (int k = 1; k <= 6 && !bad; k++) if (lines[i + k].Length < 79) bad = true;
                double[,] f = new double[8, 4];
                double af0 = 0, af1 = 0, af2 = 0;
                if (!bad)
                {
                    bad = !(TryNum(Sub(ln, 22, 19), true, out af0) && TryNum(Sub(ln, 41, 19), true, out af1) && TryNum(Sub(ln, 60, 19), true, out af2));
                    for (int rw = 1; rw <= 7 && !bad; rw++)
                        for (int k = 0; k < 4 && !bad; k++)
                            if (!TryNum(Sub(lines[i + rw], 3 + 19 * k, 19), rw <= 6, out f[rw, k])) bad = true;
                }
                if (bad || mo < 1 || mo > 12 || dd < 1 || dd > 31 || hh < 0 || hh > 23 || mi < 0 || mi > 59 || ss < 0.0 || ss >= 61.0 || yy < 0 || yy > 99) { skipped++; i++; continue; }
                BcRec r = new BcRec();
                double gs;
                try
                {
                    DateTime ep = new DateTime(yy < 80 ? 2000 + yy : 1900 + yy, mo, dd, hh, mi, 0, DateTimeKind.Unspecified).AddSeconds(Math.Round(ss));
                    gs = (ep - GpsEpoch).TotalSeconds;
                }
                catch (ArgumentOutOfRangeException) { skipped++; i++; continue; }
                r.Svid = prn; r.Af0 = af0; r.Af1 = af1; r.Af2 = af2;
                r.Iode = f[1, 0]; r.Crs = f[1, 1]; r.DeltaN = f[1, 2]; r.M0 = f[1, 3];
                r.Cuc = f[2, 0]; r.E = f[2, 1]; r.Cus = f[2, 2]; r.SqrtA = f[2, 3];
                r.Toe = f[3, 0]; r.Cic = f[3, 1]; r.Omega0 = f[3, 2]; r.Cis = f[3, 3];
                r.I0 = f[4, 0]; r.Crc = f[4, 1]; r.Omega = f[4, 2]; r.OmegaDot = f[4, 3];
                r.Idot = f[5, 0]; r.Week = f[5, 2];
                r.UraM = f[6, 0]; r.Health = f[6, 1]; r.Tgd = f[6, 2]; r.Iodc = f[6, 3];
                r.Toc = gs - 604800.0 * Math.Floor(gs / 604800.0);
                r.ToeAbs = (double)((long)r.Week) * 604800.0 + r.Toe;
                // the epoch must fall in the record's own GPS week (+-1) and the record must look like a GPS orbit
                if (Implausible(r) != null || Math.Abs(Math.Floor(gs / 604800.0) - r.Week) > 1.0) { skipped++; i++; continue; }
                recs.Add(r);
                i += 8;
            }
            return recs;
        }
    }

    // Nav-file cache + downloads (BKG https RINEX 3, Wuhan ftp RINEX 2).
    sealed class NavSource
    {
        public const string DefaultBkgBase = "https://igs.bkg.bund.de/root_ftp/IGS/BRDC/";
        public const string DefaultWuhanBase = "ftp://igs.gnsswhu.cn/pub/gps/data/daily/";
        public string BkgBase = DefaultBkgBase;
        public string WuhanBase = DefaultWuhanBase;
        public string Mode = "auto";            // auto | bkg | wuhan
        public string Active = "bkg";           // source LoadNavDay/LoadIono use
        readonly string cache;
        bool offline;
        bool allowStale = true;
        bool fuse;                              // auto mode, BKG: short timeouts, 1 retry
        readonly DateTime realNowUtc;
        readonly Action<string> log;

        public NavSource(string cacheDir, bool offlineOnly, Action<string> logger)
        {
            cache = cacheDir; offline = offlineOnly; log = logger; realNowUtc = DateTime.UtcNow;
            Directory.CreateDirectory(cache);
            try { ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12; }
            catch (Exception) { ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; }
            ServicePointManager.DefaultConnectionLimit = 16;
        }

        public static DateTime DayStart(int year, int doy) { return new DateTime(year, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddDays(doy - 1); }

        static string Y(DateTime d) { return d.ToString("yyyy", CultureInfo.InvariantCulture); }
        static string Doy(DateTime d) { return d.DayOfYear.ToString("000", CultureInfo.InvariantCulture); }

        string BkgUrl(DateTime day, string tmpl)
        {
            return BkgBase + Y(day) + "/" + Doy(day) + "/" + string.Format(tmpl, Y(day), Doy(day));
        }

        string WuhanUrl(DateTime day)
        {
            return WuhanBase + Y(day) + "/brdc/brdc" + Doy(day) + "0." + (day.Year % 100).ToString("00", CultureInfo.InvariantCulture) + "n.gz";
        }

        public static byte[] Download(string url, int timeoutMs, int tries)
        {
            Exception last = null;
            for (int attempt = 1; attempt <= tries; attempt++)
            {
                try
                {
                    HttpWebRequest rq = (HttpWebRequest)WebRequest.Create(url);
                    rq.Timeout = timeoutMs; rq.ReadWriteTimeout = timeoutMs;
                    rq.UserAgent = "agps_gen/1.0";
                    using (HttpWebResponse rs = (HttpWebResponse)rq.GetResponse())
                    using (Stream s = rs.GetResponseStream())
                        return ReadAll(s);
                }
                catch (WebException ex)
                {
                    last = ex;
                    HttpWebResponse hr = ex.Response as HttpWebResponse;
                    if (hr != null && hr.StatusCode == HttpStatusCode.NotFound) throw new NavException("404 not found: " + url);
                    if (attempt < tries) Thread.Sleep(1500 * attempt);
                }
                catch (IOException ex) { last = ex; if (attempt < tries) Thread.Sleep(1500 * attempt); }
            }
            throw new NavException("download failed (" + (last == null ? "?" : last.Message) + "): " + url);
        }

        // Anonymous, passive, binary FTP; KeepAlive off; explicit timeouts.
        public static byte[] FtpDownload(string url, int timeoutMs, int tries)
        {
            Exception last = null;
            for (int attempt = 1; attempt <= tries; attempt++)
            {
                try
                {
                    FtpWebRequest rq = (FtpWebRequest)WebRequest.Create(url);
                    rq.Method = WebRequestMethods.Ftp.DownloadFile;
                    rq.Credentials = new NetworkCredential("anonymous", "agps_gen@localhost");
                    rq.UsePassive = true; rq.UseBinary = true; rq.KeepAlive = false; rq.EnableSsl = false;
                    rq.Proxy = null;
                    rq.Timeout = timeoutMs; rq.ReadWriteTimeout = timeoutMs;
                    using (FtpWebResponse rs = (FtpWebResponse)rq.GetResponse())
                    using (Stream s = rs.GetResponseStream())
                        return ReadAll(s);
                }
                catch (WebException ex)
                {
                    last = ex;
                    FtpWebResponse fr = ex.Response as FtpWebResponse;
                    if (fr != null && fr.StatusCode == FtpStatusCode.ActionNotTakenFileUnavailable) throw new NavException("550 not found: " + url);
                    if (attempt < tries) Thread.Sleep(1000);
                }
                catch (IOException ex) { last = ex; if (attempt < tries) Thread.Sleep(1000); }
            }
            throw new NavException("ftp download failed (" + (last == null ? "?" : last.Message) + "): " + url);
        }

        static byte[] ReadAll(Stream s)
        {
            using (MemoryStream o = new MemoryStream())
            {
                byte[] buf = new byte[65536];
                int n;
                while ((n = s.Read(buf, 0, buf.Length)) > 0) o.Write(buf, 0, n);
                return o.ToArray();
            }
        }

        static bool ValidGz3(byte[] data, int minRecords)
        {
            try
            {
                Iono io;
                return Rinex.Parse(Rinex.GzText(data), out io).Count >= minRecords;
            }
            catch (Exception) { return false; }
        }

        static bool ValidGz2(byte[] data, int minRecords)
        {
            try
            {
                Iono io; int sk;
                return Rinex.Parse2(Rinex.GzTextLenient(data), out io, out sk).Count >= minRecords;
            }
            catch (Exception) { return false; }
        }

        // Is the cached file usable without a download? settleMin: minutes after the UTC day end by which the
        // published day file is considered final.
        bool CacheFresh(DateTime day, string path, bool dayFile, int settleMin)
        {
            if (!(File.Exists(path) && new FileInfo(path).Length > 0)) return false;
            DateTime m = File.GetLastWriteTimeUtc(path);
            DateTime dayEnd = day.AddDays(1);
            if (m >= dayEnd.AddMinutes(settleMin)) return true;                       // downloaded after the day was final
            if (realNowUtc >= dayEnd.AddMinutes(settleMin) && !dayFile) return true;
            if ((realNowUtc - m).TotalMinutes < 15) return true;                      // today's file: updated every 15 min
            if (realNowUtc < day) return true;
            return false;
        }

        // Returns file bytes for the cache name, downloading when missing/stale.
        byte[] Get(DateTime day, string url, string cacheName, bool dayFile, int settleMin, string[] altNames, Func<byte[]> fetch, Func<byte[], bool> valid)
        {
            string path = Path.Combine(cache, cacheName);
            bool have = File.Exists(path) && new FileInfo(path).Length > 0;
            bool fresh = CacheFresh(day, path, dayFile, settleMin);
            if (have && (fresh || offline)) return File.ReadAllBytes(path);
            if (!have && altNames != null)
            {
                for (int i = 0; i < altNames.Length; i++)
                {
                    string ap = Path.Combine(cache, altNames[i]);
                    if (File.Exists(ap) && new FileInfo(ap).Length > 0) { log("using cached " + altNames[i]); return File.ReadAllBytes(ap); }
                }
            }
            if (offline) throw new NavException("not in cache (offline): " + cacheName);
            log("downloading " + url);
            try
            {
                byte[] data = fetch();
                if (!valid(data)) throw new NavException("downloaded file is not a valid RINEX nav gzip: " + url);
                string tmp = path + ".part";
                File.WriteAllBytes(tmp, data);
                if (File.Exists(path)) File.Delete(path);
                File.Move(tmp, path);
                return data;
            }
            catch (NavException)
            {
                if (have && allowStale) { log("download failed, using stale cache " + cacheName); return File.ReadAllBytes(path); }
                throw;
            }
        }

        // ---- BKG (RINEX 3) ----
        string BkgName(DateTime day, string prefix) { return prefix + Y(day) + Doy(day) + ".gz"; }

        byte[] GetBkg(DateTime day, string tmpl, string cacheName, bool dayFile, string[] alt, int minRecords)
        {
            string url = BkgUrl(day, tmpl);
            int to = fuse ? 15000 : 60000, tries = fuse ? 2 : 3;
            return Get(day, url, cacheName, dayFile, 20, alt, delegate() { return Download(url, to, tries); }, delegate(byte[] d) { return ValidGz3(d, minRecords); });
        }

        List<BcRec> LoadNavDayBkg(DateTime day)
        {
            byte[] data = GetBkg(day, "BRDC00WRD_S_{0}{1}0000_01D_MN.rnx.gz", BkgName(day, "brdc_S_"), true,
                new string[] { BkgName(day, "nav_S_"), BkgName(day, "hist_nav_S_") }, 50);
            Iono io;
            return Rinex.Parse(Rinex.GzText(data), out io);
        }

        // Klobuchar terms from the newest BRDM (DLR) header that has them, searching back from `day`.
        Iono LoadIonoBkg(DateTime day, int maxBack)
        {
            string lastErr = "";
            for (int back = 0; back <= maxBack; back++)
            {
                DateTime d = day.AddDays(-back);
                string name = BkgName(d, "hist_brdm_");
                try
                {
                    byte[] data = GetBkg(d, "BRDM00DLR_S_{0}{1}0000_01D_MN.rnx.gz", name, false, null, 0);
                    Iono io;
                    Rinex.Parse(Rinex.GzText(data), out io);
                    if (io != null) { io.Source = "bkg " + name; return io; }
                }
                catch (Exception ex) { lastErr = ex.Message; }
            }
            throw new NavException("no ionosphere (GPSA/GPSB) source found in the last " + (maxBack + 1) + " days (" + lastErr + ")");
        }

        // ---- Wuhan (RINEX 2, ftp) ----
        byte[] GetWuhan(DateTime day)
        {
            string url = WuhanUrl(day);
            bool past = day < realNowUtc.Date;
            int minRec = past ? 150 : 5;
            // Today's file grows through the day; a past day is final some time after the day ends.
            return Get(day, url, "wh_brdc_" + Y(day) + Doy(day) + ".gz", true, 120, null,
                delegate() { return FtpDownload(url, 15000, 2); }, delegate(byte[] d) { return ValidGz2(d, minRec); });
        }

        List<BcRec> LoadNavDayWuhan(DateTime day)
        {
            byte[] data = GetWuhan(day);
            Iono io; int skipped;
            List<BcRec> recs = Rinex.Parse2(Rinex.GzTextLenient(data), out io, out skipped);
            if (recs.Count == 0) throw new NavException("no valid GPS records in Wuhan file for " + day.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture));
            if (skipped > 0) log("  " + day.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ": skipped " + skipped + " malformed/implausible records");
            return recs;
        }

        Iono LoadIonoWuhan(DateTime day, int maxBack)
        {
            string lastErr = "";
            for (int back = 0; back <= maxBack; back++)
            {
                DateTime d = day.AddDays(-back);
                try
                {
                    byte[] data = GetWuhan(d);
                    Iono io; int sk;
                    Rinex.Parse2(Rinex.GzTextLenient(data), out io, out sk);
                    if (io != null) { io.Source = "wuhan wh_brdc_" + Y(d) + Doy(d) + ".gz header"; return io; }
                }
                catch (Exception ex) { lastErr = ex.Message; }
            }
            throw new NavException("no ionosphere (ION ALPHA/BETA) found in Wuhan files of the last " + (maxBack + 1) + " days (" + lastErr + ")");
        }

        public List<BcRec> LoadNavDay(DateTime day)
        {
            return Active == "wuhan" ? LoadNavDayWuhan(day) : LoadNavDayBkg(day);
        }

        // Ionosphere from the active source; if that fails and the mode is auto, from the other one.
        public Iono LoadIono(DateTime day, int maxBack)
        {
            string[] order = Mode == "auto" ? (Active == "wuhan" ? new string[] { "wuhan", "bkg" } : new string[] { "bkg", "wuhan" }) : new string[] { Active };
            string err = "";
            foreach (string s in order)
            {
                string keep = Active;
                Active = s;
                bool keepFuse = fuse;
                try { return s == "wuhan" ? LoadIonoWuhan(day, maxBack) : LoadIonoBkg(day, maxBack); }
                catch (NavException ex) { err += (err.Length > 0 ? "; " : "") + s + ": " + ex.Message; log("  ionosphere from " + s + " failed: " + ex.Message); }
                finally { Active = keep; fuse = keepFuse; }
            }
            throw new NavException(err);
        }

        // Loads all `days` from ONE source per run. Order: auto = bkg (short fuse) then wuhan; a source is abandoned when
        // a day needed by the run (<= lastDay, bar the first hours of a new UTC day) is unavailable. Result per day in
        // perDay (null = missing, reason in dayErr). Pass 1 refuses stale cache (prefer fresh data from the other
        // source); pass 2 is cache-only.
        public void LoadDays(List<DateTime> days, DateTime lastDay, DateTime nowUtc, out List<BcRec>[] perDay, out string[] dayErr)
        {
            string[] order = Mode == "bkg" ? new string[] { "bkg" } : Mode == "wuhan" ? new string[] { "wuhan" } : new string[] { "bkg", "wuhan" };
            bool origOffline = offline;
            string errs = "";
            for (int pass = 0; pass < (origOffline ? 1 : 2); pass++)
            {
                allowStale = origOffline || pass == 1;
                offline = origOffline || pass == 1;
                foreach (string src in order)
                {
                    Active = src;
                    fuse = Mode == "auto" && src == "bkg" && !offline;
                    List<BcRec>[] pd = new List<BcRec>[days.Count];
                    string[] de = new string[days.Count];
                    Func<int, bool> load = delegate(int i)
                    {
                        try { pd[i] = LoadNavDay(days[i]); log("  " + days[i].ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + ": " + pd[i].Count + " GPS records (" + src + ")"); return true; }
                        catch (Exception ex) { de[i] = ex.Message; return false; }
                    };
                    bool dead = false;
                    if (fuse)
                    {
                        // probe the first day that needs the network, alone, under the short fuse
                        for (int i = 0; i < days.Count; i++)
                        {
                            string p = Path.Combine(cache, BkgName(days[i], "brdc_S_"));
                            if (CacheFresh(days[i], p, true, 20)) continue;
                            if (i > 0 && days[i] == lastDay && (nowUtc - days[i]).TotalHours < 3.0) continue;
                            if (!load(i)) { dead = true; errs += (errs.Length > 0 ? "; " : "") + "bkg: " + de[i]; log("  BKG probe failed (" + de[i] + ") -- switching source"); }
                            break;
                        }
                    }
                    if (dead) continue;
                    Parallel.For(0, days.Count, delegate(int i) { if (pd[i] == null && de[i] == null) load(i); });
                    string miss = null;
                    for (int i = 0; i < days.Count; i++)
                    {
                        if (pd[i] != null) continue;
                        bool needed = days[i] <= lastDay;
                        bool tolerable = days[i] == lastDay && (nowUtc - days[i]).TotalHours < 3.0 && i > 0;
                        if (needed && !tolerable) { miss = days[i].ToString("yyyy-MM-dd", CultureInfo.InvariantCulture) + " unavailable: " + de[i]; break; }
                    }
                    if (miss != null)
                    {
                        errs += (errs.Length > 0 ? "; " : "") + src + ": " + miss;
                        log("  source " + src + " lacks a needed day (" + miss + ")");
                        continue;
                    }
                    log("navigation source: " + (src == "bkg" ? "BKG (igs.bkg.bund.de, RINEX 3)" : "Wuhan (igs.gnsswhu.cn ftp, RINEX 2)") + (allowStale && !origOffline ? " [cache only]" : origOffline ? " [offline, cache only]" : ""));
                    fuse = false;
                    perDay = pd; dayErr = de;
                    return;
                }
            }
            offline = origOffline;
            throw new NavException("navigation data unavailable from " + (Mode == "auto" ? "any source" : Mode) + " (" + errs + ")");
        }
    }
}
