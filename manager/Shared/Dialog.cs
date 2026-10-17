using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace Dxup.Shared
{
    // Message boxes in the application's theme (the Win32 MessageBox stays light).
    public static class Dialog
    {
        public enum Kind { Info, Warning, Error, Question }

        // Shows a message; with yes/no buttons, returns whether the user chose yes.
        public static bool Show(Window owner, string text, Kind kind, string yes = null, string no = null)
        {
            var window = new Window
            {
                Title = owner?.Title ?? "DXUP",
                SizeToContent = SizeToContent.WidthAndHeight,
                ResizeMode = ResizeMode.NoResize,
                ShowInTaskbar = owner == null,
                WindowStartupLocation = owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen,
                Owner = owner,
                FontFamily = new FontFamily("Segoe UI"),
                FontSize = 13,
            };
            window.SetResourceReference(Control.BackgroundProperty, "Panel");
            window.SetResourceReference(Control.ForegroundProperty, "Text");
            Theme.Attach(window);

            var mark = new Border { Width = 28, Height = 28, CornerRadius = new CornerRadius(14), VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 0, 14, 0) };
            mark.SetResourceReference(Border.BackgroundProperty,
                kind == Kind.Error ? "Bad" : kind == Kind.Warning ? "Warn" : "Accent");
            mark.Child = new TextBlock
            {
                Text = kind == Kind.Question ? "?" : "!",
                Foreground = Brushes.White,
                FontWeight = FontWeights.Bold,
                FontSize = 16,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };
            var message = new TextBlock { Text = text, TextWrapping = TextWrapping.Wrap, MaxWidth = 460, VerticalAlignment = VerticalAlignment.Center };
            var body = new DockPanel { Margin = new Thickness(22, 20, 22, 18) };
            DockPanel.SetDock(mark, Dock.Left);
            body.Children.Add(mark);
            body.Children.Add(message);

            bool result = false;
            var buttons = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right };
            var ok = new Button { Content = yes ?? "OK", IsDefault = true, MinWidth = 80, Margin = new Thickness(0) };
            ok.SetResourceReference(FrameworkElement.StyleProperty, "AccentButton");
            ok.Click += (s, e) => { result = true; window.Close(); };
            if (no != null)
            {
                var cancel = new Button { Content = no, IsCancel = true, MinWidth = 80 };
                cancel.Click += (s, e) => window.Close();
                buttons.Children.Add(cancel);
            }
            else
            {
                ok.IsCancel = true;
            }
            buttons.Children.Add(ok);
            var footer = new Border { BorderThickness = new Thickness(0, 1, 0, 0), Padding = new Thickness(22, 10, 22, 10), Child = buttons };
            footer.SetResourceReference(Border.BackgroundProperty, "Ground");
            footer.SetResourceReference(Border.BorderBrushProperty, "Line");

            var root = new DockPanel { MinWidth = 360 };
            DockPanel.SetDock(footer, Dock.Bottom);
            root.Children.Add(footer);
            root.Children.Add(body);
            window.Content = root;
            window.ShowDialog();
            return result;
        }
    }
}
