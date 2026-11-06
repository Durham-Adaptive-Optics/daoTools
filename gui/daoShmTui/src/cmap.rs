//! Colour maps (256-entry tables) and display scales.

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Cmap {
    Viridis,
    Inferno,
    Magma,
    Plasma,
    Turbo,
    Gray,
    Hot,
    Coolwarm,
}

pub const CMAPS: [Cmap; 8] =
    [Cmap::Viridis, Cmap::Inferno, Cmap::Magma, Cmap::Plasma, Cmap::Turbo, Cmap::Gray, Cmap::Hot, Cmap::Coolwarm];

impl Cmap {
    pub fn name(self) -> &'static str {
        match self {
            Cmap::Viridis => "viridis",
            Cmap::Inferno => "inferno",
            Cmap::Magma => "magma",
            Cmap::Plasma => "plasma",
            Cmap::Turbo => "turbo",
            Cmap::Gray => "gray",
            Cmap::Hot => "hot",
            Cmap::Coolwarm => "coolwarm",
        }
    }
    pub fn next(self) -> Cmap {
        let i = CMAPS.iter().position(|&c| c == self).unwrap_or(0);
        CMAPS[(i + 1) % CMAPS.len()]
    }
    pub fn from_name(s: &str) -> Option<Cmap> {
        CMAPS.iter().copied().find(|c| c.name() == s)
    }

    fn eval(self, t: f64) -> [f64; 3] {
        match self {
            Cmap::Viridis => poly(&VIRIDIS, t),
            Cmap::Inferno => poly(&INFERNO, t),
            Cmap::Magma => poly(&MAGMA, t),
            Cmap::Plasma => poly(&PLASMA, t),
            Cmap::Turbo => turbo(t),
            Cmap::Gray => [t, t, t],
            Cmap::Hot => [(3.0 * t).clamp(0.0, 1.0), (3.0 * t - 1.0).clamp(0.0, 1.0), (3.0 * t - 2.0).clamp(0.0, 1.0)],
            Cmap::Coolwarm => lerp(
                &[[0.230, 0.299, 0.754], [0.552, 0.690, 0.996], [0.865, 0.865, 0.865], [0.958, 0.604, 0.482], [0.706, 0.016, 0.150]],
                t,
            ),
        }
    }

    /// 256 RGB entries
    pub fn table(self, reversed: bool) -> Vec<(u8, u8, u8)> {
        (0..256)
            .map(|i| {
                let t = i as f64 / 255.0;
                let [r, g, b] = self.eval(if reversed { 1.0 - t } else { t });
                let q = |v: f64| (v.clamp(0.0, 1.0) * 255.0).round() as u8;
                (q(r), q(g), q(b))
            })
            .collect()
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Scale {
    Lin,
    Sqrt,
    Log,
    Asinh,
}

impl Scale {
    pub fn name(self) -> &'static str {
        match self {
            Scale::Lin => "linear",
            Scale::Sqrt => "sqrt",
            Scale::Log => "log",
            Scale::Asinh => "asinh",
        }
    }
    pub fn next(self) -> Scale {
        match self {
            Scale::Lin => Scale::Sqrt,
            Scale::Sqrt => Scale::Log,
            Scale::Log => Scale::Asinh,
            Scale::Asinh => Scale::Lin,
        }
    }
    /// t in [0, 1] -> [0, 1]
    pub fn apply(self, t: f64) -> f64 {
        match self {
            Scale::Lin => t,
            Scale::Sqrt => t.sqrt(),
            Scale::Log => (1.0 + 1000.0 * t).log10() / 3.0,
            Scale::Asinh => (10.0 * t).asinh() / 10f64.asinh(),
        }
    }
}

fn poly(c: &[[f64; 3]; 7], t: f64) -> [f64; 3] {
    let mut out = [0.0; 3];
    for (k, o) in out.iter_mut().enumerate() {
        let mut v = c[6][k];
        for i in (0..6).rev() {
            v = c[i][k] + t * v;
        }
        *o = v;
    }
    out
}

fn turbo(t: f64) -> [f64; 3] {
    let (t2, t3) = (t * t, t * t * t);
    let (t4, t5) = (t3 * t, t3 * t2);
    [
        0.13572138 + 4.61539260 * t - 42.66032258 * t2 + 132.13108234 * t3 - 152.94239396 * t4 + 59.28637943 * t5,
        0.09140261 + 2.19418839 * t + 4.84296658 * t2 - 14.18503333 * t3 + 4.27729857 * t4 + 2.82956604 * t5,
        0.10667330 + 12.64194608 * t - 60.58204836 * t2 + 110.36276771 * t3 - 89.90310912 * t4 + 27.34824973 * t5,
    ]
}

fn lerp(stops: &[[f64; 3]], t: f64) -> [f64; 3] {
    let n = stops.len() - 1;
    let x = t.clamp(0.0, 1.0) * n as f64;
    let i = (x.floor() as usize).min(n - 1);
    let f = x - i as f64;
    [0, 1, 2].map(|k| stops[i][k] + (stops[i + 1][k] - stops[i][k]) * f)
}

const VIRIDIS: [[f64; 3]; 7] = [
    [0.2777273272234177, 0.005407344544966578, 0.3340998053353061],
    [0.1050930431085774, 1.404613529898575, 1.384590162594685],
    [-0.3308618287255563, 0.214847559468213, 0.09509516302823659],
    [-4.634230498983486, -5.799100973351585, -19.33244095627987],
    [6.228269936347081, 14.17993336680509, 56.69055260068105],
    [4.776384997670288, -13.74514537774601, -65.35303263337234],
    [-5.435455855934631, 4.645852612178535, 26.3124352495832],
];
const PLASMA: [[f64; 3]; 7] = [
    [0.05873234392399702, 0.02333670892565664, 0.5433401826748754],
    [2.176514634195958, 0.2383834171260182, 0.7539604599784036],
    [-2.689460476458034, -7.455851135738909, 3.110799939717086],
    [6.130348345893603, 42.3461881477227, -28.51885465332158],
    [-11.10743619062271, -82.66631109428045, 60.13984767418263],
    [10.02306557647065, 71.41361770095349, -54.07218655560067],
    [-3.658713842777788, -22.93153465461149, 18.19190778539828],
];
const MAGMA: [[f64; 3]; 7] = [
    [-0.002136485053939582, -0.000749655052795221, -0.005386127855323933],
    [0.2516605407371642, 0.6775232436837668, 2.494026599312351],
    [8.353717279216625, -3.577719514958484, 0.3144679030132573],
    [-27.66873308576866, 14.26473078096533, -13.64921318813922],
    [52.17613981234068, -27.94360607168351, 12.94416944238394],
    [-50.76852536473588, 29.04658282127291, 4.23415299384598],
    [18.65570506591883, -11.48977351997711, -5.601961508734096],
];
const INFERNO: [[f64; 3]; 7] = [
    [0.0002189403691192265, 0.001651004631001012, -0.01948089843709184],
    [0.1065134194856116, 0.5639564367884091, 3.932712388889277],
    [11.60249308247187, -3.972853965665698, -15.9423941062914],
    [-41.70399613139459, 17.43639888205313, 44.35414519872813],
    [77.162935699427, -33.40235894210092, -81.80730925738993],
    [-71.31942824499214, 32.62606426397723, 73.20951985803202],
    [25.13112622477341, -12.24266895238567, -23.07032500287172],
];
