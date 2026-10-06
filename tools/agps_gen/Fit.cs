using System;
using System.Collections.Generic;
using System.Threading.Tasks;

namespace AgpsGen
{
    delegate double[] ResFn(double[] x);

    static class Lin
    {
        // In-place Cholesky solve of A x = b (A symmetric positive definite, n x n, row-major). Returns false if not PD.
        public static bool CholSolve(double[] A, double[] b, int n, double[] x)
        {
            double[] L = new double[n * n];
            for (int i = 0; i < n; i++)
            {
                for (int j = 0; j <= i; j++)
                {
                    double s = A[i * n + j];
                    for (int k = 0; k < j; k++) s -= L[i * n + k] * L[j * n + k];
                    if (i == j)
                    {
                        if (!(s > 0) || double.IsNaN(s)) return false;
                        L[i * n + i] = Math.Sqrt(s);
                    }
                    else L[i * n + j] = s / L[j * n + j];
                }
            }
            double[] z = new double[n];
            for (int i = 0; i < n; i++)
            {
                double s = b[i];
                for (int k = 0; k < i; k++) s -= L[i * n + k] * z[k];
                z[i] = s / L[i * n + i];
            }
            for (int i = n - 1; i >= 0; i--)
            {
                double s = z[i];
                for (int k = i + 1; k < n; k++) s -= L[k * n + i] * x[k];
                x[i] = s / L[i * n + i];
            }
            return true;
        }
    }

    sealed class LmResult
    {
        public double[] X;
        public double Cost;      // sum of squares
        public int Iterations, Evals;
    }

    static class Lm
    {
        static double Dot(double[] a) { double s = 0; for (int i = 0; i < a.Length; i++) s += a[i] * a[i]; return s; }

        // Levenberg-Marquardt (diagonal scaling) with a finite-difference Jacobian.
        // step_j = baseStep * (relToX ? max(1,|x_j|) : 1); central = true -> central differences.
        public static LmResult Run(ResFn f, double[] x0, double baseStep, bool relToX, bool central, int maxIter, double relTol, bool parallelCols)
        {
            int n = x0.Length;
            double[] x = (double[])x0.Clone();
            double[] r = f(x);
            int m = r.Length;
            double cost = Dot(r);
            double lambda = 1e-3;
            int evals = 1, it;
            for (it = 0; it < maxIter; it++)
            {
                double[][] cols = new double[n][];
                double[] steps = new double[n];
                for (int j = 0; j < n; j++) steps[j] = baseStep * (relToX ? Math.Max(1.0, Math.Abs(x[j])) : 1.0);
                double[] xc = x; double[] rc = r;
                Action<int> col = delegate(int j)
                {
                    double[] xp = (double[])xc.Clone();
                    xp[j] += steps[j];
                    double[] rp = f(xp);
                    double[] c = new double[m];
                    if (central)
                    {
                        double[] xm = (double[])xc.Clone();
                        xm[j] -= steps[j];
                        double[] rm = f(xm);
                        for (int i = 0; i < m; i++) c[i] = (rp[i] - rm[i]) / (2 * steps[j]);
                    }
                    else for (int i = 0; i < m; i++) c[i] = (rp[i] - rc[i]) / steps[j];
                    cols[j] = c;
                };
                if (parallelCols) Parallel.For(0, n, col); else for (int j = 0; j < n; j++) col(j);
                evals += n * (central ? 2 : 1);
                double[] JtJ = new double[n * n];
                double[] g = new double[n];
                for (int a = 0; a < n; a++)
                {
                    double s = 0;
                    for (int i = 0; i < m; i++) s += cols[a][i] * r[i];
                    g[a] = s;
                    for (int b = 0; b <= a; b++)
                    {
                        double t = 0;
                        for (int i = 0; i < m; i++) t += cols[a][i] * cols[b][i];
                        JtJ[a * n + b] = t; JtJ[b * n + a] = t;
                    }
                }
                bool accepted = false;
                double prevCost = cost;
                for (int tries = 0; tries < 30; tries++)
                {
                    double[] A = (double[])JtJ.Clone();
                    for (int a = 0; a < n; a++) A[a * n + a] += lambda * Math.Max(JtJ[a * n + a], 1e-12);
                    double[] neg = new double[n], dx = new double[n];
                    for (int a = 0; a < n; a++) neg[a] = -g[a];
                    if (!Lin.CholSolve(A, neg, n, dx)) { lambda *= 10; continue; }
                    double[] xn = new double[n];
                    for (int a = 0; a < n; a++) xn[a] = x[a] + dx[a];
                    double[] rn = f(xn); evals++;
                    double cn = Dot(rn);
                    if (!double.IsNaN(cn) && cn < cost)
                    {
                        x = xn; r = rn; cost = cn; lambda = Math.Max(lambda / 4, 1e-12); accepted = true;
                        break;
                    }
                    lambda *= 4;
                    if (lambda > 1e12) break;
                }
                if (!accepted) break;
                if (prevCost - cost <= relTol * prevCost) { it++; break; }
            }
            LmResult res = new LmResult();
            res.X = x; res.Cost = cost; res.Iterations = it; res.Evals = evals;
            return res;
        }
    }

    sealed class OrbitResult
    {
        public double[] Times;      // absolute GPS seconds, every 300 s from T0
        public double[] Ecef;       // 3 per time
        public double FitRmsM;      // rms 3D residual of the dynamic fit (per epoch)
        public double FitMaxM;
        public double ArcSeconds;
        public int Epochs, Iterations;
        public double[] X;
        public string Note;
    }

    static class OrbitFitter
    {
        public const double FitEpoch = 900.0;
        public const double SampleDt = 300.0;

        // healthy: healthy broadcast records of one SV with ToeAbs <= T0 (sorted by toe).
        public static OrbitResult Fit(List<BcRec> healthy, double T0, double wMax, double h, SunMoonGrid grid, double horizonSec, double minArc)
        {
            OrbitResult res = new OrbitResult();
            double W = wMax;
            for (int i = 0; i + 1 < healthy.Count; i++)
            {
                BcRec a = healthy[i], b = healthy[i + 1];
                if (b.ToeAbs > T0 - W && Bc.Dist(a, b, b.ToeAbs) > 150.0) W = T0 - b.ToeAbs + 3600.0;
            }
            W = Math.Ceiling(W / FitEpoch) * FitEpoch;
            if (W > wMax) W = wMax;
            res.ArcSeconds = W;
            if (W < minArc) { res.Note = "orbit manoeuvre/discontinuity within the last " + (W / 3600.0).ToString("0.0") + " h (fit arc too short)"; return res; }
            double t0 = T0 - W;
            int nEp = (int)Math.Round(W / FitEpoch) + 1;
            res.Epochs = nEp;
            double[] ts = new double[nEp];
            double[] obs = new double[3 * nEp];
            double[] tmp = new double[3];
            int gaps = 0;
            for (int j = 0; j < nEp; j++)
            {
                ts[j] = t0 + FitEpoch * j;
                int bi = 0; double bd = double.MaxValue;
                for (int k = 0; k < healthy.Count; k++)
                {
                    double d = Math.Abs(healthy[k].ToeAbs - ts[j]);
                    if (d < bd) { bd = d; bi = k; }
                }
                if (bd > 4 * 3600.0) gaps++;
                Bc.Pos(healthy[bi], ts[j], tmp);
                obs[3 * j] = tmp[0]; obs[3 * j + 1] = tmp[1]; obs[3 * j + 2] = tmp[2];
            }
            if (gaps > nEp / 10) { res.Note = "broadcast data gaps (" + gaps + " of " + nEp + " epochs > 4 h from a record)"; return res; }

            // initial state: nearest-toe record at t0, velocity by central difference over +-0.5 s
            int ni = 0; double nd = double.MaxValue;
            for (int k = 0; k < healthy.Count; k++)
            {
                double d = Math.Abs(healthy[k].ToeAbs - t0);
                if (d < nd) { nd = d; ni = k; }
            }
            double[] p0 = new double[3], pa = new double[3], pb = new double[3], v0 = new double[3];
            Bc.Pos(healthy[ni], t0, p0); Bc.Pos(healthy[ni], t0 + 0.5, pa); Bc.Pos(healthy[ni], t0 - 0.5, pb);
            for (int k = 0; k < 3; k++) v0[k] = pa[k] - pb[k];
            double[] y0 = Prop.EcefToEciAtStart(p0, v0);

            int nSteps = (int)Math.Round(W / h);
            int every = (int)Math.Round(FitEpoch / h);
            double[] y0c = y0;
            ResFn resid = delegate(double[] x)
            {
                double[] y = new double[6];
                for (int k = 0; k < 3; k++) { y[k] = y0c[k] + x[k]; y[3 + k] = y0c[3 + k] + x[3 + k] * 1e-3; }
                double[] p = new double[7];
                for (int k = 0; k < 7; k++) p[k] = x[6 + k] * 1e-9;
                Prop pr = new Prop(grid, t0, h);
                double[] st = pr.Integrate(y, p, nSteps, every);
                double[] o = new double[3 * nEp];
                double[] e = new double[3];
                for (int j = 0; j < nEp; j++)
                {
                    pr.ToEcef(st, 6 * j, ts[j], e);
                    o[3 * j] = e[0] - obs[3 * j]; o[3 * j + 1] = e[1] - obs[3 * j + 1]; o[3 * j + 2] = e[2] - obs[3 * j + 2];
                }
                return o;
            };
            LmResult lm = Lm.Run(resid, new double[13], 0.1, true, false, 15, 1e-6, true);
            res.X = lm.X;
            res.Iterations = lm.Iterations;
            res.FitRmsM = Math.Sqrt(lm.Cost / nEp);
            double[] rr = resid(lm.X);
            double mx = 0;
            for (int j = 0; j < nEp; j++) mx = Math.Max(mx, Math.Sqrt(rr[3 * j] * rr[3 * j] + rr[3 * j + 1] * rr[3 * j + 1] + rr[3 * j + 2] * rr[3 * j + 2]));
            res.FitMaxM = mx;

            // propagate
            double[] yf = new double[6];
            for (int k = 0; k < 3; k++) { yf[k] = y0[k] + lm.X[k]; yf[3 + k] = y0[3 + k] + lm.X[3 + k] * 1e-3; }
            double[] pf = new double[7];
            for (int k = 0; k < 7; k++) pf[k] = lm.X[6 + k] * 1e-9;
            Prop pp = new Prop(grid, t0, h);
            int total = (int)Math.Round((W + horizonSec) / h);
            int ev2 = (int)Math.Round(SampleDt / h);
            double[] st2 = pp.Integrate(yf, pf, total, ev2);
            int first = (int)Math.Round(W / SampleDt);
            int nOut = total / ev2 + 1 - first;
            res.Times = new double[nOut];
            res.Ecef = new double[3 * nOut];
            double[] ec = new double[3];
            for (int j = 0; j < nOut; j++)
            {
                double t = T0 + SampleDt * j;
                res.Times[j] = t;
                pp.ToEcef(st2, 6 * (first + j), t, ec);
                res.Ecef[3 * j] = ec[0]; res.Ecef[3 * j + 1] = ec[1]; res.Ecef[3 * j + 2] = ec[2];
            }
            return res;
        }
    }

    sealed class WinFit
    {
        public BcRec Rec;
        public double Rms, Max;
    }

    static class WindowFitter
    {
        static readonly string[] Names = new string[] { "sqrtA", "e", "M0", "omega", "i0", "Omega0", "DeltaN", "OmegaDot", "IDOT", "Crs", "Crc", "Cus", "Cuc", "Cis", "Cic" };
        static readonly double[] Scale = new double[] { 0.05, 1e-4, 1e-4, 1e-4, 1e-4, 1e-4, 1e-10, 1e-11, 1e-12, 5.0, 5.0, 1e-6, 1e-6, 1e-6, 1e-6 };

        static void Cross(double[] a, double[] b, double[] o)
        {
            o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
        }
        static double Dot3(double[] a, double[] b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        static double Norm(double[] a) { return Math.Sqrt(Dot3(a, a)); }

        // cubic fit of pos(t) around toe over the points with |t-toe| <= 900 s -> position and velocity at toe
        static void StateAtToe(double[] ts, double[] pos, int n, double toe, double[] p0, double[] v0)
        {
            List<int> sel = new List<int>();
            for (int i = 0; i < n; i++) if (Math.Abs(ts[i] - toe) <= 900) sel.Add(i);
            double[] A = new double[16];
            for (int c = 0; c < 3; c++)
            {
                for (int i = 0; i < 16; i++) A[i] = 0;
                double[] bb = new double[4];
                for (int q = 0; q < sel.Count; q++)
                {
                    int i = sel[q];
                    double u = (ts[i] - toe) / 900.0;
                    double[] pw = new double[] { 1, u, u * u, u * u * u };
                    for (int a = 0; a < 4; a++)
                    {
                        bb[a] += pw[a] * pos[3 * i + c];
                        for (int b = 0; b < 4; b++) A[a * 4 + b] += pw[a] * pw[b];
                    }
                }
                double[] co = new double[4];
                if (!Lin.CholSolve(A, bb, 4, co)) throw new InvalidOperationException("polyfit failed");
                p0[c] = co[0]; v0[c] = co[1] / 900.0;
            }
        }

        static void KeplerFromState(double[] p, double[] v, out double a, out double e, out double inc, out double Om, out double w, out double M)
        {
            double[] vi = new double[] { v[0] - K.WE_T * p[1], v[1] + K.WE_T * p[0], v[2] };
            double r = Norm(p);
            double[] h = new double[3]; Cross(p, vi, h);
            double hn = Norm(h);
            a = 1 / (2 / r - Dot3(vi, vi) / K.GM);
            double[] ev = new double[3]; Cross(vi, h, ev);
            for (int k = 0; k < 3; k++) ev[k] = ev[k] / K.GM - p[k] / r;
            e = Norm(ev);
            inc = Math.Acos(h[2] / hn);
            double[] nv = new double[3]; Cross(new double[] { 0, 0, 1 }, h, nv);
            Om = Math.Atan2(nv[1], nv[0]);
            double[] t1 = new double[3]; Cross(nv, ev, t1);
            w = Math.Atan2(Dot3(t1, h) / hn, Dot3(nv, ev));
            double[] t2 = new double[3]; Cross(ev, p, t2);
            double nu = Math.Atan2(Dot3(t2, h) / hn, Dot3(ev, p));
            double E = 2 * Math.Atan2(Math.Sqrt(1 - e) * Math.Sin(nu / 2), Math.Sqrt(1 + e) * Math.Cos(nu / 2));
            M = E - e * Math.Sin(E);
        }

        static void SetParam(BcRec r, int idx, double v)
        {
            switch (idx)
            {
                case 0: r.SqrtA = v; break;
                case 1: r.E = v; break;
                case 2: r.M0 = v; break;
                case 3: r.Omega = v; break;
                case 4: r.I0 = v; break;
                case 5: r.Omega0 = v; break;
                case 6: r.DeltaN = v; break;
                case 7: r.OmegaDot = v; break;
                case 8: r.Idot = v; break;
                case 9: r.Crs = v; break;
                case 10: r.Crc = v; break;
                case 11: r.Cus = v; break;
                case 12: r.Cuc = v; break;
                case 13: r.Cis = v; break;
                default: r.Cic = v; break;
            }
        }

        // ts: absolute GPS times, pos: 3 per time (ECEF). Returns the 15-parameter broadcast-style fit centred on toeAbs.
        public static WinFit Fit(double[] ts, double[] pos, int n, double toeAbs)
        {
            double toeW = toeAbs - 604800.0 * Math.Floor(toeAbs / 604800.0);
            double[] p0 = new double[3], v0 = new double[3];
            StateAtToe(ts, pos, n, toeAbs, p0, v0);
            double a, e, inc, Om, w, M;
            KeplerFromState(p0, v0, out a, out e, out inc, out Om, out w, out M);
            double pp = a * (1 - e * e);
            double omdot0 = -1.5 * K.J2 * Math.Pow(K.RE / pp, 2) * Math.Sqrt(K.GM / (a * a * a)) * Math.Cos(inc);
            double[] b0 = new double[] { Math.Sqrt(a), e, M, w, inc, Om + K.WE * toeW, 0.0, omdot0, 0.0, 0, 0, 0, 0, 0, 0 };
            BcRec tmpl = new BcRec();
            tmpl.Toe = toeW; tmpl.ToeAbs = toeAbs;
            double[] bb = new double[3];
            ResFn resid = delegate(double[] x)
            {
                BcRec r = tmpl.Clone();
                for (int k = 0; k < 15; k++) SetParam(r, k, b0[k] + x[k] * Scale[k]);
                double[] o = new double[3 * n];
                double[] q = new double[3];
                for (int i = 0; i < n; i++)
                {
                    Bc.Pos(r, ts[i], q);
                    o[3 * i] = q[0] - pos[3 * i]; o[3 * i + 1] = q[1] - pos[3 * i + 1]; o[3 * i + 2] = q[2] - pos[3 * i + 2];
                }
                return o;
            };
            LmResult lm = Lm.Run(resid, new double[15], 1e-2, false, true, 60, 1e-13, false);
            BcRec fit = tmpl.Clone();
            for (int k = 0; k < 15; k++) SetParam(fit, k, b0[k] + lm.X[k] * Scale[k]);
            double[] rr = resid(lm.X);
            double mx = 0;
            for (int i = 0; i < n; i++) mx = Math.Max(mx, Math.Sqrt(rr[3 * i] * rr[3 * i] + rr[3 * i + 1] * rr[3 * i + 1] + rr[3 * i + 2] * rr[3 * i + 2]));
            WinFit wf = new WinFit();
            wf.Rec = fit; wf.Rms = Math.Sqrt(lm.Cost / n); wf.Max = mx;
            return wf;
        }

        // complete a fitted record with clock/health fields of the newest broadcast record, wrap angles
        public static BcRec ToCasicRec(BcRec fit, BcRec clk, int week)
        {
            BcRec r = fit.Clone();
            r.M0 = Casic.WrapPi(r.M0); r.Omega = Casic.WrapPi(r.Omega); r.Omega0 = Casic.WrapPi(r.Omega0);
            r.Week = week; r.Toc = fit.Toe;
            r.Af0 = clk.Af0; r.Af1 = clk.Af1; r.Af2 = clk.Af2; r.Tgd = clk.Tgd;
            r.Iodc = (double)(((long)clk.Iodc) & 0x3ff);
            r.UraM = clk.UraM; r.Health = 0; r.Svid = clk.Svid;
            return r;
        }
    }
}
