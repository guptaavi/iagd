using log4net;

namespace IAGrim.UI.Misc {

    /// <summary>
    /// Enlarges an existing control tree's fonts by a factor (IAGD_UI_SCALE).
    ///
    /// WHY NOT the obvious alternatives:
    ///  - Application.SetDefaultFont only reaches controls that never had a font assigned. Most of IAGD's UI
    ///    is designer-generated and the designer writes an explicit Font on anything that differs from the
    ///    default, so on its own it moves almost nothing.
    ///  - Wine's DPI setting (HKCU\Control Panel\Desktop\LogPixels) does scale everything, but IAGD builds
    ///    with ForceDesignerDPIUnaware, and at 144 DPI a maximized window ends up with its layout confined to
    ///    the top-left corner of the frame, leaving most of the window black. Verified on Wine 11.16.
    ///
    /// So instead, walk the tree once after the form is shown and multiply each control's own font. Sizes and
    /// positions are left alone: containers here are docked, anchored or auto-sizing, which absorbs a larger
    /// font. Fixed-size designer panels can still clip at large factors, which is why the scale is clamped.
    /// </summary>
    internal static class UiScaler {
        private static readonly ILog Logger = LogManager.GetLogger(typeof(UiScaler));

        /// <summary>Controls already scaled, so repeat passes are no-ops rather than compounding.</summary>
        private static readonly System.Runtime.CompilerServices.ConditionalWeakTable<Control, object> AlreadyScaled = new();
        private static readonly object Marker = new();

        public static void Apply(Control root, float scale) {
            if (scale <= 1.0f) {
                return;
            }

            var scaled = 0;
            ScaleRecursive(root, scale, ref scaled);

            if (scaled > 0) {
                Logger.Info($"UI scaler enlarged {scaled} control fonts by {scale:0.##}x");
            }
        }

        private static void ScaleRecursive(Control control, float scale, ref int scaled) {
            // Control.Font returns the PARENT's font object when a control has none of its own, so scaling
            // unconditionally multiplies inherited fonts once per nesting level - six levels deep that is
            // 1.6^6, and the UI explodes. Only touch controls that own their font; the rest inherit the
            // already-scaled value for free.
            var owned = control.Parent == null || !ReferenceEquals(control.Font, control.Parent.Font);

            // Callable more than once: the filter panel is a Form that gets torn down and rebuilt whenever the
            // mod selection changes, long after the first pass, and re-scaling an already-scaled control would
            // compound. New controls are absent from the table and get their single pass.
            if (AlreadyScaled.TryGetValue(control, out _)) {
                owned = false;
            }
            else {
                AlreadyScaled.Add(control, Marker);
            }
            // A DataGridView carries its fonts in cell styles rather than on the control, and its rows need to
            // grow with the text or the taller glyphs just get clipped.
            if (control is DataGridView grid) {
                ScaleGrid(grid, scale);
                scaled++;
                return;
            }

            if (owned && control.Font is { } font) {
                control.Font = new Font(font.FontFamily, font.Size * scale, font.Style);
                scaled++;
            }

            foreach (Control child in control.Controls) {
                ScaleRecursive(child, scale, ref scaled);
            }
        }

        private static void ScaleGrid(DataGridView grid, float scale) {
            // A null style font means "inherit the control font", which is the application default that
            // Program already enlarged - scaling it here would apply the factor a second time. Only an
            // explicitly assigned style font needs the multiplication.
            if (grid.DefaultCellStyle.Font is { } cellFont) {
                grid.DefaultCellStyle.Font = new Font(cellFont.FontFamily, cellFont.Size * scale, cellFont.Style);
            }

            if (grid.ColumnHeadersDefaultCellStyle.Font is { } headerFont) {
                grid.ColumnHeadersDefaultCellStyle.Font = new Font(headerFont.FontFamily, headerFont.Size * scale, headerFont.Style);
            }

            grid.ColumnHeadersHeight = (int)(grid.ColumnHeadersHeight * scale);

            // NativeItemGrid already sized its rows off the same scale; anything else gets it here.
            if (grid.RowTemplate.Height < 44 * scale) {
                grid.RowTemplate.Height = (int)(grid.RowTemplate.Height * scale);
                foreach (DataGridViewRow row in grid.Rows) {
                    row.Height = grid.RowTemplate.Height;
                }
            }
        }
    }
}
