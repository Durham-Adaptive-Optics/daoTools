//! Drawing.

use ratatui::buffer::Buffer;
use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::symbols;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Axis, Block, BorderType, Borders, Cell, Chart, Clear, Dataset, GraphType, Paragraph, Row, Table, TableState, Wrap};
use ratatui::Frame;

use crate::app::{base, App, Focus, Mode, Popup, RangeMode, Strip, Trend};
use crate::tabs::{Tab, TABS};
use crate::tools::{self, RecState};
use ratatui::widgets::Gauge;
use crate::shm::now_epoch;

const ACCENT: Color = Color::Rgb(74, 158, 255);
const MUTED: Color = Color::DarkGray;
const CURSOR: (u8, u8, u8) = (255, 61, 240);
const NAN: (u8, u8, u8) = (128, 128, 128);

/* ------------------------------------------------------------------ format */

pub fn fmt(v: f64, int_like: bool) -> String {
    if v.is_nan() {
        return "NaN".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if int_like || (v.fract() == 0.0 && v.abs() < 1e15) {
        return format!("{}", v as i64);
    }
    let a = v.abs();
    if a != 0.0 && (a >= 1e6 || a < 1e-3) {
        return format!("{:.3e}", v);
    }
    let digits = (4 - a.log10().floor() as i32).clamp(0, 10) as usize;
    let s = format!("{:.*}", digits, v);
    if s.contains('.') {
        s.trim_end_matches('0').trim_end_matches('.').to_string()
    } else {
        s
    }
}

fn fmt_rate(r: f64) -> String {
    if r <= 0.0 {
        "0".into()
    } else if r >= 10000.0 {
        format!("{:.0}k", r / 1000.0)
    } else if r >= 1000.0 {
        format!("{:.1}k", r / 1000.0)
    } else if r >= 100.0 {
        format!("{:.0}", r)
    } else {
        format!("{:.1}", r)
    }
}

fn fmt_dur(s: f64) -> String {
    let a = s.abs();
    if a < 1e-3 {
        format!("{:.0} µs", s * 1e6)
    } else if a < 1.0 {
        format!("{:.1} ms", s * 1e3)
    } else if a < 120.0 {
        format!("{:.1} s", s)
    } else if a < 7200.0 {
        format!("{:.1} min", s / 60.0)
    } else if a < 172800.0 {
        format!("{:.1} h", s / 3600.0)
    } else {
        format!("{:.1} d", s / 86400.0)
    }
}

fn shape_str(s: &[usize]) -> String {
    s.iter().map(|v| v.to_string()).collect::<Vec<_>>().join("×")
}

/* ------------------------------------------------------------------ colours */

fn color(truecolor: bool, (r, g, b): (u8, u8, u8)) -> Color {
    if truecolor {
        Color::Rgb(r, g, b)
    } else {
        // the 6x6x6 cube of the 256-colour palette
        let q = |v: u8| ((v as u16 * 5 + 127) / 255) as u8;
        Color::Indexed(16 + 36 * q(r) + 6 * q(g) + q(b))
    }
}

fn luminance((r, g, b): (u8, u8, u8)) -> f64 {
    0.299 * r as f64 + 0.587 * g as f64 + 0.114 * b as f64
}

fn border_style(focused: bool) -> Style {
    if focused {
        Style::default().fg(ACCENT)
    } else {
        Style::default().fg(MUTED)
    }
}

/* ------------------------------------------------------------------ layout */

pub fn draw(f: &mut Frame, app: &mut App) {
    let area = f.area();
    let rows = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Min(5), Constraint::Length(2)])
        .split(area);
    draw_header(f, rows[0], app);
    let list_w = (area.width / 3).clamp(30, 52);
    let cols = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Length(list_w), Constraint::Min(20)])
        .split(rows[1]);
    draw_list(f, cols[0], app);
    let right = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Min(3)])
        .split(cols[1]);
    draw_tabs(f, right[0], app);
    match app.tab {
        Tab::View => draw_view(f, right[1], app),
        Tab::Sems => draw_sems(f, right[1], app),
        Tab::Record => draw_record(f, right[1], app),
        Tab::Latency => draw_latency(f, right[1], app),
        Tab::Snaps => draw_snaps(f, right[1], app),
        Tab::Tmux => draw_tmux(f, right[1], app),
        Tab::Daq => draw_daq(f, right[1], app),
    }
    draw_status(f, rows[2], app);
    match app.popup {
        Popup::Help => draw_help(f, area),
        Popup::Info => draw_info(f, area, app),
        Popup::None => {}
    }
    draw_prompt(f, area, app);
}

fn draw_header(f: &mut Frame, area: Rect, app: &App) {
    let dirs = app.reg.dirs.iter().map(|d| d.display().to_string()).collect::<Vec<_>>().join(", ");
    let mut spans = vec![
        Span::styled(" DAO SHM Viewer ", Style::default().fg(Color::Black).bg(ACCENT).add_modifier(Modifier::BOLD)),
        Span::raw(" "),
        Span::styled(format!("{} · {}", app.host, dirs), Style::default().fg(MUTED)),
    ];
    if app.t.write {
        spans.insert(1, Span::styled(" WRITE ", Style::default().fg(Color::Black).bg(Color::Yellow).add_modifier(Modifier::BOLD)));
    }
    if let Some((m, _)) = &app.message {
        spans.push(Span::raw("   "));
        spans.push(Span::styled(m.clone(), Style::default().fg(Color::Yellow)));
    }
    let right = Span::styled("? help · q quit ", Style::default().fg(MUTED));
    f.render_widget(Paragraph::new(Line::from(spans)), area);
    f.render_widget(Paragraph::new(Line::from(right)).alignment(Alignment::Right), area);
}

fn draw_list(f: &mut Frame, area: Rect, app: &mut App) {
    let vis = app.visible();
    let total = app.reg.files.len();
    let live = app.reg.files.iter().filter(|x| x.rate > 0.0).count();
    let rows: Vec<Row> = vis
        .iter()
        .map(|p| {
            let sf = app.reg.get(p);
            let (shape, dt, rate, gpu) = match sf {
                Some(s) => (shape_str(&s.shape), s.dtype.map(|d| d.name()).unwrap_or("?").to_string(), s.rate, s.gpu),
                None => (String::new(), String::new(), 0.0, false),
            };
            let shown = app.selected.as_ref() == Some(p);
            let name = format!("{}{}{}", if shown { "▶ " } else { "  " }, base(p), if gpu { " ·GPU" } else { "" });
            Row::new(vec![
                Cell::from(name),
                Cell::from(Span::styled(shape, Style::default().fg(MUTED))),
                Cell::from(Span::styled(dt.replace("float", "f").replace("uint", "u").replace("int", "i").replace("complex", "c"), Style::default().fg(MUTED))),
                Cell::from(Span::styled(
                    format!("{:>6}", fmt_rate(rate)),
                    Style::default().fg(if rate > 0.0 { Color::Green } else { MUTED }),
                )),
            ])
        })
        .collect();
    let title = if app.filtering || !app.filter.is_empty() {
        format!(" SHMs · filter: {}{} ", app.filter, if app.filtering { "▏" } else { "" })
    } else {
        format!(" SHMs {}/{} · {} updating ", vis.len(), total, live)
    };
    let sort = match app.sort {
        crate::app::Sort::Name => "name",
        crate::app::Sort::Rate => "Hz",
        crate::app::Sort::Size => "size",
    };
    let table = Table::new(rows, [Constraint::Min(10), Constraint::Length(9), Constraint::Length(4), Constraint::Length(6)])
        .header(Row::new(vec!["name", "shape", "type", "    Hz"]).style(Style::default().fg(MUTED).add_modifier(Modifier::BOLD)))
        .block(
            Block::default()
                .borders(Borders::ALL)
                .border_type(BorderType::Rounded)
                .border_style(border_style(app.focus == Focus::List))
                .title(title)
                .title_bottom(Line::from(format!(" sort: {} (s) ", sort)).right_aligned()),
        )
        .highlight_style(Style::default().bg(Color::Rgb(40, 50, 70)).add_modifier(Modifier::BOLD))
        .column_spacing(1);
    let mut state = TableState::default();
    if !vis.is_empty() {
        state.select(Some(app.list_sel.min(vis.len() - 1)));
    }
    f.render_stateful_widget(table, area, &mut state);
}

fn draw_view(f: &mut Frame, area: Rect, app: &mut App) {
    let mode = app.effective_mode();
    let title = match (&app.selected, &app.frame) {
        (Some(p), Some(fr)) => format!(
            " {} · {} {}{}{} ",
            base(p),
            shape_str(&fr.shape),
            fr.dtype.name(),
            if fr.depth > 0 { format!(" · slice {}/{}", fr.slice, fr.depth - 1) } else { String::new() },
            if fr.dtype.is_complex() { format!(" · {}", app.part.name()) } else { String::new() }
        ),
        (Some(p), None) => format!(" {} ", base(p)),
        _ => " no SHM ".into(),
    };
    let mut settings = format!(" {} ", mode.name());
    if mode == Mode::Image {
        settings += &format!(
            "· {}{} · {} · {} · {}°{}{}{} ",
            app.cmap.name(),
            if app.rev { "(rev)" } else { "" },
            app.scale.name(),
            app.range.name(),
            app.rot as u32 * 90,
            if app.flipx { " ⇋" } else { "" },
            if app.flipy { " ⇵" } else { "" },
            if app.zoom > 1.0 { format!(" · ×{:.1}", app.zoom) } else { String::new() }
        );
    } else if mode == Mode::Line || mode == Mode::Bars {
        settings += &format!("· {} ", app.range.name());
    }
    if app.avg > 1 {
        settings += &format!("· avg {} ", app.avg);
    }
    if app.reference.is_some() {
        settings += "· −ref ";
    }
    if app.paused {
        settings += "· PAUSED ";
    }
    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(border_style(app.focus == Focus::View))
        .title(Span::styled(title, Style::default().add_modifier(Modifier::BOLD)))
        .title_bottom(Line::from(Span::styled(settings, Style::default().fg(MUTED))).right_aligned());
    let inner = block.inner(area);
    f.render_widget(block, area);
    if let Some(n) = &app.notice {
        f.render_widget(
            Paragraph::new(Span::styled(n.clone(), Style::default().fg(Color::Yellow))).alignment(Alignment::Center).wrap(Wrap { trim: true }),
            Rect { y: inner.y + inner.height / 2, height: 2.min(inner.height), ..inner },
        );
        return;
    }
    if app.frame.is_none() {
        f.render_widget(Paragraph::new("waiting for the first frame…").alignment(Alignment::Center), inner);
        return;
    }
    let (content, strip) = if app.strip != Strip::Off || mode == Mode::Value {
        let h = if mode == Mode::Value { inner.height / 2 } else { (inner.height / 3).clamp(6, 12) };
        let c = Layout::default()
            .direction(Direction::Vertical)
            .constraints([Constraint::Min(3), Constraint::Length(h)])
            .split(inner);
        (c[0], Some(c[1]))
    } else {
        (inner, None)
    };
    match mode {
        Mode::Image | Mode::Auto => draw_image(f, content, app),
        Mode::Line => draw_plot(f, content, app, false),
        Mode::Bars => draw_plot(f, content, app, true),
        Mode::Table => draw_table(f, content, app),
        Mode::Value => draw_value(f, content, app),
    }
    if let Some(s) = strip {
        if app.strip == Strip::Hist && mode != Mode::Value {
            draw_hist(f, s, app);
        } else {
            draw_trend(f, s, app);
        }
    }
}

/* ------------------------------------------------------------------ image */

fn draw_image(f: &mut Frame, area: Rect, app: &mut App) {
    let (h, w) = app.disp_dims();
    let cb_w: u16 = 11;
    if h == 0 || w == 0 || area.width <= cb_w + 2 || area.height < 2 {
        return;
    }
    let (lo, hi) = app.range();
    let lut = app.cmap.table(app.rev);
    let scale = app.scale;
    let tc = app.truecolor;
    let img = Rect { width: area.width - cb_w, ..area };
    let sw = img.width as f64;
    let sh = img.height as f64 * 2.0; // half-block rows: 2 pixels per cell
    let s = (sw / w as f64).min(sh / h as f64) * app.zoom; // screen subpixels per image pixel
    let (cy, cx) = app.cursor;
    let mx = AxisMap::new(w, sw, s, cx);
    let my = AxisMap::new(h, sh, s, cy);
    let inv = 1.0 / (hi - lo);
    let rgb_of = |v: f64| -> (u8, u8, u8) {
        if v.is_nan() {
            return NAN;
        }
        let t = scale.apply(((v - lo) * inv).clamp(0.0, 1.0));
        lut[(t * 255.0).round() as usize]
    };
    let pix = |kx: f64, ky: f64| -> Option<(u8, u8, u8)> {
        let (x, y) = (mx.index(kx)?, my.index(ky)?);
        let (r, c) = app.to_src(y, x);
        app.value_at_src(r, c).map(rgb_of)
    };
    // the cursor's subpixel (when the view is focused)
    let cursor_sub = if app.focus == Focus::View {
        let (sx, sy) = (mx.center(cx).floor(), my.center(cy).floor());
        if sx >= 0.0 && sy >= 0.0 && sx < sw && sy < sh {
            Some((sx as u16, sy as u16))
        } else {
            None
        }
    } else {
        None
    };
    let buf = f.buffer_mut();
    for j in 0..img.height {
        for i in 0..img.width {
            let mut top = pix(i as f64, 2.0 * j as f64);
            let mut bot = pix(i as f64, 2.0 * j as f64 + 1.0);
            if let Some((ci, cj)) = cursor_sub {
                if ci == i {
                    if cj == 2 * j {
                        top = Some(CURSOR);
                    } else if cj == 2 * j + 1 {
                        bot = Some(CURSOR);
                    }
                }
            }
            let cell = &mut buf[(img.x + i, img.y + j)];
            match (top, bot) {
                (Some(t), Some(b)) => {
                    cell.set_char('▀').set_fg(color(tc, t)).set_bg(color(tc, b));
                }
                (Some(t), None) => {
                    cell.set_char('▀').set_fg(color(tc, t)).set_bg(Color::Reset);
                }
                (None, Some(b)) => {
                    cell.set_char('▄').set_fg(color(tc, b)).set_bg(Color::Reset);
                }
                (None, None) => {
                    cell.set_char(' ').set_fg(Color::Reset).set_bg(Color::Reset);
                }
            }
        }
    }
    // values in the pixels once each is wide enough (the cursor's on magenta)
    if s >= 9.0 {
        let int_like = app.frame.as_ref().map(|fr| fr.dtype.is_int()).unwrap_or(false) && app.avg == 1 && app.reference.is_none();
        let maxc = (s.floor() as usize).saturating_sub(1).max(1);
        let (x0, x1) = mx.visible();
        let (y0, y1) = my.visible();
        for yy in y0..y1 {
            for xx in x0..x1 {
                let (r, c) = app.to_src(yy, xx);
                let v = match app.value_at_src(r, c) {
                    Some(v) => v,
                    None => continue,
                };
                let csx = mx.center(xx);
                let csy = my.center(yy) / 2.0; // cells
                if csx < 0.0 || csy < 0.0 || csx >= sw || csy >= img.height as f64 {
                    continue;
                }
                let mut txt = fmt(v, int_like);
                if txt.chars().count() > maxc {
                    txt = txt.chars().take(maxc).collect();
                }
                let n = txt.chars().count() as f64;
                let tx = (csx - n / 2.0).max(0.0) as u16;
                let ty = csy as u16;
                let is_cursor = cursor_sub.is_some() && (yy, xx) == (cy, cx);
                let bg = if is_cursor { CURSOR } else { rgb_of(v) };
                let fg = if luminance(bg) > 140.0 { Color::Black } else { Color::White };
                buf.set_string(img.x + tx, img.y + ty, &txt, Style::default().fg(fg).bg(color(tc, bg)));
            }
        }
    }
    draw_colorbar(buf, Rect { x: img.x + img.width, width: cb_w, ..area }, &lut, scale, lo, hi, tc);
}

/// One image axis on the screen, in subpixels (columns, or half rows). An image that fits is
/// centred and every one of its pixels gets at least one subpixel (a 1-pixel-tall vector is
/// still drawn); zoomed in past the screen, the view follows the cursor.
struct AxisMap {
    n: usize,
    fit: bool,
    pad: f64,
    ext: f64,
    o: f64,
    s: f64,
    screen: f64,
}

impl AxisMap {
    fn new(n: usize, screen: f64, s: f64, cursor: usize) -> AxisMap {
        let full = n as f64 * s;
        if full <= screen {
            let ext = full.round().max(1.0);
            AxisMap { n, fit: true, pad: ((screen - ext) / 2.0).floor(), ext, o: 0.0, s, screen }
        } else {
            let v = screen / s;
            let o = (cursor as f64 + 0.5 - v / 2.0).clamp(0.0, n as f64 - v);
            AxisMap { n, fit: false, pad: 0.0, ext: screen, o, s, screen }
        }
    }
    /// image index at subpixel k
    fn index(&self, k: f64) -> Option<usize> {
        if self.fit {
            if k < self.pad || k >= self.pad + self.ext {
                return None;
            }
            Some((((k - self.pad) * self.n as f64 / self.ext).floor() as usize).min(self.n - 1))
        } else {
            let x = self.o + (k + 0.5) / self.s;
            if x < 0.0 || x >= self.n as f64 {
                None
            } else {
                Some(x as usize)
            }
        }
    }
    /// subpixel of the centre of image index i
    fn center(&self, i: usize) -> f64 {
        if self.fit {
            self.pad + (i as f64 + 0.5) * self.ext / self.n as f64
        } else {
            (i as f64 + 0.5 - self.o) * self.s
        }
    }
    /// image indices on screen
    fn visible(&self) -> (usize, usize) {
        if self.fit {
            (0, self.n)
        } else {
            (self.o.floor().max(0.0) as usize, ((self.o + self.screen / self.s).ceil() as usize).min(self.n))
        }
    }
}

fn draw_colorbar(buf: &mut Buffer, area: Rect, lut: &[(u8, u8, u8)], scale: crate::cmap::Scale, lo: f64, hi: f64, tc: bool) {
    if area.height < 3 {
        return;
    }
    let x = area.x + 1;
    let h = area.height;
    let n = (2 * h) as f64 - 1.0;
    let col = |sub: u16| -> (u8, u8, u8) {
        let t = 1.0 - sub as f64 / n;
        lut[(scale.apply(t).clamp(0.0, 1.0) * 255.0).round() as usize]
    };
    for j in 0..h {
        let (t, b) = (col(2 * j), col(2 * j + 1));
        for dx in 0..2 {
            buf[(x + dx, area.y + j)].set_char('▀').set_fg(color(tc, t)).set_bg(color(tc, b));
        }
    }
    let lab = |v: f64| {
        let s = fmt(v, false);
        s.chars().take((area.width - 4) as usize).collect::<String>()
    };
    let st = Style::default().fg(Color::Gray);
    buf.set_string(x + 3, area.y, lab(hi), st);
    buf.set_string(x + 3, area.y + h / 2, lab((lo + hi) / 2.0), st);
    buf.set_string(x + 3, area.y + h - 1, lab(lo), st);
}

/* ------------------------------------------------------------------ vectors */

fn axis_labels(lo: f64, hi: f64, int: bool) -> Vec<Span<'static>> {
    vec![Span::raw(fmt(lo, int)), Span::raw(fmt((lo + hi) / 2.0, int)), Span::raw(fmt(hi, int))]
}

fn draw_plot(f: &mut Frame, area: Rect, app: &mut App, bars: bool) {
    let d = app.view.clone();
    let n = d.len();
    if n == 0 || area.width < 10 || area.height < 3 {
        return;
    }
    let (mut lo, mut hi) = app.range();
    if app.range == RangeMode::MinMax {
        let pad = (hi - lo) * 0.05;
        lo -= pad;
        hi += pad;
    }
    if bars {
        lo = lo.min(0.0);
        hi = hi.max(0.0);
    }
    let width = area.width.saturating_sub(10) as usize;
    let data: Vec<(f64, f64)> = if n > width * 2 && !bars {
        // min / max envelope per column
        let per = n as f64 / width as f64;
        let mut out = Vec::with_capacity(width * 2);
        for px in 0..width {
            let a = (px as f64 * per) as usize;
            let b = (((px + 1) as f64 * per) as usize).min(n).max(a + 1);
            let (mut mn, mut mx) = (f64::INFINITY, f64::NEG_INFINITY);
            for v in &d[a..b] {
                mn = mn.min(*v);
                mx = mx.max(*v);
            }
            out.push((a as f64, mn));
            out.push((a as f64, mx));
        }
        out
    } else {
        d.iter().enumerate().map(|(i, v)| (i as f64, *v)).collect()
    };
    let ci = app.cursor.1.min(n - 1);
    let cursor = vec![(ci as f64, d[ci])];
    let mut sets = vec![Dataset::default()
        .marker(if bars { symbols::Marker::HalfBlock } else { symbols::Marker::Braille })
        .graph_type(if bars { GraphType::Bar } else { GraphType::Line })
        .style(Style::default().fg(ACCENT))
        .data(&data)];
    if app.focus == Focus::View {
        sets.push(
            Dataset::default()
                .marker(symbols::Marker::Block)
                .graph_type(GraphType::Scatter)
                .style(Style::default().fg(Color::Rgb(CURSOR.0, CURSOR.1, CURSOR.2)))
                .data(&cursor),
        );
    }
    let int_like = app.frame.as_ref().map(|fr| fr.dtype.is_int()).unwrap_or(false);
    let xmax = (n.max(2) - 1) as f64;
    let chart = Chart::new(sets)
        .x_axis(
            Axis::default()
                .bounds([0.0, xmax])
                .labels(vec![Span::raw("0"), Span::raw(format!("{}", (n - 1) / 2)), Span::raw(format!("{}", n - 1))])
                .style(Style::default().fg(MUTED)),
        )
        .y_axis(Axis::default().bounds([lo, hi]).labels(axis_labels(lo, hi, int_like)).style(Style::default().fg(MUTED)));
    f.render_widget(chart, area);
}

/* ------------------------------------------------------------------ table */

fn draw_table(f: &mut Frame, area: Rect, app: &mut App) {
    let (rows, cols) = app.src_dims();
    if rows == 0 || area.height < 2 {
        return;
    }
    let cw: u16 = 11;
    let rh: u16 = 7;
    let vis_c = ((area.width.saturating_sub(rh)) / cw).max(1) as usize;
    let vis_r = (area.height - 1) as usize;
    let (cr, cc) = (app.cursor.0.min(rows - 1), app.cursor.1.min(cols - 1));
    // keep the cursor visible
    if cr < app.table_off.0 {
        app.table_off.0 = cr;
    } else if cr >= app.table_off.0 + vis_r {
        app.table_off.0 = cr + 1 - vis_r;
    }
    if cc < app.table_off.1 {
        app.table_off.1 = cc;
    } else if cc >= app.table_off.1 + vis_c {
        app.table_off.1 = cc + 1 - vis_c;
    }
    let (r0, c0) = app.table_off;
    let int_like = app.frame.as_ref().map(|fr| fr.dtype.is_int()).unwrap_or(false) && app.avg == 1 && app.reference.is_none();
    let buf = f.buffer_mut();
    let head = Style::default().fg(MUTED).add_modifier(Modifier::BOLD);
    for (k, c) in (c0..(c0 + vis_c).min(cols)).enumerate() {
        buf.set_string(area.x + rh + k as u16 * cw, area.y, format!("{:>10}", c), head);
    }
    for (j, r) in (r0..(r0 + vis_r).min(rows)).enumerate() {
        let y = area.y + 1 + j as u16;
        buf.set_string(area.x, y, format!("{:>6}", r), head);
        for (k, c) in (c0..(c0 + vis_c).min(cols)).enumerate() {
            let v = app.view[r * cols + c];
            let mut s = fmt(v, int_like);
            if s.len() > 10 {
                s = format!("{:.3e}", v);
            }
            let st = if r == cr && c == cc && app.focus == Focus::View {
                Style::default().fg(Color::Black).bg(Color::Rgb(CURSOR.0, CURSOR.1, CURSOR.2))
            } else {
                Style::default()
            };
            buf.set_string(area.x + rh + k as u16 * cw, y, format!("{:>10}", s), st);
        }
    }
}

/* ------------------------------------------------------------------ value */

const GLYPHS: [(char, [&str; 5]); 16] = [
    ('0', ["███", "█ █", "█ █", "█ █", "███"]),
    ('1', [" █ ", "██ ", " █ ", " █ ", "███"]),
    ('2', ["███", "  █", "███", "█  ", "███"]),
    ('3', ["███", "  █", " ██", "  █", "███"]),
    ('4', ["█ █", "█ █", "███", "  █", "  █"]),
    ('5', ["███", "█  ", "███", "  █", "███"]),
    ('6', ["███", "█  ", "███", "█ █", "███"]),
    ('7', ["███", "  █", "  █", " █ ", " █ "]),
    ('8', ["███", "█ █", "███", "█ █", "███"]),
    ('9', ["███", "█ █", "███", "  █", "███"]),
    ('.', ["   ", "   ", "   ", "   ", " █ "]),
    ('-', ["   ", "   ", "███", "   ", "   "]),
    ('+', ["   ", " █ ", "███", " █ ", "   "]),
    ('e', ["   ", "███", "█▄█", "█  ", "███"]),
    ('N', ["█ █", "███", "███", "█ █", "█ █"]),
    ('a', ["   ", "██ ", " ██", "█ █", "███"]),
];

fn draw_value(f: &mut Frame, area: Rect, app: &mut App) {
    let v = match app.view.first() {
        Some(v) => *v,
        None => return,
    };
    let int_like = app.frame.as_ref().map(|fr| fr.dtype.is_int()).unwrap_or(false) && app.avg == 1 && app.reference.is_none();
    let s = fmt(v, int_like);
    let width = s.chars().count() as u16 * 4;
    let mut lines: Vec<Line> = Vec::new();
    if width <= area.width && area.height >= 7 {
        for row in 0..5 {
            let mut l = String::new();
            for ch in s.chars() {
                let g = GLYPHS.iter().find(|(c, _)| *c == ch).map(|(_, g)| g[row]).unwrap_or("   ");
                l.push_str(g);
                l.push(' ');
            }
            lines.push(Line::from(Span::styled(l, Style::default().fg(ACCENT).add_modifier(Modifier::BOLD))));
        }
    } else {
        lines.push(Line::from(Span::styled(s, Style::default().fg(ACCENT).add_modifier(Modifier::BOLD))));
    }
    let dt = app.frame.as_ref().map(|fr| fr.dtype.name()).unwrap_or("");
    lines.push(Line::from(""));
    lines.push(Line::from(Span::styled(
        format!("{}{}{}", dt, if app.reference.is_some() { " − ref" } else { "" }, if app.avg > 1 { format!(" · avg {}", app.avg) } else { String::new() }),
        Style::default().fg(MUTED),
    )));
    let top = area.height.saturating_sub(lines.len() as u16) / 2;
    f.render_widget(Paragraph::new(lines).alignment(Alignment::Center), Rect { y: area.y + top, height: area.height - top, ..area });
}

/* ------------------------------------------------------------------ strips */

fn draw_trend(f: &mut Frame, area: Rect, app: &mut App) {
    if app.hist.len() < 2 || area.height < 4 {
        f.render_widget(
            Paragraph::new(Span::styled(format!("trend of {}: collecting…", app.trend.name()), Style::default().fg(MUTED))),
            area,
        );
        return;
    }
    let now = now_epoch();
    let series: Vec<Vec<(f64, f64)>> = if app.trend == Trend::MinMax && app.effective_mode() != Mode::Value {
        vec![
            app.hist.iter().filter(|p| p.a.is_finite()).map(|p| (p.t - now, p.a)).collect(),
            app.hist.iter().filter(|p| p.b.is_finite()).map(|p| (p.t - now, p.b)).collect(),
            app.hist.iter().filter(|p| p.c.is_finite()).map(|p| (p.t - now, p.c)).collect(),
        ]
    } else {
        vec![app.hist.iter().filter(|p| p.a.is_finite()).map(|p| (p.t - now, p.a)).collect()]
    };
    let (mut lo, mut hi) = (f64::INFINITY, f64::NEG_INFINITY);
    for s in &series {
        for (_, v) in s {
            lo = lo.min(*v);
            hi = hi.max(*v);
        }
    }
    if !lo.is_finite() {
        lo = 0.0;
        hi = 1.0;
    }
    if hi <= lo {
        hi = lo + 0.5;
        lo -= 0.5;
    }
    let pad = (hi - lo) * 0.08;
    let (lo, hi) = (lo - pad, hi + pad);
    let t0 = app.hist.front().map(|p| p.t - now).unwrap_or(-1.0).min(-1e-3);
    let colors = if series.len() == 3 { vec![MUTED, ACCENT, MUTED] } else { vec![ACCENT] };
    let sets: Vec<Dataset> = series
        .iter()
        .zip(colors)
        .map(|(s, c)| Dataset::default().marker(symbols::Marker::Braille).graph_type(GraphType::Line).style(Style::default().fg(c)).data(s))
        .collect();
    let last = app.hist.back().map(|p| p.a).unwrap_or(f64::NAN);
    let label = if app.trend == Trend::Pixel {
        match app.cursor_src() {
            Some((r, c)) => format!("pixel [{}, {}]", r, c),
            None => "pixel".into(),
        }
    } else if app.effective_mode() == Mode::Value {
        "value".into()
    } else {
        app.trend.name().into()
    };
    let chart = Chart::new(sets)
        .block(Block::default().borders(Borders::TOP).border_style(Style::default().fg(MUTED)).title(Span::styled(
            format!(" {}: {} (T: change) ", label, fmt(last, false)),
            Style::default().fg(Color::Gray),
        )))
        .x_axis(
            Axis::default()
                .bounds([t0, 0.0])
                .labels(vec![Span::raw(format!("-{}", fmt_dur(-t0))), Span::raw("now")])
                .style(Style::default().fg(MUTED)),
        )
        .y_axis(Axis::default().bounds([lo, hi]).labels(axis_labels(lo, hi, false)).style(Style::default().fg(MUTED)));
    f.render_widget(chart, area);
}

fn draw_hist(f: &mut Frame, area: Rect, app: &mut App) {
    let (lo, hi) = app.last_range;
    let nb = (area.width.saturating_sub(12) as usize).clamp(10, 120);
    let mut counts = vec![0u64; nb];
    let k = nb as f64 / (hi - lo);
    let mut out = 0usize;
    for &v in &app.view {
        let b = ((v - lo) * k).floor();
        if b >= 0.0 && (b as usize) < nb {
            counts[b as usize] += 1;
        } else if v == hi {
            counts[nb - 1] += 1;
        } else {
            out += 1;
        }
    }
    let data: Vec<(f64, f64)> =
        counts.iter().enumerate().map(|(i, c)| (lo + (i as f64 + 0.5) / k, (1.0 + *c as f64).log10())).collect();
    let top = data.iter().map(|p| p.1).fold(0.0, f64::max).max(1.0);
    let chart = Chart::new(vec![Dataset::default()
        .marker(symbols::Marker::HalfBlock)
        .graph_type(GraphType::Bar)
        .style(Style::default().fg(ACCENT))
        .data(&data)])
    .block(Block::default().borders(Borders::TOP).border_style(Style::default().fg(MUTED)).title(Span::styled(
        format!(" histogram, log count · {} bins{} ", nb, if out > 0 { format!(" · {} outside", out) } else { String::new() }),
        Style::default().fg(Color::Gray),
    )))
    .x_axis(Axis::default().bounds([lo, hi]).labels(axis_labels(lo, hi, false)).style(Style::default().fg(MUTED)))
    .y_axis(Axis::default().bounds([0.0, top]).labels(vec![Span::raw("1"), Span::raw(format!("{:.0}", 10f64.powf(top) - 1.0))]).style(Style::default().fg(MUTED)));
    f.render_widget(chart, area);
}

/* ------------------------------------------------------------------ status */

fn draw_status(f: &mut Frame, area: Rect, app: &mut App) {
    let fr = match &app.frame {
        Some(fr) => fr,
        None => {
            f.render_widget(Paragraph::new(Span::styled(" Tab: list ↔ view · / filter · ? help", Style::default().fg(MUTED))), area);
            return;
        }
    };
    let sf = app.selected.as_ref().and_then(|p| app.reg.get(p));
    let rate = sf.map(|s| s.rate).unwrap_or(0.0);
    let live = rate > 0.0;
    let fps = if app.shown.len() > 1 {
        let span = app.shown.back().unwrap().duration_since(*app.shown.front().unwrap()).as_secs_f64();
        if span > 0.05 {
            (app.shown.len() - 1) as f64 / span
        } else {
            0.0
        }
    } else {
        0.0
    };
    let age = if fr.ts > 0.0 { Some(now_epoch() - fr.ts) } else { None };
    let int_like = fr.dtype.is_int() && app.avg == 1 && app.reference.is_none();
    let st = app.stats;
    let mut l1 = vec![
        Span::styled(" cnt0 ", Style::default().fg(MUTED)),
        Span::raw(fr.cnt0.to_string()),
        Span::styled("  cnt2 ", Style::default().fg(MUTED)),
        Span::raw(fr.cnt2.to_string()),
        Span::raw("  "),
        Span::styled(format!("{} Hz", fmt_rate(rate)), Style::default().fg(if live { Color::Green } else { MUTED })),
        Span::styled(format!(" ({:.0} shown/s, at most {})", fps, app.fps), Style::default().fg(MUTED)),
    ];
    if let Some(a) = age {
        if a > -1.0 && a < 1e8 {
            l1.push(Span::raw("  "));
            l1.push(Span::styled(
                format!("written {} ago", fmt_dur(a)),
                Style::default().fg(if !live && a > 5.0 { Color::Yellow } else { MUTED }),
            ));
        }
    }
    let mut l2 = vec![Span::styled(
        format!(
            " min {}  max {}  mean {}  std {}{}",
            fmt(st.min, int_like),
            fmt(st.max, int_like),
            fmt(st.mean, false),
            fmt(st.std, false),
            if st.nonfinite > 0 { format!("  · {} NaN/inf", st.nonfinite) } else { String::new() }
        ),
        Style::default().fg(Color::Gray),
    )];
    if let (Some((r, c)), Some(v)) = (app.cursor_src(), app.cursor_value()) {
        l2.push(Span::raw("   "));
        let where_ = if app.is_1d() && app.effective_mode() != Mode::Image {
            format!("[{}]", if fr.rows == 1 { c } else { r })
        } else {
            format!("[{}, {}]", r, c)
        };
        l2.push(Span::styled(format!("{} = {}", where_, fmt(v, int_like)), Style::default().fg(Color::Rgb(CURSOR.0, CURSOR.1, CURSOR.2))));
    }
    f.render_widget(Paragraph::new(vec![Line::from(l1), Line::from(l2)]), area);
}

/* ------------------------------------------------------------------ popups */

fn centered(area: Rect, w: u16, h: u16) -> Rect {
    let w = w.min(area.width.saturating_sub(2));
    let h = h.min(area.height.saturating_sub(2));
    Rect { x: area.x + (area.width - w) / 2, y: area.y + (area.height - h) / 2, width: w, height: h }
}

fn draw_help(f: &mut Frame, area: Rect) {
    let keys: &[(&str, &str)] = &[
        ("", "anywhere"),
        ("Tab", "switch between the SHM list and the view"),
        ("/", "filter the SHMs (Enter: keep, Esc: clear)"),
        ("space", "pause / resume"),
        ("f", "frames per second at most: 2 5 10 20 30 60"),
        ("i", "info: every metadata field, the semaphores and their readers"),
        ("w", "save the frame as .npy (the SHM's type and shape) in the current directory"),
        ("q", "quit"),
        ("", "SHM list"),
        ("↑ ↓ PgUp PgDn g G", "select an SHM (shown at once)"),
        ("s", "sort: name, update rate, size"),
        ("Enter →", "go to the view"),
        ("", "view"),
        ("← → ↑ ↓", "move the cursor (Shift: by 10); its value in the status line"),
        ("+ - 0", "zoom on the cursor, out, reset"),
        ("m", "view: auto, image, line, bars, table, value"),
        ("c  C", "colour map, reversed"),
        ("s", "scale: linear, sqrt, log, asinh"),
        ("r  l", "range: min-max, 99.9 / 99.5 / 99 %, ±max; lock the current one"),
        ("o  x  y", "rotate 90°, flip left-right, flip up-down"),
        ("[ ]", "cube: previous / next slice"),
        ("p", "complex: |z|, real, imag, phase"),
        ("a", "average the last 1, 4, 16, 64 frames"),
        ("b", "show frame − this frame (b again: back)"),
        ("t  T", "strip: trend, histogram, none; T: trend of mean, std, min/max, pixel, Δcnt0"),
        ("e V R L N", "--write: edit the cursor value, set all, random, load .npy, new SHM"),
        ("Esc", "back to the list"),
        ("", "tabs (1-7; Tab: list ↔ tab)"),
        ("2 Semaphores", "who waits on the selected SHM: semaphore, PID, command"),
        ("3 Record", "n frames · Enter record · c cancel · s save (.npy, .npz)"),
        ("4 Latency", "a start · b end (the list's SHM) · n window · Enter run · x stop · v show"),
        ("5 Snapshots", "c create · Enter load (daoSnapshot.py)"),
        ("6 tmux", "↑↓ session · PgUp PgDn scroll · Enter attach (Ctrl-b d: back here)"),
        ("7 DAQ", "a SHM · f file · d delete · ←→ Enter edit · o root · E/I export/import · S start/finish"),
    ];
    let lines: Vec<Line> = keys
        .iter()
        .map(|(k, d)| {
            if k.is_empty() {
                Line::from(Span::styled(format!(" {}", d), Style::default().fg(ACCENT).add_modifier(Modifier::BOLD)))
            } else {
                Line::from(vec![Span::styled(format!("  {:<18}", k), Style::default().fg(Color::Yellow)), Span::raw(*d)])
            }
        })
        .collect();
    let r = centered(area, 110, lines.len() as u16 + 4);
    f.render_widget(Clear, r);
    f.render_widget(
        Paragraph::new(lines).block(
            Block::default()
                .borders(Borders::ALL)
                .border_type(BorderType::Rounded)
                .border_style(Style::default().fg(ACCENT))
                .title(" keys ")
                .title_bottom(Line::from(" frames are read when cnt0 changes, at most f per second; read-only: no semaphore taken ").centered()),
        ),
        r,
    );
}

fn draw_info(f: &mut Frame, area: Rect, app: &App) {
    // one line per field: long values keep their end (a path's file name)
    let r0 = centered(area, 110, 10);
    let room = r0.width.saturating_sub(15) as usize;
    let fit = |v: &str| -> String {
        let n = v.chars().count();
        if n <= room {
            v.to_string()
        } else {
            format!("…{}", v.chars().skip(n - room + 1).collect::<String>())
        }
    };
    let mut lines: Vec<Line> = app
        .info
        .iter()
        .map(|(k, v)| Line::from(vec![Span::styled(format!(" {:<11}", k), Style::default().fg(MUTED)), Span::raw(fit(v))]))
        .collect();
    lines.push(Line::from(""));
    match &app.sems {
        None => lines.push(Line::from(Span::styled(" semaphore users cannot be read on this system", Style::default().fg(MUTED)))),
        Some(used) => {
            lines.push(Line::from(Span::styled(
                format!(" {} semaphores: {} in use, {} free", app.nsem, used.len(), app.nsem.saturating_sub(used.len())),
                Style::default().add_modifier(Modifier::BOLD),
            )));
            for (s, users) in used {
                if users.is_empty() {
                    lines.push(Line::from(Span::styled(format!("   {:>2}  in use · process not visible", s), Style::default().fg(Color::Yellow))));
                } else {
                    for (pid, cmd) in users {
                        lines.push(Line::from(vec![
                            Span::styled(format!("   {:>2}  PID {:<8}", s, pid), Style::default().fg(Color::Green)),
                            Span::raw(fit(cmd)),
                        ]));
                    }
                }
            }
            if used.is_empty() {
                lines.push(Line::from(Span::styled("   no reader waits on this SHM", Style::default().fg(MUTED))));
            }
        }
    }
    let r = centered(area, 110, lines.len() as u16 + 2);
    f.render_widget(Clear, r);
    f.render_widget(
        Paragraph::new(lines).block(
            Block::default().borders(Borders::ALL).border_type(BorderType::Rounded).border_style(Style::default().fg(ACCENT)).title(" info (refreshed every second) "),
        ),
        r,
    );
}

/* ------------------------------------------------------------------ tabs */

fn tab_block<'a>(title: String, app: &App, keys: &'a str) -> Block<'a> {
    Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(border_style(app.focus == Focus::View))
        .title(Span::styled(title, Style::default().add_modifier(Modifier::BOLD)))
        .title_bottom(Line::from(Span::styled(keys, Style::default().fg(MUTED))).right_aligned())
}

fn draw_tabs(f: &mut Frame, area: Rect, app: &App) {
    let mut spans = Vec::new();
    for (i, (t, name)) in TABS.iter().enumerate() {
        let on = *t == app.tab;
        spans.push(Span::styled(
            format!(" {} {} ", i + 1, name),
            if on { Style::default().fg(Color::Black).bg(ACCENT).add_modifier(Modifier::BOLD) } else { Style::default().fg(MUTED) },
        ));
        spans.push(Span::raw(" "));
    }
    f.render_widget(Paragraph::new(Line::from(spans)), area);
}

fn selected_name(app: &App) -> String {
    app.selected.as_ref().map(|p| base(p)).unwrap_or_else(|| "no SHM".into())
}

fn short_cmd(cmd: &str) -> String {
    let parts: Vec<&str> = cmd.split_whitespace().collect();
    let exe = parts.first().map(|p| p.rsplit('/').next().unwrap_or(p)).unwrap_or("?");
    if exe.starts_with("python") && parts.len() > 1 {
        if parts[1] == "-c" {
            return format!("{} -c", exe);
        }
        if parts[1] == "-m" && parts.len() > 2 {
            return parts[2].to_string();
        }
        if let Some(a) = parts[1..].iter().find(|a| !a.starts_with('-')) {
            return a.rsplit('/').next().unwrap_or(a).to_string();
        }
    }
    exe.to_string()
}

fn draw_sems(f: &mut Frame, area: Rect, app: &mut App) {
    let block = tab_block(format!(" semaphores · {} ", selected_name(app)), app, " refreshed every second ");
    let inner = block.inner(area);
    f.render_widget(block, area);
    let used = match &app.sems {
        None => {
            f.render_widget(Paragraph::new("semaphore users cannot be read on this system (or no SHM selected)"), inner);
            return;
        }
        Some(u) => u.clone(),
    };
    let nsem = app.nsem;
    let head = format!(" {} semaphores: {} in use, {} free", nsem, used.len(), nsem.saturating_sub(used.len()));
    f.render_widget(Paragraph::new(Span::styled(head, Style::default().add_modifier(Modifier::BOLD))), inner);
    // columns of 8, as daoShmViewer
    let ncol = ((nsem + 7) / 8).max(1) as u16;
    let colw = (inner.width / ncol).max(20);
    let buf = f.buffer_mut();
    for s in 0..nsem {
        let col = (s / 8) as u16;
        let row = (s % 8) as u16;
        let x = inner.x + col * colw;
        let y = inner.y + 2 + row * 2;
        if y >= inner.y + inner.height || x >= inner.x + inner.width {
            continue;
        }
        let (txt, st) = match used.iter().find(|(q, _)| *q == s) {
            None => (format!(" {:>2}  free", s), Style::default().fg(Color::Gray).bg(Color::Rgb(38, 42, 48))),
            Some((_, u)) if u.is_empty() => (format!(" {:>2}  in use · process not visible", s), Style::default().fg(Color::White).bg(Color::Rgb(90, 68, 19))),
            Some((_, u)) => (
                format!(" {:>2}  in use{} · PID {} · {}", s, if u.len() > 1 { format!(" +{}", u.len() - 1) } else { String::new() }, u[0].0, short_cmd(&u[0].1)),
                Style::default().fg(Color::White).bg(Color::Rgb(31, 81, 48)),
            ),
        };
        let w = (colw - 2).min(inner.x + inner.width - x) as usize;
        let mut t: String = txt.chars().take(w).collect();
        while t.chars().count() < w {
            t.push(' ');
        }
        buf.set_string(x, y, t, st);
    }
    let note_y = inner.y + 2 + 16;
    if note_y < inner.y + inner.height {
        buf.set_string(inner.x, note_y, " Each reader waiting on this SHM holds a semaphore of its own; this viewer holds none.", Style::default().fg(MUTED));
    }
}

fn draw_record(f: &mut Frame, area: Rect, app: &mut App) {
    let block = tab_block(format!(" quick record · {} ", selected_name(app)), app, " n frames · Enter record · c cancel · s save ");
    let inner = block.inner(area);
    f.render_widget(block, area);
    let mut lines = vec![
        Line::from(vec![Span::styled(" frames   ", Style::default().fg(MUTED)), Span::raw(app.t.rec_n.to_string()), Span::styled("   (n to change)", Style::default().fg(MUTED))]),
        Line::from(vec![Span::styled(" timeout  ", Style::default().fg(MUTED)), Span::raw(format!("{} s without a new frame", app.t.rec_timeout))]),
        Line::from(Span::styled(
            " N consecutive frames of the selected SHM, each with its cnt0, cnt2 and timestamp; frames missed show as gaps in cnt0.",
            Style::default().fg(MUTED),
        )),
        Line::from(Span::styled(" Polled read-only (no semaphore taken). Saved as .npy (the frames) or .npz (frames, cnt0, cnt2, timestamp).", Style::default().fg(MUTED))),
        Line::from(""),
    ];
    let mut gauge = None;
    if let Some(r) = &app.t.rec {
        let g = r.lock().unwrap();
        let secs = if g.state == RecState::Recording { g.t0.elapsed().as_secs_f64() } else { g.secs };
        let name = base(&g.path);
        match &g.state {
            RecState::Recording => {
                lines.push(Line::from(format!(" {}: {} / {} frames · {:.1} s", name, g.done, g.n, secs)));
                gauge = Some(g.done as f64 / g.n.max(1) as f64);
            }
            RecState::Done => {
                let rate = if secs > 0.0 { (g.cnt0.len().max(1) - 1) as f64 / secs } else { 0.0 };
                lines.push(Line::from(Span::styled(
                    format!(" {}: {} frames in {:.2} s · {} Hz · {:.1} MB", name, g.cnt0.len(), secs, fmt_rate(rate), g.raw.len() as f64 / 1048576.0),
                    Style::default().fg(Color::Green),
                )));
                lines.push(Line::from(if g.gaps > 0 {
                    Span::styled(format!(" {} frames missed (gaps in cnt0): the SHM is written faster than they can be copied", g.gaps), Style::default().fg(Color::Yellow))
                } else {
                    Span::styled(" no frame missed", Style::default().fg(Color::Green))
                }));
                gauge = Some(1.0);
            }
            RecState::Failed(e) => lines.push(Line::from(Span::styled(format!(" {}: {}", name, e), Style::default().fg(Color::Red)))),
        }
    }
    if let Some(sv) = &app.t.rec_saved {
        lines.push(Line::from(Span::styled(format!(" saved {}", sv), Style::default().fg(Color::Green))));
    }
    let n = lines.len() as u16;
    f.render_widget(Paragraph::new(lines), inner);
    if let Some(r) = gauge {
        let y = inner.y + n + 1;
        if y < inner.y + inner.height {
            f.render_widget(
                Gauge::default().gauge_style(Style::default().fg(ACCENT).bg(Color::Rgb(40, 44, 52))).ratio(r.clamp(0.0, 1.0)),
                Rect { x: inner.x + 1, y, width: inner.width.saturating_sub(2), height: 1 },
            );
        }
    }
}

fn draw_latency(f: &mut Frame, area: Rect, app: &mut App) {
    let block = tab_block(" SHM to SHM latency (daoTimeDiff) ".into(), app, " a start · b end (list selection) · n window · Enter run · ↑↓ · x stop · v show Array ");
    let inner = block.inner(area);
    f.render_widget(block, area);
    let nm = |p: &Option<std::path::PathBuf>| p.as_ref().map(|p| base(p)).unwrap_or_else(|| "—".into());
    let mut lines = vec![
        Line::from(vec![
            Span::styled(" start ", Style::default().fg(MUTED)),
            Span::raw(nm(&app.t.lat_a)),
            Span::styled("   end ", Style::default().fg(MUTED)),
            Span::raw(nm(&app.t.lat_b)),
            Span::styled("   window ", Style::default().fg(MUTED)),
            Span::raw(app.t.lat_n.to_string()),
        ]),
        Line::from(Span::styled(
            " runs daoTimeDiff -S in its own tmux session (as daoShmViewer, same session name); it waits on a semaphore of its own.",
            Style::default().fg(MUTED),
        )),
        Line::from(""),
        Line::from(Span::styled(" running:", Style::default().add_modifier(Modifier::BOLD))),
    ];
    if app.t.lat_sessions.is_empty() {
        lines.push(Line::from(Span::styled("   none", Style::default().fg(MUTED))));
    }
    for (i, n) in app.t.lat_sessions.iter().enumerate() {
        let on = i == app.t.lat_sel;
        lines.push(Line::from(Span::styled(
            format!(" {} {}", if on { "▶" } else { " " }, n.trim_start_matches(tools::LAT_PREFIX)),
            if on { Style::default().add_modifier(Modifier::BOLD) } else { Style::default() },
        )));
    }
    let top = (lines.len() as u16 + 1).min(inner.height);
    f.render_widget(Paragraph::new(lines), Rect { height: top, ..inner });
    let rest = Rect { y: inner.y + top, height: inner.height.saturating_sub(top), ..inner };
    let d = match &app.t.lat_data {
        Some(d) if rest.height >= 6 => d,
        Some(_) => return,
        None => {
            if !app.t.lat_sessions.is_empty() {
                f.render_widget(Paragraph::new(Span::styled(" waiting for daoTimeDiff's SHMs (in a watched directory?)", Style::default().fg(MUTED))), rest);
            }
            return;
        }
    };
    let v = &d.values;
    let head = format!(" AVG {} µs   RMS {} µs   n {}/{}", fmt(d.avg, false), fmt(d.rms, false), v.len(), d.pop);
    f.render_widget(Paragraph::new(Span::styled(head, Style::default().add_modifier(Modifier::BOLD))), Rect { height: 1, ..rest });
    let plots = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([Constraint::Percentage(58), Constraint::Length(3), Constraint::Min(10)])
        .split(Rect { y: rest.y + 1, height: rest.height - 1, ..rest });
    if v.is_empty() {
        return;
    }
    let (mut lo, mut hi) = v.iter().fold((d.avg - d.rms, d.avg + d.rms), |(a, b), x| (a.min(*x), b.max(*x)));
    if hi <= lo {
        hi = lo + 1.0;
    }
    let pad = (hi - lo) * 0.08;
    lo -= pad;
    hi += pad;
    let pts: Vec<(f64, f64)> = v.iter().enumerate().map(|(i, x)| (i as f64, *x)).collect();
    let n = (v.len().max(2) - 1) as f64;
    let avg = vec![(0.0, d.avg), (n, d.avg)];
    let up = vec![(0.0, d.avg + d.rms), (n, d.avg + d.rms)];
    let dn = vec![(0.0, d.avg - d.rms), (n, d.avg - d.rms)];
    let chart = Chart::new(vec![
        Dataset::default().marker(symbols::Marker::Dot).graph_type(GraphType::Scatter).style(Style::default().fg(ACCENT)).data(&pts),
        Dataset::default().marker(symbols::Marker::Braille).graph_type(GraphType::Line).style(Style::default().fg(Color::Green)).data(&avg),
        Dataset::default().marker(symbols::Marker::Braille).graph_type(GraphType::Line).style(Style::default().fg(Color::DarkGray)).data(&up),
        Dataset::default().marker(symbols::Marker::Braille).graph_type(GraphType::Line).style(Style::default().fg(Color::DarkGray)).data(&dn),
    ])
    .x_axis(Axis::default().bounds([0.0, n]).labels(vec![Span::raw("0"), Span::raw(format!("{}", v.len() - 1))]).style(Style::default().fg(MUTED)))
    .y_axis(Axis::default().bounds([lo, hi]).labels(axis_labels(lo, hi, false)).style(Style::default().fg(MUTED)));
    f.render_widget(chart, plots[0]);
    // histogram, as daoShmViewer's: bins = clip(n / 2, 5, 40)
    let nb = (v.len() / 2).clamp(5, 40);
    let (mn, mx) = v.iter().fold((f64::INFINITY, f64::NEG_INFINITY), |(a, b), x| (a.min(*x), b.max(*x)));
    let w = if mx > mn { mx - mn } else { 1.0 };
    let mut c = vec![0u64; nb];
    for x in v {
        c[(((x - mn) / w) * nb as f64).floor().min(nb as f64 - 1.0).max(0.0) as usize] += 1;
    }
    let top = *c.iter().max().unwrap_or(&1) as f64;
    let hd: Vec<(f64, f64)> = c.iter().enumerate().map(|(i, k)| (mn + (i as f64 + 0.5) * w / nb as f64, *k as f64)).collect();
    let hist = Chart::new(vec![Dataset::default().marker(symbols::Marker::HalfBlock).graph_type(GraphType::Bar).style(Style::default().fg(ACCENT)).data(&hd)])
        .x_axis(Axis::default().bounds([mn, mn + w]).labels(vec![Span::raw(fmt(mn, false)), Span::raw(fmt(mn + w, false))]).style(Style::default().fg(MUTED)))
        .y_axis(Axis::default().bounds([0.0, top.max(1.0)]).labels(vec![Span::raw("0"), Span::raw(format!("{}", top))]).style(Style::default().fg(MUTED)));
    f.render_widget(hist, plots[2]);
}

fn draw_snaps(f: &mut Frame, area: Rect, app: &mut App) {
    let block = tab_block(" snapshots (daoSnapshot.py) ".into(), app, " c create · ↑↓ · Enter load ");
    let inner = block.inner(area);
    f.render_widget(block, area);
    let mut lines = vec![
        Line::from(vec![Span::styled(" directory ", Style::default().fg(MUTED)), Span::raw(app.t.snapdir.display().to_string())]),
        Line::from(Span::styled(
            " create: every SHM of /tmp and the commands of the tmux sessions; load: recreates those SHMs (overwriting) and restarts the commands.",
            Style::default().fg(MUTED),
        )),
        Line::from(""),
    ];
    if let Some(b) = &app.t.snap_busy {
        lines.push(Line::from(Span::styled(format!(" {}", b), Style::default().fg(Color::Yellow))));
        lines.push(Line::from(""));
    }
    if app.t.snaps.is_empty() {
        lines.push(Line::from(Span::styled(" no snapshot", Style::default().fg(MUTED))));
    }
    for (i, s) in app.t.snaps.iter().enumerate() {
        let on = i == app.t.snap_sel;
        let date = {
            let t = s.mtime as libc::time_t;
            let mut tm: libc::tm = unsafe { std::mem::zeroed() };
            unsafe { libc::localtime_r(&t, &mut tm) };
            format!("{:04}-{:02}-{:02} {:02}:{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min)
        };
        lines.push(Line::from(vec![
            Span::styled(format!(" {} {:<40}", if on { "▶" } else { " " }, s.name), if on { Style::default().add_modifier(Modifier::BOLD) } else { Style::default() }),
            Span::styled(format!(" {:>10}  {}", format!("{:.1} MB", s.bytes as f64 / 1048576.0), date), Style::default().fg(MUTED)),
        ]));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

fn draw_tmux(f: &mut Frame, area: Rect, app: &mut App) {
    let block = tab_block(format!(" tmux sessions ({}) ", app.t.sessions.len()), app, " ↑↓ session · PgUp PgDn End scroll · Enter attach (detach: Ctrl-b d) ");
    let inner = block.inner(area);
    f.render_widget(block, area);
    let n = app.t.sessions.len() as u16;
    let list_h = (n + 1).min(inner.height / 3).max(2);
    let mut lines: Vec<Line> = Vec::new();
    if app.t.sessions.is_empty() {
        lines.push(Line::from(Span::styled(" no tmux server running", Style::default().fg(MUTED))));
    }
    for (i, s) in app.t.sessions.iter().enumerate() {
        let on = i == app.t.tmux_sel;
        lines.push(Line::from(vec![
            Span::styled(format!(" {} {:<32}", if on { "▶" } else { " " }, s.name), if on { Style::default().add_modifier(Modifier::BOLD) } else { Style::default() }),
            Span::styled(format!(" {} window{}{}", s.windows, if s.windows > 1 { "s" } else { "" }, if s.attached { " · attached" } else { "" }), Style::default().fg(MUTED)),
        ]));
    }
    let sel = app.t.tmux_sel.min(app.t.sessions.len().saturating_sub(1)) as u16;
    let scroll_list = sel.saturating_sub(list_h.saturating_sub(1));
    f.render_widget(Paragraph::new(lines).scroll((scroll_list, 0)), Rect { height: list_h, ..inner });
    let out = Rect { y: inner.y + list_h, height: inner.height.saturating_sub(list_h), ..inner };
    if out.height < 3 {
        return;
    }
    let text = &app.t.tmux_text;
    let total = text.lines().count() as u16;
    let h = out.height - 1;
    let scroll = total.saturating_sub(h).saturating_sub(app.t.tmux_scroll);
    let title = match app.t.sessions.get(app.t.tmux_sel) {
        Some(s) => format!(" {} (read-only view) ", s.name),
        None => String::new(),
    };
    f.render_widget(
        Paragraph::new(text.clone()).scroll((scroll, 0)).block(Block::default().borders(Borders::TOP).border_style(Style::default().fg(MUTED)).title(Span::styled(title, Style::default().fg(Color::Gray)))),
        out,
    );
}

fn draw_daq(f: &mut Frame, area: Rect, app: &mut App) {
    let block = tab_block(" DAQ (daoDAQCli.py) ".into(), app, " a add SHM · f file · d delete · ↑↓ ←→ Enter edit · o root · E export · I import · S start/finish · R recover ");
    let inner = block.inner(area);
    f.render_widget(block, area);
    let (state, err) = app.t.daq_state.lock().unwrap().clone();
    let (stxt, scol) = match &state {
        Some(s) if s == "Acquiring" => (s.clone(), Color::Green),
        Some(s) if s == "Error" => (s.clone(), Color::Red),
        Some(s) => (s.clone(), Color::Gray),
        None => (if err.is_empty() { "…".into() } else { format!("unreachable ({})", err) }, Color::Red),
    };
    let mut lines = vec![
        Line::from(vec![Span::styled(" state  ", Style::default().fg(MUTED)), Span::styled(stxt, Style::default().fg(scol).add_modifier(Modifier::BOLD))]),
        Line::from(vec![Span::styled(" root   ", Style::default().fg(MUTED)), Span::raw(app.t.daq.root.clone())]),
        Line::from(""),
        Line::from(Span::styled(format!(" sources ({})", app.t.daq.sources.len()), Style::default().add_modifier(Modifier::BOLD))),
    ];
    if app.t.daq.sources.is_empty() {
        lines.push(Line::from(Span::styled("   none: a adds the SHM selected in the list, f a file", Style::default().fg(MUTED))));
    }
    for (i, s) in app.t.daq.sources.iter().enumerate() {
        let on = i == app.t.daq_sel;
        let name = if s.is_smem() { format!("SHM  {}", s.uri.trim_start_matches("smem://")) } else { format!("file {}", s.uri.trim_start_matches("file://")) };
        lines.push(Line::from(Span::styled(format!(" {} {}", if on { "▶" } else { " " }, name), if on { Style::default().add_modifier(Modifier::BOLD) } else { Style::default() })));
        if on && s.is_smem() {
            let mut sp = vec![Span::raw("      ")];
            for (k, key) in tools::SMEM_KEYS.iter().enumerate() {
                let v = s.get(key).unwrap_or(if *key == "eager_start" { "true" } else if *key == "metadata_only" { "false" } else { "—" });
                let st = if k == app.t.daq_field && app.focus == Focus::View {
                    Style::default().fg(Color::Black).bg(Color::Rgb(CURSOR.0, CURSOR.1, CURSOR.2))
                } else {
                    Style::default().fg(Color::Gray)
                };
                sp.push(Span::styled(format!("{}={}", key, v), st));
                sp.push(Span::raw("  "));
            }
            lines.push(Line::from(sp));
        }
    }
    f.render_widget(Paragraph::new(lines).wrap(Wrap { trim: false }), inner);
}

/* ------------------------------------------------------------------ prompt, confirm */

fn draw_prompt(f: &mut Frame, area: Rect, app: &App) {
    if let Some(p) = &app.prompt {
        let r = Rect { x: area.x + 2, y: area.y + area.height.saturating_sub(5), width: area.width.saturating_sub(4), height: 3 };
        f.render_widget(Clear, r);
        f.render_widget(
            Paragraph::new(Line::from(vec![Span::raw(p.buf.clone()), Span::styled("▏", Style::default().fg(ACCENT))])).block(
                Block::default()
                    .borders(Borders::ALL)
                    .border_type(BorderType::Rounded)
                    .border_style(Style::default().fg(ACCENT))
                    .title(format!(" {} ", p.label))
                    .title_bottom(Line::from(" Enter: ok · Esc: cancel ").right_aligned()),
            ),
            r,
        );
    }
    if let Some((m, _)) = &app.confirm {
        let w = (m.chars().count() as u16 + 6).clamp(40, area.width.saturating_sub(4));
        let r = centered(area, w, 5);
        f.render_widget(Clear, r);
        f.render_widget(
            Paragraph::new(vec![Line::from(m.clone()), Line::from(""), Line::from(Span::styled("y: yes · any other key: no", Style::default().fg(MUTED)))])
                .wrap(Wrap { trim: true })
                .alignment(Alignment::Center)
                .block(Block::default().borders(Borders::ALL).border_type(BorderType::Rounded).border_style(Style::default().fg(Color::Yellow)).title(" confirm ")),
            r,
        );
    }
}
