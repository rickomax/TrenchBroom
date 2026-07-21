#!/usr/bin/env python3
"""Generate the user-facing Spline Tool documentation PDF for TrenchBroom."""

from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib import colors
from reportlab.lib.styles import getSampleStyleSheet, ParagraphStyle
from reportlab.lib.enums import TA_LEFT
from reportlab.platypus import (
    SimpleDocTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
    HRFlowable,
    ListFlowable,
    ListItem,
)

OUT = "/home/user/TrenchBroom/app/TrenchBroom/resources/documentation/SplineTool.pdf"

# --- Palette (matches the tool's orange accent) -----------------------------
ACCENT = colors.HexColor("#E8820C")
DARK = colors.HexColor("#222222")
MUTED = colors.HexColor("#555555")
RULE = colors.HexColor("#DDDDDD")
CHIP_BG = colors.HexColor("#F3F3F3")

# --- Styles -----------------------------------------------------------------
styles = getSampleStyleSheet()

h_title = ParagraphStyle(
    "TBTitle", parent=styles["Title"], textColor=DARK, fontSize=26,
    spaceAfter=2, alignment=TA_LEFT, fontName="Helvetica-Bold",
)
h_sub = ParagraphStyle(
    "TBSub", parent=styles["Normal"], textColor=ACCENT, fontSize=12,
    spaceAfter=10, fontName="Helvetica-Bold",
)
h1 = ParagraphStyle(
    "TBH1", parent=styles["Heading1"], textColor=DARK, fontSize=14,
    spaceBefore=14, spaceAfter=6, fontName="Helvetica-Bold",
)
h2 = ParagraphStyle(
    "TBH2", parent=styles["Heading2"], textColor=ACCENT, fontSize=11,
    spaceBefore=9, spaceAfter=3, fontName="Helvetica-Bold",
)
body = ParagraphStyle(
    "TBBody", parent=styles["Normal"], textColor=DARK, fontSize=9.5,
    leading=14, spaceAfter=5,
)
small = ParagraphStyle(
    "TBSmall", parent=body, textColor=MUTED, fontSize=8.5, leading=12,
)
cell = ParagraphStyle("TBCell", parent=body, fontSize=9, leading=12, spaceAfter=0)
cell_b = ParagraphStyle(
    "TBCellB", parent=cell, fontName="Helvetica-Bold", textColor=DARK,
)


def bullets(items, st=body):
    return ListFlowable(
        [ListItem(Paragraph(t, st), leftIndent=6, value="•") for t in items],
        bulletType="bullet", bulletColor=ACCENT, leftIndent=10, bulletFontSize=8,
    )


def control_table(rows):
    data = [[Paragraph("Control", cell_b), Paragraph("What it does", cell_b)]]
    for name, desc in rows:
        data.append([Paragraph(name, cell_b), Paragraph(desc, cell)])
    t = Table(data, colWidths=[34 * mm, 132 * mm])
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), ACCENT),
        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ("FONTSIZE", (0, 0), (-1, 0), 9),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("TOPPADDING", (0, 0), (-1, -1), 5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 5),
        ("LEFTPADDING", (0, 0), (-1, -1), 7),
        ("RIGHTPADDING", (0, 0), (-1, -1), 7),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, CHIP_BG]),
        ("LINEBELOW", (0, 0), (-1, -1), 0.4, RULE),
    ]))
    return t


def rule():
    return HRFlowable(width="100%", thickness=0.6, color=RULE,
                      spaceBefore=6, spaceAfter=6)


story = []

# --- Header -----------------------------------------------------------------
story.append(Paragraph("Spline Tool", h_title))
story.append(Paragraph("TrenchBroom &nbsp;|&nbsp; User Guide", h_sub))
story.append(HRFlowable(width="100%", thickness=2, color=ACCENT, spaceAfter=8))

story.append(Paragraph(
    "The Spline Tool sweeps a template brush (or a group of brushes) along a smooth "
    "curve, producing solid, grid-aligned geometry that bends and twists to follow the "
    "path. Use it for pipes, rails, tunnels, cables, arches, tracks &mdash; anything "
    "that repeats a cross-section along a line.", body))

# --- Getting started --------------------------------------------------------
story.append(Paragraph("Getting Started", h1))
story.append(bullets([
    "<b>Activate the tool</b> from the toolbar (the curve icon) or the "
    "<b>Edit &rarr; Tools &rarr; Spline Tool</b> menu. The default shortcut is <b>Y</b>.",
    "When the tool is active, <b>every spline in the map is shown</b> and can be picked "
    "up for editing. Click a spline's points or its generated brushes to start editing "
    "that spline.",
    "The tool's controls appear in the <b>tool bar strip</b> below the main toolbar.",
]))

story.append(Paragraph("Building a spline", h2))
story.append(bullets([
    "Press <b>Add</b> to enter add-point mode, then click in a viewport to drop control "
    "points. Each click appends a point; a preview line shows where the next point will "
    "connect.",
    "Add mode is <b>always off when you switch to the tool</b>, so you never create "
    "stray points by accident. Toggle <b>Add</b> off (or press <b>Esc</b>) to stop "
    "adding and go back to selecting points.",
    "You need at least <b>two points</b> and a linked template before any brushes appear.",
]))

story.append(rule())

# --- Editing points ---------------------------------------------------------
story.append(Paragraph("Editing Control Points", h1))
story.append(bullets([
    "<b>Select</b> a point by clicking it (with Add mode off). The selected point's "
    "Roll, Scale and Locked values are shown in the tool bar.",
    "<b>Drag</b> a point to move it. Hold <b>Alt</b> to move vertically (3D view), and "
    "<b>Ctrl</b> to toggle grid snapping &mdash; exactly like moving a brush.",
    "<b>Arrow keys</b> move the selected point by one grid step; "
    "<b>Page&nbsp;Up / Page&nbsp;Down</b> move it on the vertical axis.",
    "<b>Insert between points:</b> select a point that sits between two others, and the "
    "next point you add is inserted right after it instead of at the end &mdash; handy "
    "for refining a section of the curve.",
    "<b>Remove</b> deletes the selected point (or the last one if none is selected).",
    "<b>Double-click</b> a point to toggle its Locked state quickly.",
]))

story.append(Paragraph("Per-point shape", h2))
story.append(control_table([
    ("Roll", "Twists the swept cross-section around the curve at this point, in "
             "degrees. The twist blends smoothly between points."),
    ("Scale", "Scales the cross-section at this point. Values taper the profile "
              "between points (e.g. 1.0 &rarr; 0.5 makes a cone-like narrowing)."),
    ("Locked", "Anchors the sweep's orientation at this point. A twist introduced by "
               "rolling other points cannot propagate past a locked point, so lock a "
               "point to &ldquo;pin&rdquo; the geometry's up direction there. Locked "
               "points are drawn in a different color."),
]))
story.append(Paragraph(
    "Each point also shows a short reference arrow pointing along its &ldquo;up&rdquo; "
    "direction, so you can see how the profile is oriented before generating brushes.",
    small))

story.append(rule())

# --- Templates --------------------------------------------------------------
story.append(Paragraph("Templates &mdash; the Swept Shape", h1))
story.append(Paragraph(
    "The <b>template</b> is the cross-section that gets repeated along the curve. It can "
    "be a single brush, several brushes, or a whole group.", body))
story.append(control_table([
    ("Link", "Uses the current selection as the template. Select a <b>group</b> to link "
             "by reference (later edits to the group update the spline), or select "
             "<b>one or more brushes</b> to link a snapshot of them."),
    ("Unlink", "Detaches the template. The spline keeps its points but stops generating "
               "brushes until you link a template again."),
    ("Break", "Duplicates the generated brushes as ordinary, editable brushes and then "
              "unlinks the template &mdash; in a single undo step. Use this when you are "
              "happy with the shape and want to hand-edit the result."),
]))
story.append(Paragraph(
    "The template is swept along its <b>X axis</b>: the brush is copied and stretched to "
    "fit each segment of the curve while keeping its cross-section proportions. "
    "Generated brushes are <b>not selectable</b> on their own &mdash; edit the spline "
    "through its points, or press Break to release the brushes.", small))

story.append(rule())

# --- Curve options ----------------------------------------------------------
story.append(Paragraph("Curve Options", h1))
story.append(control_table([
    ("Closed", "Connects the last point back to the first, forming a loop. Brushes are "
               "generated on the closing segment too, and the seam is sealed. Great for "
               "rings, arches and racetracks."),
    ("Subdivisions", "How finely the curve is sampled between control points. Higher "
                     "values give a smoother curve (and more brushes); lower values are "
                     "coarser and cheaper."),
    ("View Options", "Standard TrenchBroom view options for the viewport."),
]))
story.append(Paragraph(
    "Generated vertices are always snapped to the <b>grid (1 unit)</b>, and every swept "
    "cell is built from tetrahedra, so the geometry stays valid and watertight even on "
    "tight curves and twists.", small))

story.append(rule())

# --- Tips -------------------------------------------------------------------
story.append(Paragraph("Tips", h1))
story.append(bullets([
    "Splines are stored as <b>func_group</b> entities, so compilers merge the generated "
    "brushes into the world. They save and load with the map.",
    "Every change &mdash; adding, moving, rolling, closing, breaking &mdash; is a single "
    "<b>undoable</b> step.",
    "Keep the template compact along X for a tight cross-section; the sweep repeats it "
    "as many times as needed to cover the curve.",
    "If a twist &ldquo;unwinds&rdquo; over a segment on a closed loop, lock a point to "
    "control where the orientation is anchored.",
]))

story.append(Spacer(1, 8))
story.append(HRFlowable(width="100%", thickness=0.6, color=RULE, spaceAfter=4))
story.append(Paragraph(
    "TrenchBroom Spline Tool &mdash; user guide. Controls appear in the tool bar strip "
    "when the Spline Tool is active.", small))


def _footer(canvas, doc):
    canvas.saveState()
    canvas.setFont("Helvetica", 7.5)
    canvas.setFillColor(MUTED)
    canvas.drawString(20 * mm, 12 * mm, "TrenchBroom — Spline Tool")
    canvas.drawRightString(190 * mm, 12 * mm, "Page %d" % doc.page)
    canvas.setStrokeColor(RULE)
    canvas.setLineWidth(0.5)
    canvas.line(20 * mm, 14 * mm, 190 * mm, 14 * mm)
    canvas.restoreState()


doc = SimpleDocTemplate(
    OUT, pagesize=A4,
    leftMargin=20 * mm, rightMargin=20 * mm,
    topMargin=18 * mm, bottomMargin=18 * mm,
    title="TrenchBroom Spline Tool — User Guide",
    author="TrenchBroom",
)
doc.build(story, onFirstPage=_footer, onLaterPages=_footer)
print("wrote", OUT)
