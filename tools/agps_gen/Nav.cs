using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Text;
using System.Threading;

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
    }

    // Nav-file cache + BKG downloads.
    sealed class NavSource
    {
        public const string BkgBase = "https://igs.bkg.bund.de/root_ftp/IGS/BRDC/";
        readonly string cache;
        readonly bool offline;
        readonly DateTime realNowUtc;
        readonly Action<string> log;

        public NavSource(string cacheDir, bool offlineOnly, Action<string> logger)
        {
            cache = cacheDir; offline = offlineOnly; log = logger; realNowUtc = DateTime.UtcNow;
            Directory.CreateDirectory(cache);
            try { ServicePointManager.SecurityProtocol = SecurityProtocolType.Tls12; }
            catch (Exception) { ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072; }
        }

        public static DateTime DayStart(int year, int doy) { return new DateTime(year, 1, 1, 0, 0, 0, DateTimeKind.Utc).AddDays(doy - 1); }

        static string Url(DateTime day, string tmpl)
        {
            return BkgBase + day.ToString("yyyy", CultureInfo.InvariantCulture) + "/" + day.DayOfYear.ToString("000") + "/" +
                string.Format(tmpl, day.ToString("yyyy", CultureInfo.InvariantCulture), day.DayOfYear.ToString("000"));
        }

        public static byte[] Download(string url, int timeoutMs)
        {
            Exception last = null;
            for (int attempt = 1; attempt <= 3; attempt++)
            {
                try
                {
                    HttpWebRequest rq = (HttpWebRequest)WebRequest.Create(url);
                    rq.Timeout = timeoutMs; rq.ReadWriteTimeout = timeoutMs;
                    rq.UserAgent = "agps_gen/1.0";
                    using (HttpWebResponse rs = (HttpWebResponse)rq.GetResponse())
                    using (Stream s = rs.GetResponseStream())
                    using (MemoryStream o = new MemoryStream())
                    {
                        byte[] buf = new byte[65536];
                        int n;
                        while ((n = s.Read(buf, 0, buf.Length)) > 0) o.Write(buf, 0, n);
                        return o.ToArray();
                    }
                }
                catch (WebException ex)
                {
                    last = ex;
                    HttpWebResponse hr = ex.Response as HttpWebResponse;
                    if (hr != null && hr.StatusCode == HttpStatusCode.NotFound) throw new NavException("404 not found: " + url);
                    Thread.Sleep(1500 * attempt);
                }
                catch (IOException ex) { last = ex; Thread.Sleep(1500 * attempt); }
            }
            throw new NavException("download failed (" + (last == null ? "?" : last.Message) + "): " + url);
        }

        static bool ValidGz(byte[] data, int minRecords)
        {
            try
            {
                Iono io;
                return Rinex.Parse(Rinex.GzText(data), out io).Count >= minRecords;
            }
            catch (Exception) { return false; }
        }

        // Returns file bytes for the cache name, downloading when missing/stale. forceFresh: re-download if older than 15 min
        // (or downloaded before the UTC day was complete).
        byte[] Get(DateTime day, string tmpl, string cacheName, bool dayFile, string[] altNames, int minRecords)
        {
            string path = Path.Combine(cache, cacheName);
            bool have = File.Exists(path) && new FileInfo(path).Length > 0;
            bool fresh = false;
            if (have)
            {
                DateTime m = File.GetLastWriteTimeUtc(path);
                DateTime dayEnd = day.AddDays(1);
                if (m >= dayEnd.AddMinutes(20)) fresh = true;                       // downloaded after the day was complete
                else if (realNowUtc >= dayEnd.AddMinutes(20) && !dayFile) fresh = true;
                else if ((realNowUtc - m).TotalMinutes < 15) fresh = true;          // today's file: updated every 15 min
                if (realNowUtc < day) fresh = true;
            }
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
            string url = Url(day, tmpl);
            log("downloading " + url);
            try
            {
                byte[] data = Download(url, 60000);
                if (!ValidGz(data, minRecords)) throw new NavException("downloaded file is not a valid RINEX nav gzip: " + url);
                string tmp = path + ".part";
                File.WriteAllBytes(tmp, data);
                if (File.Exists(path)) File.Delete(path);
                File.Move(tmp, path);
                return data;
            }
            catch (NavException)
            {
                if (have) { log("download failed, using stale cache " + cacheName); return File.ReadAllBytes(path); }
                throw;
            }
        }

        public List<BcRec> LoadNavDay(DateTime day)
        {
            string name = "brdc_S_" + day.ToString("yyyy", CultureInfo.InvariantCulture) + day.DayOfYear.ToString("000") + ".gz";
            string alt1 = "nav_S_" + day.ToString("yyyy", CultureInfo.InvariantCulture) + day.DayOfYear.ToString("000") + ".gz";
            string alt2 = "hist_nav_S_" + day.ToString("yyyy", CultureInfo.InvariantCulture) + day.DayOfYear.ToString("000") + ".gz";
            byte[] data = Get(day, "BRDC00WRD_S_{0}{1}0000_01D_MN.rnx.gz", name, true, new string[] { alt1, alt2 }, 50);
            Iono io;
            return Rinex.Parse(Rinex.GzText(data), out io);
        }

        // Klobuchar terms from the newest BRDM (DLR) header that has them, searching back from `day`.
        public Iono LoadIono(DateTime day, int maxBack)
        {
            string lastErr = "";
            for (int back = 0; back <= maxBack; back++)
            {
                DateTime d = day.AddDays(-back);
                string name = "hist_brdm_" + d.ToString("yyyy", CultureInfo.InvariantCulture) + d.DayOfYear.ToString("000") + ".gz";
                try
                {
                    byte[] data = Get(d, "BRDM00DLR_S_{0}{1}0000_01D_MN.rnx.gz", name, false, null, 0);
                    Iono io;
                    Rinex.Parse(Rinex.GzText(data), out io);
                    if (io != null) { io.Source = name; return io; }
                }
                catch (Exception ex) { lastErr = ex.Message; }
            }
            throw new NavException("no ionosphere (GPSA/GPSB) source found in the last " + (maxBack + 1) + " days (" + lastErr + ")");
        }
    }
}
