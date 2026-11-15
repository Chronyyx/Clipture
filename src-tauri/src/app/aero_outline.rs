//! The silhouette of the Aero theme's shell, for cutting the window to it.
//!
//! Outside a maximized window the Aero theme shows only its moulded shell,
//! and the window is cut to that shape with a window region, as Windows XP
//! skins were: there is then nothing transparent left for Windows to tint
//! on SDR monitors, and clicks outside the shell reach what lies behind it.
//!
//! The geometry is a copy of the outline in
//! src/renderer/shared/aero/shellArt.ts, which draws the shell; the two must
//! change together. Points are in CSS pixels from the shell's upper left.

/// The shell's size in a 1220 x 763 window; points right of `SHELL_W - ANCHOR_RIGHT`
/// or below `SHELL_H - ANCHOR_BOTTOM` keep their distance from that edge.
const SHELL_W: f64 = 1138.5;
const SHELL_H: f64 = 719.3;
const ANCHOR_RIGHT: f64 = 260.0;
const ANCHOR_BOTTOM: f64 = 185.0;

/// Where the shell sits in the window, in CSS pixels (aero.css `.aero-shell` inset).
pub const SHELL_INSET_LEFT: f64 = 42.3;
pub const SHELL_INSET_TOP: f64 = 9.2;
pub const SHELL_INSET_RIGHT: f64 = 39.2;
pub const SHELL_INSET_BOTTOM: f64 = 34.5;

const TOP_RUN: [(f64, f64); 10] = [(47.0, 23.1), (84.0, 26.2), (130.0, 30.0), (148.5, 30.0), (167.0, 28.5), (190.0, 25.4), (213.0, 21.6), (236.0, 18.5), (268.5, 17.7), (300.0, 17.7)];
const PLATFORM_RUN: [(f64, f64); 7] = [(880.0, 17.7), (890.0, 17.4), (913.0, 14.6), (941.0, 6.9), (960.0, 3.0), (976.0, 1.2), (1000.0, 0.8)];
const FOOT_RUN: [(f64, f64); 10] = [(300.0, 719.3), (240.8, 719.3), (222.0, 717.5), (204.0, 714.0), (186.0, 709.0), (168.0, 704.6), (148.0, 702.0), (127.0, 700.8), (100.0, 700.3), (73.0, 700.2)];

/// A superellipse corner at a vertex, from `reach_a` along `a` to `reach_b` along `b`.
fn corner(vertex: (f64, f64), reach_a: f64, reach_b: f64, a: (f64, f64), b: (f64, f64), exponent: f64) -> Vec<(f64, f64)> {
    let k = 2.0 / exponent;
    (0..=28)
        .map(|i| {
            let angle = std::f64::consts::FRAC_PI_2 * f64::from(i) / 28.0;
            let along_a = reach_a * (1.0 - angle.sin().powf(k));
            let along_b = reach_b * (1.0 - angle.cos().powf(k));
            (vertex.0 + along_a * a.0 + along_b * b.0, vertex.1 + along_a * a.1 + along_b * b.1)
        })
        .collect()
}

/// A Catmull-Rom run through `points`, endpoints included.
fn spline(points: &[(f64, f64)]) -> Vec<(f64, f64)> {
    let mut out = Vec::new();
    let last = points.len() - 1;
    for i in 0..last {
        let (p0, p1, p2, p3) = (points[i.saturating_sub(1)], points[i], points[i + 1], points[(i + 2).min(last)]);
        for s in 0..8 {
            let t = f64::from(s) / 8.0;
            let (t2, t3) = (t * t, t * t * t);
            let at = |c0: f64, c1: f64, c2: f64, c3: f64| {
                0.5 * (2.0 * c1 + (-c0 + c2) * t + (2.0 * c0 - 5.0 * c1 + 4.0 * c2 - c3) * t2 + (-c0 + 3.0 * c1 - 3.0 * c2 + c3) * t3)
            };
            out.push((at(p0.0, p1.0, p2.0, p3.0), at(p0.1, p1.1, p2.1, p3.1)));
        }
    }
    out.push(points[last]);
    out
}

/// The shell's outline for a shell of `width` x `height` CSS pixels, clockwise.
pub fn outline(width: f64, height: f64) -> Vec<(f64, f64)> {
    let (w, h) = (width.max(700.0), height.max(400.0));
    let x = |x: f64| if x > SHELL_W - ANCHOR_RIGHT { w - (SHELL_W - x) } else { x };
    let y = |y: f64| if y > SHELL_H - ANCHOR_BOTTOM { h - (SHELL_H - y) } else { y };
    let mut first = corner((0.0, 23.1), 47.7, 47.0, (0.0, 1.0), (1.0, 0.0), 2.6);
    first.pop();
    let mut last = corner((0.0, 700.2), 73.0, 60.0, (1.0, 0.0), (0.0, -1.0), 2.6);
    last.remove(0);
    first
        .into_iter()
        .chain(spline(&TOP_RUN))
        .chain(spline(&PLATFORM_RUN))
        .chain(corner((SHELL_W, 0.8), 39.0, 36.0, (-1.0, 0.0), (0.0, 1.0), 2.6))
        .chain(corner((SHELL_W, SHELL_H), 50.0, 46.0, (0.0, -1.0), (-1.0, 0.0), 2.2))
        .chain(spline(&FOOT_RUN))
        .chain(last)
        .map(|(px, py)| (x(px), y(py)))
        .collect()
}

/// The window region for a window of `width` x `height` physical pixels at
/// `scale` physical pixels per CSS pixel: the outline placed in the window,
/// moved outward by `grow` physical pixels (inward when negative).
pub fn window_polygon(width: f64, height: f64, scale: f64, grow: f64) -> Vec<(i32, i32)> {
    let shell_w = width / scale - SHELL_INSET_LEFT - SHELL_INSET_RIGHT;
    let shell_h = height / scale - SHELL_INSET_TOP - SHELL_INSET_BOTTOM;
    let points = outline(shell_w, shell_h);
    let count = points.len();
    (0..count)
        .map(|i| {
            let (ax, ay) = points[(i + count - 1) % count];
            let (bx, by) = points[(i + 1) % count];
            let (px, py) = points[i];
            // Outward normal of a clockwise outline (y down): (dy, -dx).
            let length = (bx - ax).hypot(by - ay).max(1e-9);
            let (nx, ny) = ((by - ay) / length, -(bx - ax) / length);
            let wx = (px + SHELL_INSET_LEFT) * scale + nx * grow;
            let wy = (py + SHELL_INSET_TOP) * scale + ny * grow;
            (wx.round() as i32, wy.round() as i32)
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn outline_spans_the_shell_and_keeps_its_edges() {
        let points = outline(SHELL_W, SHELL_H);
        let min_x = points.iter().map(|p| p.0).fold(f64::MAX, f64::min);
        let max_x = points.iter().map(|p| p.0).fold(f64::MIN, f64::max);
        let max_y = points.iter().map(|p| p.1).fold(f64::MIN, f64::max);
        // The foot's spline swells a fraction of a pixel past its traced
        // points, as it does in the drawn art.
        assert!(min_x.abs() < 1e-6 && (max_x - SHELL_W).abs() < 1e-6 && (max_y - SHELL_H).abs() < 0.5);
    }

    #[test]
    fn right_and_bottom_follow_the_window_size() {
        let wide = outline(SHELL_W + 300.0, SHELL_H + 200.0);
        let max_x = wide.iter().map(|p| p.0).fold(f64::MIN, f64::max);
        let max_y = wide.iter().map(|p| p.1).fold(f64::MIN, f64::max);
        assert!((max_x - (SHELL_W + 300.0)).abs() < 1e-6 && (max_y - (SHELL_H + 200.0)).abs() < 0.5);
    }

    #[test]
    fn window_polygon_is_placed_and_scaled_in_the_window() {
        let scale = 1.25;
        let polygon = window_polygon(1220.0 * scale, 763.0 * scale, scale, 0.0);
        let min_x = polygon.iter().map(|p| p.0).min().unwrap();
        let max_x = polygon.iter().map(|p| p.0).max().unwrap();
        assert_eq!(min_x, (SHELL_INSET_LEFT * scale).round() as i32);
        assert_eq!(max_x, ((SHELL_INSET_LEFT + SHELL_W) * scale).round() as i32);
    }
}
