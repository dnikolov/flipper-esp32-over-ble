using System;
using System.Threading.Tasks;

namespace AgpsGen
{
    static class K
    {
        public const double GM = 3.986005e14;          // IS-GPS-200 value (broadcast user algorithm)
        public const double GM_DYN = 3.986004418e14;   // physical value, for the dynamics
        public const double WE = 7.2921151467e-5;      // IS-GPS-200, broadcast algorithm only
        public const double WE_T = 7.292115855e-5;     // true Earth rotation rate, frame changes in the dynamics
        public const double RE = 6378137.0;
        public const double J2 = 1.08262668e-3;
        public const double GM_SUN = 1.32712440018e20;
        public const double GM_MOON = 4.9028e12;
        public const double GPS_PI = 3.1415926535898;  // IS-GPS-200 pi, used by the CASIC codec
        // GPS time = UTC + 18 s (valid since 2017-01-01). unix_utc = gps_abs + GPS_UNIX0 - LEAP.
        public const long GPS_UNIX0 = 315964800;       // unix time of the GPS epoch 1980-01-06 00:00:00
        public const int LEAP = 18;
    }

    sealed class BcRec
    {
        public int Svid;
        public double Af0, Af1, Af2, Iode, Crs, DeltaN, M0, Cuc, E, Cus, SqrtA, Toe, Cic, Omega0, Cis;
        public double I0, Crc, Omega, OmegaDot, Idot, Week, UraM, Health, Tgd, Iodc, Toc, ToeAbs;
        public BcRec Clone() { return (BcRec)MemberwiseClone(); }
    }

    static class Bc
    {
        public static double Kepler(double M, double e)
        {
            double E = M;
            for (int i = 0; i < 12; i++) E = E - (E - e * Math.Sin(E) - M) / (1 - e * Math.Cos(E));
            return E;
        }

        // IS-GPS-200 ECEF position at absolute GPS seconds t.
        public static void Pos(BcRec r, double t, double[] o)
        {
            double toeW = r.Toe;
            double tk = t - r.ToeAbs;
            double A = r.SqrtA * r.SqrtA;
            double n = Math.Sqrt(K.GM / (A * A * A)) + r.DeltaN;
            double M = r.M0 + n * tk;
            double E = Kepler(M, r.E);
            double nu = Math.Atan2(Math.Sqrt(1 - r.E * r.E) * Math.Sin(E), Math.Cos(E) - r.E);
            double phi = nu + r.Omega;
            double s2 = Math.Sin(2 * phi), c2 = Math.Cos(2 * phi);
            double u = phi + r.Cus * s2 + r.Cuc * c2;
            double rr = A * (1 - r.E * Math.Cos(E)) + r.Crs * s2 + r.Crc * c2;
            double i = r.I0 + r.Idot * tk + r.Cis * s2 + r.Cic * c2;
            double xp = rr * Math.Cos(u), yp = rr * Math.Sin(u);
            double Om = r.Omega0 + (r.OmegaDot - K.WE) * tk - K.WE * toeW;
            double co = Math.Cos(Om), so = Math.Sin(Om), ci = Math.Cos(i), si = Math.Sin(i);
            o[0] = xp * co - yp * ci * so;
            o[1] = xp * so + yp * ci * co;
            o[2] = yp * si;
        }

        public static double Dist(BcRec a, BcRec b, double t)
        {
            double[] p = new double[3], q = new double[3];
            Pos(a, t, p); Pos(b, t, q);
            double dx = p[0] - q[0], dy = p[1] - q[1], dz = p[2] - q[2];
            return Math.Sqrt(dx * dx + dy * dy + dz * dz);
        }
    }

    static class Ephem
    {
        static double Rad(double d) { return d * Math.PI / 180.0; }

        public static double JdUtc(double tabs) { return 2444244.5 + (tabs - K.LEAP) / 86400.0; }

        public static double Gmst(double tabs)
        {
            double jd = JdUtc(tabs);
            double T = (jd - 2451545.0) / 36525.0;
            double g = 280.46061837 + 360.98564736629 * (jd - 2451545.0) + 0.000387933 * T * T;
            g = g - 360.0 * Math.Floor(g / 360.0);
            return Rad(g);
        }

        // Low-precision Sun (Montenbruck & Gill), mean equator, with precession in longitude J2000 -> of date.
        public static void Sun(double tabs, double[] o)
        {
            double T = (JdUtc(tabs) - 2451545.0) / 36525.0;
            double Mdeg = 357.5256 + 35999.049 * T;
            double M = Rad(Mdeg);
            double lon = Rad(282.9400 + 1.39700 * T + Mdeg + (6892.0 * Math.Sin(M) + 72.0 * Math.Sin(2 * M)) / 3600.0);
            double r = (149.619 - 2.499 * Math.Cos(M) - 0.021 * Math.Cos(2 * M)) * 1e9;
            double eps = Rad(23.43929111 - 0.01300417 * T);
            o[0] = r * Math.Cos(lon);
            o[1] = r * Math.Sin(lon) * Math.Cos(eps);
            o[2] = r * Math.Sin(lon) * Math.Sin(eps);
        }

        public static void Moon(double tabs, double[] o)
        {
            double T = (JdUtc(tabs) - 2451545.0) / 36525.0;
            double L0 = 218.31617 + 481267.88088 * T - 1.3972 * T;
            double l = Rad(134.96292 + 477198.86753 * T);
            double lp = Rad(357.52543 + 35999.04944 * T);
            double F = Rad(93.27283 + 483202.01873 * T);
            double D = Rad(297.85027 + 445267.11135 * T);
            double lon = L0 + (22640 * Math.Sin(l) + 769 * Math.Sin(2 * l) - 4586 * Math.Sin(l - 2 * D) + 2370 * Math.Sin(2 * D)
                - 668 * Math.Sin(lp) - 412 * Math.Sin(2 * F) - 212 * Math.Sin(2 * l - 2 * D) - 206 * Math.Sin(l + lp - 2 * D)
                + 192 * Math.Sin(l + 2 * D) - 165 * Math.Sin(lp - 2 * D) + 148 * Math.Sin(l - lp) - 125 * Math.Sin(D)
                - 110 * Math.Sin(l + lp) - 55 * Math.Sin(2 * F - 2 * D)) / 3600.0;
            double lat = (18520 * Math.Sin(F + Rad(lon - L0) + Rad((412 * Math.Sin(2 * F) + 541 * Math.Sin(lp)) / 3600.0))
                - 526 * Math.Sin(F - 2 * D) + 44 * Math.Sin(l + F - 2 * D) - 31 * Math.Sin(-l + F - 2 * D)
                - 25 * Math.Sin(-2 * l + F) - 23 * Math.Sin(lp + F - 2 * D) + 21 * Math.Sin(-l + F)
                + 11 * Math.Sin(-lp + F - 2 * D)) / 3600.0;
            double r = (385000 - 20905 * Math.Cos(l) - 3699 * Math.Cos(2 * D - l) - 2956 * Math.Cos(2 * D)
                - 570 * Math.Cos(2 * l) + 246 * Math.Cos(2 * l - 2 * D) - 205 * Math.Cos(lp - 2 * D)
                - 171 * Math.Cos(l + 2 * D) - 152 * Math.Cos(l + lp - 2 * D)) * 1e3;
            double lo = Rad(lon + 1.39700 * T), la = Rad(lat);
            double eps = Rad(23.43929111 - 0.01300417 * T);
            double x = r * Math.Cos(la) * Math.Cos(lo), y = r * Math.Cos(la) * Math.Sin(lo), z = r * Math.Sin(la);
            o[0] = x;
            o[1] = y * Math.Cos(eps) - z * Math.Sin(eps);
            o[2] = y * Math.Sin(eps) + z * Math.Cos(eps);
        }
    }

    // Sun and Moon positions on a fixed time grid (absolute GPS seconds), shared read-only by all propagations.
    sealed class SunMoonGrid
    {
        public readonly double Base, Dt;
        public readonly int N;
        public readonly double[] S;   // 6 doubles per node: sun xyz, moon xyz

        public SunMoonGrid(double b, double dt, int n)
        {
            Base = b; Dt = dt; N = n;
            S = new double[6 * n];
            double[] sref = S;
            Parallel.For(0, n, delegate(int i)
            {
                double t = b + i * dt;
                double[] s = new double[3], m = new double[3];
                Ephem.Sun(t, s); Ephem.Moon(t, m);
                for (int k = 0; k < 3; k++) { sref[6 * i + k] = s[k]; sref[6 * i + 3 + k] = m[k]; }
            });
        }

        public int Index(double t)
        {
            double f = (t - Base) / Dt;
            int i = (int)Math.Round(f);
            if (Math.Abs(f - i) > 1e-6 || i < 0 || i >= N) throw new InvalidOperationException("time off the Sun/Moon grid");
            return i;
        }
    }

    // Force model + fixed-step RK4 in the inertial frame that coincides with ECEF at the arc start T0.
    sealed class Prop
    {
        readonly SunMoonGrid g;
        public readonly double T0;
        readonly double h;
        readonly double c0, s0;
        readonly double[] y1 = new double[6], y2 = new double[6], y3 = new double[6], y4 = new double[6];
        readonly double[] k1 = new double[6], k2 = new double[6], k3 = new double[6], k4 = new double[6];

        public Prop(SunMoonGrid grid, double t0, double step)
        {
            g = grid; T0 = t0; h = step;
            double th = Ephem.Gmst(t0);
            c0 = Math.Cos(th); s0 = Math.Sin(th);
        }

        // ---------------- tesseral gravity (EGM96-like n<=4, fully normalised -> unnormalised) ----------------
        static readonly int[] TN = new int[] { 2, 3, 3, 3, 4, 4, 4, 4 };
        static readonly int[] TM = new int[] { 2, 1, 2, 3, 1, 2, 3, 4 };
        static readonly double[] TC = MakeT(true);
        static readonly double[] TS = MakeT(false);

        static double Fact(int n) { double f = 1; for (int i = 2; i <= n; i++) f *= i; return f; }

        static double[] MakeT(bool cosine)
        {
            double[] c = new double[] { 2.43914e-6, 2.03046e-6, 9.04706e-7, 7.21145e-7, -5.36e-7, 3.50e-7, 9.90e-7, -1.88e-7 };
            double[] s = new double[] { -1.40027e-6, 2.48513e-7, -6.19006e-7, 1.41436e-6, -4.74e-7, 6.63e-7, -2.01e-7, 3.09e-7 };
            double[] o = new double[8];
            for (int i = 0; i < 8; i++)
            {
                int n = TN[i], m = TM[i];
                double nm = Math.Sqrt((m == 0 ? 1 : 2) * (2 * n + 1) * Fact(n - m) / Fact(n + m));
                o[i] = (cosine ? c[i] : s[i]) * nm;
            }
            return o;
        }

        static double TessV(double x, double y, double z)
        {
            double rho2 = x * x + y * y;
            double r = Math.Sqrt(rho2 + z * z);
            double rho = Math.Sqrt(rho2);
            double sn = z / r, cs = rho / r;
            double c1 = x / rho, s1 = y / rho;
            double c2 = c1 * c1 - s1 * s1, s2 = 2 * c1 * s1;
            double c3 = c2 * c1 - s2 * s1, s3 = s2 * c1 + c2 * s1;
            double c4 = c2 * c2 - s2 * s2, s4 = 2 * c2 * s2;
            double[] cm = new double[] { 1, c1, c2, c3, c4 };
            double[] sm = new double[] { 0, s1, s2, s3, s4 };
            double q = K.RE / r;
            double q2 = q * q, q3 = q2 * q, q4 = q3 * q;
            double cs2 = cs * cs;
            double[] qn = new double[] { 0, 0, q2, q3, q4 };
            double[] pm = new double[8];
            pm[0] = 3 * cs2;
            pm[1] = 1.5 * cs * (5 * sn * sn - 1);
            pm[2] = 15 * sn * cs2;
            pm[3] = 15 * cs2 * cs;
            pm[4] = 2.5 * cs * (7 * sn * sn * sn - 3 * sn);
            pm[5] = 7.5 * cs2 * (7 * sn * sn - 1);
            pm[6] = 105 * sn * cs2 * cs;
            pm[7] = 105 * cs2 * cs2;
            double V = 0;
            for (int i = 0; i < 8; i++)
                V += qn[TN[i]] * pm[i] * (TC[i] * cm[TM[i]] + TS[i] * sm[TM[i]]);
            return K.GM_DYN / r * V;
        }

        static void TessGrad(double x, double y, double z, double[] o)
        {
            const double hh = 50.0;
            o[0] = (TessV(x + hh, y, z) - TessV(x - hh, y, z)) / (2 * hh);
            o[1] = (TessV(x, y + hh, z) - TessV(x, y - hh, z)) / (2 * hh);
            o[2] = (TessV(x, y, z + hh) - TessV(x, y, z - hh)) / (2 * hh);
        }

        readonly double[] tg = new double[3];

        // dy = f(t, y). p: 7 accelerations in m/s^2 [D, Pxc, Pxs, Pyc, Pys, Pzc, Pzs]. node: Sun/Moon grid index of t.
        void Deriv(double t, int node, double[] y, double[] p, double[] dy)
        {
            double x = y[0], yy = y[1], z = y[2];
            double rn2 = x * x + yy * yy + z * z;
            double rn = Math.Sqrt(rn2);
            double r3 = rn2 * rn;
            double f = -K.GM_DYN / r3;
            double ax = f * x, ay = f * yy, az = f * z;

            double sx = z / rn;
            double gsx = -z * x / r3, gsy = -z * yy / r3, gsz = 1.0 / rn - z * z / r3;
            double inv = 1.0 / rn;
            double ip3 = inv * inv * inv;   // rn^-3
            double gmre2 = K.GM_DYN * K.RE * K.RE, gmre3 = gmre2 * K.RE, gmre4 = gmre3 * K.RE;
            {   // J2
                double P = 0.5 * (3 * sx * sx - 1), dP = 3 * sx;
                double Vr = gmre2 * K.J2 * 3 * ip3 * inv * P;       // rn^-4
                double Vs = -gmre2 * K.J2 * ip3 * dP;               // rn^-3
                ax += Vr * x * inv + Vs * gsx; ay += Vr * yy * inv + Vs * gsy; az += Vr * z * inv + Vs * gsz;
            }
            {   // J3
                double P = 0.5 * (5 * sx * sx * sx - 3 * sx), dP = 0.5 * (15 * sx * sx - 3);
                double Vr = gmre3 * -2.5327e-6 * 4 * ip3 * inv * inv * P;   // rn^-5
                double Vs = -gmre3 * -2.5327e-6 * ip3 * inv * dP;           // rn^-4
                ax += Vr * x * inv + Vs * gsx; ay += Vr * yy * inv + Vs * gsy; az += Vr * z * inv + Vs * gsz;
            }
            {   // J4
                double s2 = sx * sx;
                double P = (35 * s2 * s2 - 30 * s2 + 3) / 8, dP = (140 * s2 * sx - 60 * sx) / 8;
                double Vr = gmre4 * -1.6196e-6 * 5 * ip3 * ip3 * P;         // rn^-6
                double Vs = -gmre4 * -1.6196e-6 * ip3 * inv * inv * dP;     // rn^-5
                ax += Vr * x * inv + Vs * gsx; ay += Vr * yy * inv + Vs * gsy; az += Vr * z * inv + Vs * gsz;
            }

            double th = K.WE_T * (t - T0);
            double c = Math.Cos(th), s = Math.Sin(th);
            TessGrad(c * x + s * yy, -s * x + c * yy, z, tg);
            ax += c * tg[0] - s * tg[1]; ay += s * tg[0] + c * tg[1]; az += tg[2];

            double[] S = g.S;
            int b = 6 * node;
            double sux = c0 * S[b] + s0 * S[b + 1], suy = -s0 * S[b] + c0 * S[b + 1], suz = S[b + 2];
            double mox = c0 * S[b + 3] + s0 * S[b + 4], moy = -s0 * S[b + 3] + c0 * S[b + 4], moz = S[b + 5];
            {
                double dx = sux - x, dy2 = suy - yy, dz = suz - z;
                double dn = Math.Sqrt(dx * dx + dy2 * dy2 + dz * dz), dn3 = dn * dn * dn;
                double bn = Math.Sqrt(sux * sux + suy * suy + suz * suz), bn3 = bn * bn * bn;
                ax += K.GM_SUN * (dx / dn3 - sux / bn3); ay += K.GM_SUN * (dy2 / dn3 - suy / bn3); az += K.GM_SUN * (dz / dn3 - suz / bn3);
                ax += p[0] * dx / dn; ay += p[0] * dy2 / dn; az += p[0] * dz / dn;   // D: unit vector sat -> Sun
            }
            {
                double dx = mox - x, dy2 = moy - yy, dz = moz - z;
                double dn = Math.Sqrt(dx * dx + dy2 * dy2 + dz * dz), dn3 = dn * dn * dn;
                double bn = Math.Sqrt(mox * mox + moy * moy + moz * moz), bn3 = bn * bn * bn;
                ax += K.GM_MOON * (dx / dn3 - mox / bn3); ay += K.GM_MOON * (dy2 / dn3 - moy / bn3); az += K.GM_MOON * (dz / dn3 - moz / bn3);
            }
            double ph = K.WE_T * t;
            double cp = Math.Cos(ph), sp = Math.Sin(ph);
            ax += cp * p[1] + sp * p[2]; ay += cp * p[3] + sp * p[4]; az += cp * p[5] + sp * p[6];

            dy[0] = y[3]; dy[1] = y[4]; dy[2] = y[5];
            dy[3] = ax; dy[4] = ay; dy[5] = az;
        }

        // Integrate nSteps RK4 steps of size h from y0 at T0; store the state every `every` steps (including step 0).
        public double[] Integrate(double[] y0, double[] p, int nSteps, int every)
        {
            int nOut = nSteps / every + 1;
            double[] outv = new double[6 * nOut];
            double[] y = new double[6];
            Array.Copy(y0, y, 6);
            int node = g.Index(T0);
            int half = 1;   // grid spacing is h/2
            Array.Copy(y, 0, outv, 0, 6);
            for (int st = 1; st <= nSteps; st++)
            {
                double t = T0 + (st - 1) * h;
                Deriv(t, node, y, p, k1);
                for (int i = 0; i < 6; i++) y1[i] = y[i] + 0.5 * h * k1[i];
                Deriv(t + 0.5 * h, node + half, y1, p, k2);
                for (int i = 0; i < 6; i++) y2[i] = y[i] + 0.5 * h * k2[i];
                Deriv(t + 0.5 * h, node + half, y2, p, k3);
                for (int i = 0; i < 6; i++) y3[i] = y[i] + h * k3[i];
                Deriv(t + h, node + 2 * half, y3, p, k4);
                for (int i = 0; i < 6; i++) y[i] += h / 6.0 * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]);
                node += 2 * half;
                if (st % every == 0) Array.Copy(y, 0, outv, 6 * (st / every), 6);
            }
            return outv;
        }

        // ECI_F position -> ECEF at time t
        public void ToEcef(double[] st, int off, double t, double[] o)
        {
            double th = K.WE_T * (t - T0);
            double c = Math.Cos(th), s = Math.Sin(th);
            o[0] = c * st[off] + s * st[off + 1];
            o[1] = -s * st[off] + c * st[off + 1];
            o[2] = st[off + 2];
        }

        // ECEF position/velocity -> ECI_F state at the arc start (th = 0 -> frames coincide)
        public static double[] EcefToEciAtStart(double[] p, double[] v)
        {
            return new double[] { p[0], p[1], p[2], v[0] - K.WE_T * p[1], v[1] + K.WE_T * p[0], v[2] };
        }
    }
}
