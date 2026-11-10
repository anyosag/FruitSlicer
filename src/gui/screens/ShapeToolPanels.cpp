// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 FruitSlicer contributors
// The shape tools along the view's foot: Cut (a plane through the selected
// part), Label (letters raised or engraved on it) and Combine (join, subtract
// or keep the overlap of two parts). Each has a panel, an apply button pinned
// under it (with progress and Cancel while it works), and its marks in the
// view: the plane, the letters' outline, and the tags A and B.
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

#include "App.h"
#include "Format.h"
#include "Icons.h"
#include "ShapeTools.h"
#include "Theme.h"
#include "Widgets.h"
#include "screens/JobParts.h"

namespace gui::screens::parts {

namespace {

constexpr ImU32 kPlane = IM_COL32(0xFF, 0x8F, 0x3F, 255);
constexpr ImU32 kPlaneFill = IM_COL32(0xFF, 0x8F, 0x3F, 48);
constexpr ImU32 kTagA = IM_COL32(0xFF, 0x8F, 0x3F, 255);
constexpr ImU32 kTagB = IM_COL32(0x6C, 0xB6, 0xFF, 255);

// A note (what the last attempt came to) as a card.
void noteCard(const ShapeState& s, Tool t) {
    if (s.note.empty() || s.note_tool != t) return;
    const bool bad = s.note_bad;
    beginCard("shape_note", ImVec2(0, 0), 12, bad ? col::WarnBg : col::OkBg, bad ? col::WarnBorder : col::Border);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q = ImGui::GetCursorScreenPos();
    if (bad) text("!", fonts().bold, size::Body, col::Warn);
    else icon::check(dl, ImVec2(q.x, q.y + 2), 14, col::Ok);
    ImGui::SetCursorScreenPos(ImVec2(q.x + 22, q.y));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2);
    text(s.note.c_str(), fonts().regular, size::Body, col::TextSecondary, true);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
}

// The slider and a number field for one position, in mm.
bool positionRow(double* v, double lo, double hi) {
    const float w = ImGui::GetContentRegionAvail().x;
    bool changed = false;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Active);
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col::Active);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 8));
    ImGui::SetNextItemWidth(w - 100);
    float f = static_cast<float>(*v);
    if (ImGui::SliderFloat("##position", &f, static_cast<float>(lo), static_cast<float>(hi), "", ImGuiSliderFlags_AlwaysClamp)) {
        *v = std::round(static_cast<double>(f) * 10) / 10;
        changed = true;
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0, 8);
    if (numberField("position", v, 92, "%.1f", lo, hi)) changed = true;
    return changed;
}

// Two number fields side by side, each under its label.
bool twoFields(const char* l0, double* v0, double lo0, double hi0, const char* l1, double* v1, double lo1, double hi1) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float fw = (w - 8) / 2;
    const float x0 = ImGui::GetCursorPosX(), y = ImGui::GetCursorPosY();
    bool changed = false;
    ImGui::SetCursorPos(ImVec2(x0, y));
    text(l0, fonts().regular, size::Help, col::TextMuted);
    ImGui::SetCursorPos(ImVec2(x0, y + 20));
    changed |= numberField(l0, v0, fw, "%g", lo0, hi0);
    ImGui::SetCursorPos(ImVec2(x0 + fw + 8, y));
    text(l1, fonts().regular, size::Help, col::TextMuted);
    ImGui::SetCursorPos(ImVec2(x0 + fw + 8, y + 20));
    changed |= numberField(l1, v1, fw, "%g", lo1, hi1);
    ImGui::SetCursorPos(ImVec2(x0, y + 20 + 34 + 8));
    ImGui::Dummy(ImVec2(0, 0));
    return changed;
}

// One choice of a few, a single line high (radioCard without its description).
bool choiceRow(const char* id, const char* label, bool selected) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 34;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    dl->AddRectFilled(p, q, selected ? IM_COL32(0x2A, 0x1E, 0x14, 255) : (hovered ? col::Card : col::Panel), 10);
    dl->AddRect(p, q, selected ? col::Accent : (hovered ? col::BorderStrong : col::Border), 10, selected ? 1.5f : 1.0f);
    const ImVec2 c(p.x + 12 + 8, p.y + h / 2);
    dl->AddCircle(c, 7.5f, selected ? col::Accent : col::TextMuted, 24, 1.5f);
    if (selected) dl->AddCircleFilled(c, 4, col::Accent, 24);
    ImGui::PushFont(fonts().semibold, size::Body);
    dl->AddText(ImVec2(p.x + 12 + 16 + 10, p.y + (h - ImGui::GetFontSize()) / 2), col::Text, label);
    ImGui::PopFont();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return clicked;
}

std::string capitalised(const char* s) {
    std::string r = s;
    if (!r.empty()) r[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(r[0])));
    return r;
}

// Projects a plate point to the view (false behind the camera).
struct Projector {
    const float* vp;
    ImVec2 origin, size;
    bool operator()(const fruit::Vec3& w, ImVec2* out) const {
        const float x = vp[0] * static_cast<float>(w.x) + vp[4] * static_cast<float>(w.y) + vp[8] * static_cast<float>(w.z) + vp[12];
        const float y = vp[1] * static_cast<float>(w.x) + vp[5] * static_cast<float>(w.y) + vp[9] * static_cast<float>(w.z) + vp[13];
        const float c = vp[3] * static_cast<float>(w.x) + vp[7] * static_cast<float>(w.y) + vp[11] * static_cast<float>(w.z) + vp[15];
        if (c <= 1e-4f) return false;
        *out = ImVec2(origin.x + (x / c * 0.5f + 0.5f) * size.x, origin.y + (0.5f - y / c * 0.5f) * size.y);
        return true;
    }
};

// A small pill with text, centred on `at`.
void pill(ImDrawList* dl, ImVec2 at, const std::string& s, ImU32 fg) {
    ImGui::PushFont(fonts().semibold, size::Help);
    const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
    const ImVec2 a(at.x - ts.x / 2 - 8, at.y - ts.y / 2 - 4), b(at.x + ts.x / 2 + 8, at.y + ts.y / 2 + 4);
    dl->AddRectFilled(a, b, IM_COL32(0x0E, 0x10, 0x14, 225), 8);
    dl->AddRect(a, b, fg, 8.0f, 1.0f);
    dl->AddText(ImVec2(at.x - ts.x / 2, at.y - ts.y / 2), col::Text, s.c_str());
    ImGui::PopFont();
}

bool mouseInView(ImVec2 origin, ImVec2 size) {
    const ImGuiIO& io = ImGui::GetIO();
    return io.MousePos.x >= origin.x && io.MousePos.y >= origin.y + 44 && io.MousePos.x < origin.x + size.x &&
           io.MousePos.y < origin.y + size.y && ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered();
}

bool mouseIdle() {
    return !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Right) &&
           !ImGui::IsMouseDown(ImGuiMouseButton_Middle);
}

bool clickedInView() {
    const ImGuiIO& io = ImGui::GetIO();
    return ImGui::IsMouseReleased(ImGuiMouseButton_Left) && io.MouseDragMaxDistanceSqr[0] < 9;
}

}  // namespace

// ---- Cut ----

void cutPanel(App& app, Document& doc) {
    ShapeState& s = app.shape();
    CutSettings& c = s.cut;
    const Part& part = doc.part();
    cutFit(c, part);
    groupLabel("Cut with a plane");
    struct Axis {
        CutAxis axis;
        const char* label;
    };
    const Axis axes[] = {{CutAxis::Z, "Height"}, {CutAxis::X, "Across X"}, {CutAxis::Y, "Across Y"}};
    for (int i = 0; i < 3; ++i) {
        if (i) ImGui::SameLine(0, 4);
        ImGui::PushID(i);
        if (segmentButton(axes[i].label, c.axis == axes[i].axis)) {
            c.axis = axes[i].axis;
            cutFit(c, part);
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 2));
    const char* help = c.axis == CutAxis::Z   ? "A flat cut at a height above the plate."
                       : c.axis == CutAxis::X ? "An upright cut at a place along X: left of it, and right of it."
                                              : "An upright cut at a place along Y: in front of it, and behind it.";
    text(help, fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));

    const CutRange r = cutRange(part, c.axis);
    text(c.axis == CutAxis::Z ? "Height from the plate (mm)" : c.axis == CutAxis::X ? "Position along X (mm)" : "Position along Y (mm)",
         fonts().semibold, size::Body, col::Text);
    ImGui::Dummy(ImVec2(0, 2));
    if (positionRow(&c.position, r.lo, r.hi)) s.note.clear();
    if (c.axis == CutAxis::Z)
        text(strf("Measured from where it stands, not from the raft. This part is %.1f mm tall.", r.hi - r.lo).c_str(),
             fonts().regular, size::Help, col::TextMuted, true);
    else
        text(strf("From the plate's centre. This part spans %.1f to %.1f mm.", r.lo, r.hi).c_str(), fonts().regular,
             size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));
    if (!cutCrosses(part, c)) {
        beginCard("cut_off", ImVec2(0, 0), 12, col::WarnBg, col::WarnBorder);
        text("The plane is not through the part. Move it between its two ends.", fonts().regular, size::Body, col::TextSecondary, true);
        endCard();
        ImGui::Dummy(ImVec2(0, 4));
    }

    groupLabel("Keep");
    const std::string up = capitalised(cutSideWord(c.axis, true)), down = capitalised(cutSideWord(c.axis, false));
    if (choiceRow("keep_both", "Both, as two parts", c.keep == CutKeep::Both)) c.keep = CutKeep::Both;
    if (choiceRow("keep_above", (up + " only").c_str(), c.keep == CutKeep::Above)) c.keep = CutKeep::Above;
    if (choiceRow("keep_below", (down + " only").c_str(), c.keep == CutKeep::Below)) c.keep = CutKeep::Below;
    ImGui::Dummy(ImVec2(0, 4));
    text(c.keep == CutKeep::Both ? "The pieces are laid out beside each other, each with room for its raft."
                                 : "The other side is thrown away. Undo brings it back.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 4));
    text("The cut faces are filled, so the pieces print solid. A piece above a flat cut comes down onto the plate, "
         "cut face first. Tips and holes you placed by hand on this part are left out.",
         fonts().regular, size::Help, col::TextMuted, true);
}

// ---- Label ----

void labelPanel(App& app, Document& doc) {
    ShapeState& s = app.shape();
    LabelSettings& ls = s.label;
    groupLabel("Text");
    {
        static char buf[kLabelMaxChars + 1];
        const ImGuiID id = ImGui::GetID("##label_text");
        if (ImGui::GetActiveID() != id) {
            std::memset(buf, 0, sizeof buf);
            std::strncpy(buf, ls.text.c_str(), kLabelMaxChars);
        }
        ImGui::PushFont(fonts().regular, size::Body);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Viewport);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::Viewport);
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, col::Viewport);
        ImGui::PushStyleColor(ImGuiCol_Border, col::BorderStrong);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##label_text", "What should it say?", buf, sizeof buf)) {
            ls.text = buf;
            s.note.clear();
        }
        if (ImGui::IsItemActive())
            ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), col::Accent, 6, 1.5f);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(0, 6));
    twoFields("Letter height mm", &ls.height, kLabelMinHeight, kLabelMaxHeight, "Depth mm", &ls.depth, kLabelMinDepth,
              kLabelMaxDepth);
    text("The height is of a capital letter.", fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));

    groupLabel("Style");
    if (segmentButton("Raised", !ls.engrave)) ls.engrave = false;
    ImGui::SameLine(0, 4);
    if (segmentButton("Engraved", ls.engrave)) ls.engrave = true;
    ImGui::Dummy(ImVec2(0, 2));
    text(ls.engrave ? "The letters are cut into the surface, as deep as the depth."
                    : "The letters stand out from the surface, as high as the depth.",
         fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));

    groupLabel("Where");
    const bool placed = labelSpotStillOn(ls, doc) && ls.spot->part == doc.selectedId();
    const LabelPreview& pv = labelPreview(ls);
    beginCard("label_where", ImVec2(0, 0), 12, placed ? col::Card : col::Panel, col::Border);
    if (placed) text("Placed. Click somewhere else on the part to move the letters.", fonts().regular, size::Body,
                     col::TextSecondary, true);
    else text(doc.partCount() > 1 ? "Click the selected part in the view to put the letters there."
                                  : "Click the model in the view to put the letters there.",
              fonts().regular, size::Body, col::TextSecondary, true);
    endCard();
    ImGui::Dummy(ImVec2(0, 4));
    if (!ls.text.empty() && !pv.ok) {
        text(pv.message.c_str(), fonts().regular, size::Help, col::Warn, true);
        ImGui::Dummy(ImVec2(0, 2));
    } else if (!pv.message.empty() && pv.ok) {
        text(pv.message.c_str(), fonts().regular, size::Help, col::TextMuted, true);
        ImGui::Dummy(ImVec2(0, 2));
    }
    text("The outline in the view shows where they will go. The letters are flat, not bent round a curve, so on a "
         "curved surface keep them small.",
         fonts().regular, size::Help, col::TextMuted, true);
}

// ---- Combine ----

void combinePanel(App& app, Document& doc) {
    ShapeState& s = app.shape();
    CombineSettings& cs = s.combine;
    if (doc.partCount() < 2) {
        beginCard("combine_one", ImVec2(0, 0), 12, col::WarnBg, col::WarnBorder);
        text("Combine needs two parts. This job has one.", fonts().semibold, size::Body, col::Text, true);
        text("Add another with Add a part in the Job panel, or cut this one in two.", fonts().regular, size::Body,
             col::TextSecondary, true);
        endCard();
        return;
    }
    const int b = combineSecond(cs, doc);
    groupLabel("A, the selected part");
    beginCard("combine_a", ImVec2(0, 0), 10, col::Card, col::Border);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 9, p.y + 10), 9, kTagA);
        ImGui::PushFont(fonts().bold, size::Help);
        dl->AddText(ImVec2(p.x + 5, p.y + 3), col::OnAccent, "A");
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 26, p.y));
        text(doc.part().name().c_str(), fonts().medium, size::Body, col::Text);
    }
    endCard();
    text("Select another in the Parts list to change A.", fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));

    groupLabel("B, the other part");
    for (const Part& p : doc.parts()) {
        if (p.id() == doc.selectedId()) continue;
        ImGui::PushID(p.id());
        if (radioCard("pick_b", p.name().c_str(), dims(p.fit().size).c_str(), p.id() == b)) app.combinePick(p.id());
        ImGui::PopID();
        ImGui::Dummy(ImVec2(0, 2));
    }
    text("Or click it in the view.", fonts().regular, size::Help, col::TextMuted, true);
    ImGui::Dummy(ImVec2(0, 8));

    groupLabel("What to do");
    if (choiceRow("mode_join", "Join", cs.mode == CombineMode::Join)) cs.mode = CombineMode::Join;
    if (choiceRow("mode_sub", "Subtract (A minus B)", cs.mode == CombineMode::Subtract)) cs.mode = CombineMode::Subtract;
    if (choiceRow("mode_int", "Overlap only", cs.mode == CombineMode::Overlap)) cs.mode = CombineMode::Overlap;
    ImGui::Dummy(ImVec2(0, 4));
    text(cs.mode == CombineMode::Join       ? "Fuses A and B into one solid."
         : cs.mode == CombineMode::Subtract ? "The shape of B is cut out of A, and B goes."
                                            : "Keeps only the space where A and B both are.",
         fonts().regular, size::Help, col::TextSecondary, true);
    ImGui::Dummy(ImVec2(0, 6));
    text("It makes one part, where A is. Both parts must be closed solids: Repair shows what was found. "
         "Tips and holes you placed by hand on them are left out.",
         fonts().regular, size::Help, col::TextMuted, true);
}

// ---- The apply button under the panel, with what the last try came to ----

void shapeApplyArea(App& app, Document& doc, Tool t) {
    ShapeState& s = app.shape();
    noteCard(s, t);
    if (app.shapeBusy()) {
        const JobProgress jp = app.jobs().progress();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col::Accent);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, col::Input);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4);
        ImGui::ProgressBar(static_cast<float>(std::clamp(jp.fraction, 0.03, 1.0)), ImVec2(-1, 8), "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        text((jp.name.empty() ? s.job : jp.name).c_str(), fonts().regular, size::Help, col::TextMuted, true);
        if (secondaryButton("Cancel", ImVec2(-1, 34))) app.shapeCancel();
        return;
    }
    bool enabled = true;
    std::string label;
    const char* why = nullptr;
    if (t == Tool::Cut) {
        const CutSettings& c = s.cut;
        enabled = cutCrosses(doc.part(), c);
        label = c.keep == CutKeep::Both ? "Cut into two parts"
                                        : "Cut, keep the " + std::string(cutSideWord(c.axis, c.keep == CutKeep::Above));
    } else if (t == Tool::Label) {
        const bool placed = labelSpotStillOn(s.label, doc) && s.label.spot->part == doc.selectedId();
        label = s.label.engrave ? "Engrave the text" : "Raise the text";
        enabled = !s.label.text.empty() && placed && labelPreview(s.label).ok;
        if (s.label.text.empty()) why = "Type the text first.";
        else if (!placed) why = "Click the part to say where it goes.";
        else if (!labelPreview(s.label).ok) why = "The letters can't be made.";
    } else if (t == Tool::Combine) {
        enabled = doc.partCount() > 1 && combineSecond(s.combine, doc) >= 0;
        label = combineVerb(s.combine.mode);
        if (doc.partCount() < 2) why = "Needs two parts.";
        else if (!enabled) why = "Pick the second part.";
    }
    if (primaryButton(label.c_str(), ImVec2(-1, 38), enabled)) {
        if (t == Tool::Cut) app.cutApply();
        else if (t == Tool::Label) app.labelApply();
        else if (t == Tool::Combine) app.combineApply();
    }
    if (why) text(why, fonts().regular, size::Help, col::TextMuted, true);
}

const char* shapeHint(App& app, const Document& doc) {
    if (!app.toolPanelOpen()) return nullptr;
    const ShapeState& s = app.shape();
    switch (app.tool()) {
        case Tool::Cut: return "The plane shows where it will be cut · move it with the slider";
        case Tool::Label:
            return labelSpotStillOn(s.label, doc) ? "Click somewhere else on the part to move the letters"
                                                   : "Click the part to put the letters there";
        case Tool::Combine:
            return doc.partCount() < 2 ? nullptr : "Click a part to pick it as B · A is the selected part";
        default: return nullptr;
    }
}

// ---- Marks in the view ----

namespace {

void wireBox(ImDrawList* dl, const Projector& proj, const fruit::BBox3& b, ImU32 c, float thickness) {
    ImVec2 p[8];
    bool ok = true;
    for (int i = 0; i < 8; ++i)
        ok = proj({(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z}, &p[i]) && ok;
    if (!ok) return;
    static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) dl->AddLine(p[e[0]], p[e[1]], c, thickness);
}

void cutOverlay(App& app, Document& doc, const Projector& proj, ImDrawList* dl) {
    CutSettings& c = app.shape().cut;
    const Part& part = doc.part();
    cutFit(c, part);
    const fruit::BBox3 b = part.box();
    const double m = 6, lift = part.placement().lift();
    fruit::Vec3 q[4];
    switch (c.axis) {
        case CutAxis::Z: {
            const double z = lift + c.position;
            q[0] = {b.min.x - m, b.min.y - m, z};
            q[1] = {b.max.x + m, b.min.y - m, z};
            q[2] = {b.max.x + m, b.max.y + m, z};
            q[3] = {b.min.x - m, b.max.y + m, z};
            break;
        }
        case CutAxis::X:
            q[0] = {c.position, b.min.y - m, b.min.z - m};
            q[1] = {c.position, b.max.y + m, b.min.z - m};
            q[2] = {c.position, b.max.y + m, b.max.z + m};
            q[3] = {c.position, b.min.y - m, b.max.z + m};
            break;
        case CutAxis::Y:
            q[0] = {b.min.x - m, c.position, b.min.z - m};
            q[1] = {b.max.x + m, c.position, b.min.z - m};
            q[2] = {b.max.x + m, c.position, b.max.z + m};
            q[3] = {b.min.x - m, c.position, b.max.z + m};
            break;
    }
    ImVec2 p[4];
    for (int i = 0; i < 4; ++i)
        if (!proj(q[i], &p[i])) return;
    const bool ok = cutCrosses(part, c);
    const ImU32 line = ok ? kPlane : IM_COL32(0x8A, 0x92, 0xA0, 255);
    dl->AddConvexPolyFilled(p, 4, ok ? kPlaneFill : IM_COL32(0x8A, 0x92, 0xA0, 36));
    dl->AddPolyline(p, 4, line, 2.0f, ImDrawFlags_Closed);
    const char* word = c.axis == CutAxis::Z ? "Cut at %.1f mm" : c.axis == CutAxis::X ? "Cut at X %.1f mm" : "Cut at Y %.1f mm";
    pill(dl, ImVec2((p[2].x + p[3].x) / 2, (p[2].y + p[3].y) / 2 - 16), strf(word, c.position), line);
}

void labelOverlay(App& app, Document& doc, const Projector& proj, ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    ShapeState& s = app.shape();
    LabelSettings& ls = s.label;
    const Part& part = doc.part();
    const LabelPreview& pv = labelPreview(ls);
    const ImGuiIO& io = ImGui::GetIO();
    auto draw = [&](const fruit::ops::TextPlacement& at, ImU32 c, float w) {
        for (const auto& e : labelOutlineOnPlate(pv, at)) {
            ImVec2 a, b;
            if (proj(e.first, &a) && proj(e.second, &b)) dl->AddLine(a, b, c, w);
        }
    };
    const bool inside = mouseInView(origin, size);
    fruit::Vec3 under, normal;
    bool over_part = false;
    if (inside && mouseIdle()) {
        fruit::Vec3 o, d;
        rayThroughPixel(app.camera(), io.MousePos.x, io.MousePos.y, origin.x, origin.y, size.x, size.y, &o, &d);
        over_part = app.pickPart(o, d) == part.id() && app.pickModel(o, d, &under, &normal);
    }
    const bool placed = labelSpotStillOn(ls, doc) && ls.spot->part == part.id();
    if (placed && pv.ok) {
        const fruit::ops::TextPlacement at = labelPlacement(part, *ls.spot, ls.engrave);
        draw(at, IM_COL32(0x0E, 0x10, 0x14, 200), 4.0f);
        draw(at, kPlane, 2.0f);
    }
    if (over_part) {
        const LabelSpot ghost = labelSpotAt(part, under, normal);
        if (pv.ok) draw(labelPlacement(part, ghost, ls.engrave), IM_COL32(0xFF, 0xB2, 0x7A, 150), 1.5f);
        ImVec2 a;
        if (proj(under, &a)) dl->AddCircle(a, 7, col::Accent, 24, 1.5f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (inside && clickedInView()) {
        fruit::Vec3 o, d;
        rayThroughPixel(app.camera(), io.MousePos.x, io.MousePos.y, origin.x, origin.y, size.x, size.y, &o, &d);
        const int id = app.pickPart(o, d);
        fruit::Vec3 at, n;
        if (id == part.id() && app.pickModel(o, d, &at, &n)) app.labelPick(at, n);
        else if (id >= 0) app.selectPart(id);  // another part: the letters go on that one
    }
}

void combineOverlay(App& app, Document& doc, const Projector& proj, ImDrawList* dl, ImVec2 origin, ImVec2 size) {
    if (doc.partCount() < 2) return;
    const int b = combineSecond(app.shape().combine, doc);
    auto tag = [&](const Part& p, const char* letter, ImU32 c) {
        wireBox(dl, proj, p.box(), c, 1.5f);
        const fruit::BBox3 bx = p.box();
        ImVec2 at;
        if (!proj({(bx.min.x + bx.max.x) / 2, (bx.min.y + bx.max.y) / 2, bx.max.z}, &at)) return;
        at.y -= 22;
        dl->AddCircleFilled(at, 11, c);
        ImGui::PushFont(fonts().bold, size::Body);
        const ImVec2 ts = ImGui::CalcTextSize(letter);
        dl->AddText(ImVec2(at.x - ts.x / 2, at.y - ts.y / 2), col::OnAccent, letter);
        ImGui::PopFont();
    };
    tag(doc.part(), "A", kTagA);
    if (const Part* pb = doc.findPart(b)) tag(*pb, "B", kTagB);
    // A click on another part picks it as B.
    const ImGuiIO& io = ImGui::GetIO();
    if (mouseInView(origin, size) && clickedInView()) {
        fruit::Vec3 o, d;
        rayThroughPixel(app.camera(), io.MousePos.x, io.MousePos.y, origin.x, origin.y, size.x, size.y, &o, &d);
        const int id = app.pickPart(o, d);
        if (id >= 0 && id != doc.selectedId()) app.combinePick(id);
    }
}

}  // namespace

void shapeOverlay(App& app, Document& doc, Tool t, ImVec2 origin, ImVec2 size, const float* vp) {
    const Projector proj{vp, origin, size};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (t == Tool::Cut) cutOverlay(app, doc, proj, dl);
    else if (t == Tool::Label) labelOverlay(app, doc, proj, dl, origin, size);
    else if (t == Tool::Combine) combineOverlay(app, doc, proj, dl, origin, size);
}

}  // namespace gui::screens::parts
