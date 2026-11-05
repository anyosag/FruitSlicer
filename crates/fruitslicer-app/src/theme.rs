//! Look and feel: colours, fonts, spacing and a few small widgets the rest
//! of the UI shares (cards, segmented controls, stat tiles, overlay pills).

use eframe::egui::{
    self, epaint, Color32, CornerRadius, FontFamily, FontId, Margin, Painter, Pos2, Rect, Response,
    RichText, Sense, Shadow, Stroke, TextStyle, Ui, Vec2,
};

// ----- palette ------------------------------------------------------------

/// Window background, behind everything.
pub const BG: Color32 = Color32::from_rgb(14, 16, 20);
/// Side panels and bars.
pub const PANEL: Color32 = Color32::from_rgb(21, 24, 30);
/// Cards inside panels.
pub const CARD: Color32 = Color32::from_rgb(28, 32, 40);
/// Input fields and buttons at rest.
pub const FIELD: Color32 = Color32::from_rgb(37, 42, 52);
pub const FIELD_HOVER: Color32 = Color32::from_rgb(47, 53, 65);
pub const BORDER: Color32 = Color32::from_rgb(40, 45, 56);
pub const TEXT: Color32 = Color32::from_rgb(226, 229, 235);
pub const TEXT_WEAK: Color32 = Color32::from_rgb(138, 146, 160);
pub const TEXT_FAINT: Color32 = Color32::from_rgb(98, 106, 120);
/// Tangerine: the one accent colour (primary action, selection).
pub const ACCENT: Color32 = Color32::from_rgb(255, 128, 64);
pub const ACCENT_HOVER: Color32 = Color32::from_rgb(255, 150, 92);
pub const ON_ACCENT: Color32 = Color32::from_rgb(24, 14, 8);
pub const WARN: Color32 = Color32::from_rgb(242, 184, 72);
pub const ERROR: Color32 = Color32::from_rgb(244, 98, 98);
pub const OK: Color32 = Color32::from_rgb(110, 211, 140);
pub const INFO: Color32 = Color32::from_rgb(110, 180, 255);

/// Font family used for headings, buttons and emphasised values.
pub fn semibold() -> FontFamily {
    FontFamily::Name("semibold".into())
}

pub fn install(ctx: &egui::Context) {
    install_fonts(ctx);
    ctx.set_theme(egui::Theme::Dark);
    ctx.style_mut_of(egui::Theme::Dark, style);
}

fn install_fonts(ctx: &egui::Context) {
    let mut fonts = egui::FontDefinitions::default();
    fonts.font_data.insert(
        "inter".into(),
        std::sync::Arc::new(egui::FontData::from_static(include_bytes!(
            "../assets/fonts/Inter-Regular.otf"
        ))),
    );
    fonts.font_data.insert(
        "inter-semibold".into(),
        std::sync::Arc::new(egui::FontData::from_static(include_bytes!(
            "../assets/fonts/Inter-SemiBold.otf"
        ))),
    );
    // Keep egui's own fonts as fallbacks for symbols (⚠ ✔ ✖ …).
    let fallbacks = fonts
        .families
        .get(&FontFamily::Proportional)
        .cloned()
        .unwrap_or_default();
    let mut prop = vec!["inter".to_string()];
    prop.extend(fallbacks.iter().cloned());
    fonts.families.insert(FontFamily::Proportional, prop);
    let mut bold = vec!["inter-semibold".to_string()];
    bold.extend(fallbacks);
    fonts.families.insert(semibold(), bold);
    ctx.set_fonts(fonts);
}

fn style(s: &mut egui::Style) {
    s.text_styles = [
        (TextStyle::Small, FontId::proportional(11.5)),
        (TextStyle::Body, FontId::proportional(13.5)),
        (TextStyle::Button, FontId::proportional(13.5)),
        (TextStyle::Heading, FontId::new(15.0, semibold())),
        (TextStyle::Monospace, FontId::monospace(12.5)),
    ]
    .into();

    let sp = &mut s.spacing;
    sp.item_spacing = Vec2::new(8.0, 7.0);
    sp.button_padding = Vec2::new(10.0, 4.0);
    sp.interact_size = Vec2::new(40.0, 26.0);
    sp.window_margin = Margin::same(14);
    sp.menu_margin = Margin::same(6);
    sp.slider_width = 120.0;
    sp.combo_width = 120.0;
    sp.indent = 16.0;
    sp.scroll = egui::style::ScrollStyle::thin();

    let v = &mut s.visuals;
    v.dark_mode = true;
    v.panel_fill = PANEL;
    v.window_fill = CARD;
    v.window_stroke = Stroke::new(1.0, BORDER);
    v.window_corner_radius = CornerRadius::same(12);
    v.window_shadow = Shadow {
        offset: [0, 8],
        blur: 28,
        spread: 0,
        color: Color32::from_black_alpha(110),
    };
    v.popup_shadow = Shadow {
        offset: [0, 6],
        blur: 18,
        spread: 0,
        color: Color32::from_black_alpha(100),
    };
    v.menu_corner_radius = CornerRadius::same(8);
    v.extreme_bg_color = Color32::from_rgb(17, 19, 24);
    v.faint_bg_color = Color32::from_rgb(25, 28, 35);
    v.code_bg_color = FIELD;
    v.warn_fg_color = WARN;
    v.error_fg_color = ERROR;
    v.hyperlink_color = ACCENT;
    v.selection.bg_fill = ACCENT.gamma_multiply(0.28);
    v.selection.stroke = Stroke::new(1.0, ACCENT);
    v.slider_trailing_fill = true;
    v.handle_shape = egui::style::HandleShape::Circle;
    v.striped = false;

    let r = CornerRadius::same(6);
    let w = &mut v.widgets;
    w.noninteractive.bg_fill = PANEL;
    w.noninteractive.weak_bg_fill = PANEL;
    w.noninteractive.bg_stroke = Stroke::new(1.0, BORDER);
    w.noninteractive.fg_stroke = Stroke::new(1.0, TEXT_WEAK);
    w.noninteractive.corner_radius = r;

    w.inactive.bg_fill = FIELD;
    w.inactive.weak_bg_fill = FIELD;
    w.inactive.bg_stroke = Stroke::NONE;
    w.inactive.fg_stroke = Stroke::new(1.0, TEXT);
    w.inactive.corner_radius = r;

    w.hovered.bg_fill = FIELD_HOVER;
    w.hovered.weak_bg_fill = FIELD_HOVER;
    w.hovered.bg_stroke = Stroke::new(1.0, Color32::from_rgb(70, 78, 94));
    w.hovered.fg_stroke = Stroke::new(1.5, Color32::WHITE);
    w.hovered.corner_radius = r;
    w.hovered.expansion = 0.0;

    w.active.bg_fill = Color32::from_rgb(58, 65, 80);
    w.active.weak_bg_fill = Color32::from_rgb(58, 65, 80);
    w.active.bg_stroke = Stroke::new(1.0, ACCENT);
    w.active.fg_stroke = Stroke::new(1.5, Color32::WHITE);
    w.active.corner_radius = r;
    w.active.expansion = 0.0;

    w.open.bg_fill = FIELD_HOVER;
    w.open.weak_bg_fill = FIELD_HOVER;
    w.open.bg_stroke = Stroke::new(1.0, BORDER);
    w.open.fg_stroke = Stroke::new(1.0, TEXT);
    w.open.corner_radius = r;
}

// ----- building blocks ----------------------------------------------------

/// A rounded, slightly raised group with an optional small caps title.
pub fn card<R>(ui: &mut Ui, title: Option<&str>, add: impl FnOnce(&mut Ui) -> R) -> R {
    egui::Frame::new()
        .fill(CARD)
        .stroke(Stroke::new(1.0, BORDER))
        .corner_radius(CornerRadius::same(10))
        .inner_margin(Margin::symmetric(12, 10))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            if let Some(t) = title {
                section_title(ui, t);
                ui.add_space(2.0);
            }
            add(ui)
        })
        .inner
}

/// Small, letter-spaced heading used at the top of cards and panels.
pub fn section_title(ui: &mut Ui, text: &str) {
    ui.label(
        RichText::new(text.to_uppercase())
            .font(FontId::new(11.0, semibold()))
            .color(TEXT_FAINT)
            .extra_letter_spacing(0.8),
    );
}

/// A muted explanatory line.
pub fn hint(ui: &mut Ui, text: impl Into<String>) {
    ui.label(RichText::new(text.into()).small().color(TEXT_FAINT));
}

/// A coloured note with a tinted background (warnings, notices).
pub fn callout(ui: &mut Ui, color: Color32, text: impl Into<String>) {
    egui::Frame::new()
        .fill(color.gamma_multiply(0.10))
        .stroke(Stroke::new(1.0, color.gamma_multiply(0.35)))
        .corner_radius(CornerRadius::same(6))
        .inner_margin(Margin::symmetric(8, 6))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.label(RichText::new(text.into()).small().color(color));
        });
}

/// The big filled button for the main action.
pub fn primary_button(ui: &mut Ui, enabled: bool, text: &str) -> Response {
    let (fill, fg) = if enabled {
        (ACCENT, ON_ACCENT)
    } else {
        (FIELD, TEXT_FAINT)
    };
    let b = egui::Button::new(
        RichText::new(text)
            .font(FontId::new(14.0, semibold()))
            .color(fg),
    )
    .fill(fill)
    .stroke(Stroke::NONE)
    .corner_radius(CornerRadius::same(7))
    .min_size(Vec2::new(96.0, 30.0));
    let r = ui.add_enabled(enabled, b);
    if enabled && r.hovered() {
        ui.painter().rect_filled(
            r.rect,
            CornerRadius::same(7),
            ACCENT_HOVER.gamma_multiply(0.25),
        );
    }
    r
}

/// A quieter button with an outline, for secondary actions.
pub fn secondary_button(ui: &mut Ui, enabled: bool, text: &str) -> Response {
    let b = egui::Button::new(RichText::new(text).font(FontId::new(13.5, semibold())))
        .fill(FIELD)
        .corner_radius(CornerRadius::same(7))
        .min_size(Vec2::new(0.0, 30.0));
    ui.add_enabled(enabled, b)
}

/// Pill-shaped segmented control. Returns true when the value changed.
pub fn segmented<T: PartialEq + Copy>(
    ui: &mut Ui,
    value: &mut T,
    options: &[(T, &str, bool)],
    stretch: bool,
) -> bool {
    let mut changed = false;
    let h = 28.0;
    let font = FontId::new(13.0, semibold());
    let pad = 14.0;
    let widths: Vec<f32> = options
        .iter()
        .map(|(_, l, _)| {
            ui.fonts_mut(|f| f.layout_no_wrap(l.to_string(), font.clone(), TEXT).size().x)
                + pad * 2.0
        })
        .collect();
    let natural: f32 = widths.iter().sum::<f32>() + 6.0;
    let total = if stretch {
        ui.available_width()
    } else {
        natural
    };
    let (outer, _) = ui.allocate_exact_size(Vec2::new(total, h + 6.0), Sense::hover());
    let p = ui.painter();
    p.rect_filled(outer, CornerRadius::same(9), Color32::from_rgb(17, 19, 24));
    p.rect_stroke(
        outer,
        CornerRadius::same(9),
        Stroke::new(1.0, BORDER),
        egui::StrokeKind::Inside,
    );
    let scale = (total - 6.0) / (natural - 6.0);
    let mut x = outer.left() + 3.0;
    for ((v, label, enabled), w) in options.iter().zip(widths) {
        let w = w * scale;
        let rect = Rect::from_min_size(Pos2::new(x, outer.top() + 3.0), Vec2::new(w, h));
        x += w;
        let id = ui.scope_id().with(("seg", label));
        let resp = ui.interact(
            rect,
            id,
            if *enabled {
                Sense::click()
            } else {
                Sense::hover()
            },
        );
        let selected = *value == *v;
        let p = ui.painter();
        if selected {
            p.rect_filled(rect, CornerRadius::same(7), FIELD_HOVER);
        } else if resp.hovered() && *enabled {
            p.rect_filled(rect, CornerRadius::same(7), FIELD);
        }
        let color = if !enabled {
            TEXT_FAINT.gamma_multiply(0.6)
        } else if selected {
            Color32::WHITE
        } else {
            TEXT_WEAK
        };
        p.text(
            rect.center(),
            egui::Align2::CENTER_CENTER,
            label,
            font.clone(),
            color,
        );
        if selected {
            let bar = Rect::from_center_size(
                Pos2::new(rect.center().x, rect.bottom() - 2.5),
                Vec2::new((w * 0.35).min(28.0), 2.5),
            );
            p.rect_filled(bar, CornerRadius::same(2), ACCENT);
        }
        if resp.clicked() && *enabled && !selected {
            *value = *v;
            changed = true;
        }
    }
    changed
}

/// A labelled number in a small tile.
pub fn stat(ui: &mut Ui, label: &str, value: &str, sub: Option<&str>) {
    egui::Frame::new()
        .fill(Color32::from_rgb(33, 38, 47))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(10, 8))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.spacing_mut().item_spacing.y = 1.0;
            ui.label(RichText::new(label).small().color(TEXT_WEAK));
            ui.label(
                RichText::new(value)
                    .font(FontId::new(17.0, semibold()))
                    .color(TEXT),
            );
            if let Some(s) = sub {
                ui.label(RichText::new(s).small().color(TEXT_FAINT));
            }
        });
}

/// A label + control row with the label in the left column. Use inside a
/// two-column `egui::Grid`.
pub fn row_label(ui: &mut Ui, text: &str) {
    ui.label(RichText::new(text).color(TEXT_WEAK));
}

/// Text in a dark rounded pill, drawn over a view. Returns its rectangle.
pub fn pill(
    painter: &Painter,
    pos: Pos2,
    anchor: egui::Align2,
    text: &str,
    color: Color32,
    font: FontId,
) -> Rect {
    let galley = painter.layout_no_wrap(text.to_string(), font, color);
    let size = galley.size() + Vec2::new(14.0, 6.0);
    let rect = anchor.anchor_size(pos, size);
    painter.rect_filled(
        rect,
        CornerRadius::same(255),
        Color32::from_black_alpha(190),
    );
    painter.rect_stroke(
        rect,
        CornerRadius::same(255),
        Stroke::new(1.0, color.gamma_multiply(0.45)),
        egui::StrokeKind::Inside,
    );
    painter.galley(rect.min + Vec2::new(7.0, 3.0), galley, color);
    rect
}

/// Mouse and key hints along the bottom of a view, drawn as small key caps.
pub fn hints(painter: &Painter, at: Pos2, items: &[(&str, &str)]) {
    let key_font = FontId::new(11.0, semibold());
    let font = FontId::proportional(11.5);
    let mut x = at.x;
    let bottom = at.y;
    // Measure first so the strip can sit on a backdrop.
    let mut parts = Vec::new();
    let mut total = 0.0;
    for (k, a) in items {
        let kg = painter.layout_no_wrap(k.to_string(), key_font.clone(), TEXT);
        let ag = painter.layout_no_wrap(a.to_string(), font.clone(), TEXT_WEAK);
        total += kg.size().x + 10.0 + 5.0 + ag.size().x + 14.0;
        parts.push((kg, ag));
    }
    let h = 20.0;
    let back = Rect::from_min_size(
        Pos2::new(x - 6.0, bottom - h - 6.0),
        Vec2::new(total, h + 6.0),
    );
    painter.rect_filled(
        back,
        CornerRadius::same(8),
        Color32::from_rgba_unmultiplied(14, 16, 20, 215),
    );
    for (kg, ag) in parts {
        let kr = Rect::from_min_size(
            Pos2::new(x, bottom - h - 3.0),
            Vec2::new(kg.size().x + 10.0, h),
        );
        painter.rect_filled(kr, CornerRadius::same(4), FIELD);
        painter.rect_stroke(
            kr,
            CornerRadius::same(4),
            Stroke::new(1.0, Color32::from_rgb(60, 67, 82)),
            egui::StrokeKind::Inside,
        );
        let ky = kr.center().y - kg.size().y / 2.0;
        let aw = ag.size().x;
        let ay = kr.center().y - ag.size().y / 2.0;
        painter.galley(Pos2::new(kr.left() + 5.0, ky), kg, TEXT);
        painter.galley(Pos2::new(kr.right() + 5.0, ay), ag, TEXT_WEAK);
        x = kr.right() + 5.0 + aw + 14.0;
    }
}

/// A vertical gradient fill, for view backgrounds.
pub fn gradient(painter: &Painter, rect: Rect, top: Color32, bottom: Color32) {
    let mut mesh = epaint::Mesh::default();
    mesh.colored_vertex(rect.left_top(), top);
    mesh.colored_vertex(rect.right_top(), top);
    mesh.colored_vertex(rect.left_bottom(), bottom);
    mesh.colored_vertex(rect.right_bottom(), bottom);
    mesh.add_triangle(0, 1, 2);
    mesh.add_triangle(1, 3, 2);
    painter.add(egui::Shape::mesh(mesh));
}

/// The app mark: a fruit slice (a disc with segments), drawn in `rect`.
pub fn logo(painter: &Painter, rect: Rect) {
    let c = rect.center();
    let r = rect.width().min(rect.height()) / 2.0;
    painter.circle_filled(c, r, ACCENT);
    painter.circle_filled(c, r * 0.80, Color32::from_rgb(255, 196, 120));
    for i in 0..6 {
        let a = i as f32 * std::f32::consts::TAU / 6.0 + 0.3;
        let d = Vec2::angled(a);
        painter.line_segment(
            [c + d * r * 0.12, c + d * r * 0.78],
            Stroke::new(1.3, ACCENT),
        );
    }
    painter.circle_filled(c, r * 0.14, ACCENT);
}

/// A small coloured dot followed by text, as one row.
pub fn dot_label(ui: &mut Ui, color: Color32, text: RichText) -> Response {
    ui.horizontal(|ui| {
        let (r, _) = ui.allocate_exact_size(Vec2::splat(10.0), Sense::hover());
        ui.painter().circle_filled(r.center(), 4.0, color);
        ui.label(text)
    })
    .inner
}
