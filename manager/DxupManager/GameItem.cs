using System;
using System.ComponentModel;
using System.IO;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Dxup.Shared;

namespace Dxup.Manager
{
    // A game in the list: its games.cfg entry, icon and DXUP state.
    public sealed class GameItem : INotifyPropertyChanged
    {
        public GameEntry Entry { get; }
        public ImageSource Icon { get; }
        public GameState State { get; private set; }

        public GameItem(GameEntry entry)
        {
            Entry = entry;
            Icon = LoadIcon(entry.ExePath);
        }

        public string Name => Entry.Name;
        public string Dir => Entry.Dir;

        public string StateText
        {
            get
            {
                switch (State)
                {
                    case GameState.Active: return Loc.T("StateActive");
                    case GameState.Outdated: return Loc.T("StateOutdated");
                    case GameState.Missing: return Loc.T("StateMissing");
                    default: return Loc.T("StateInactive");
                }
            }
        }

        public Brush StateBrush
        {
            get
            {
                switch (State)
                {
                    case GameState.Active: return (Brush)Application.Current.FindResource("Good");
                    case GameState.Outdated: return (Brush)Application.Current.FindResource("Warn");
                    case GameState.Missing: return (Brush)Application.Current.FindResource("Bad");
                    default: return (Brush)Application.Current.FindResource("Neutral");
                }
            }
        }

        public void Refresh(GameDeployer deployer)
        {
            State = deployer.State(Entry);
            Changed(nameof(Name));
            Changed(nameof(StateText));
            Changed(nameof(StateBrush));
        }

        static ImageSource LoadIcon(string path)
        {
            try
            {
                if (!File.Exists(path))
                    return null;
                using (var icon = System.Drawing.Icon.ExtractAssociatedIcon(path))
                {
                    var image = Imaging.CreateBitmapSourceFromHIcon(icon.Handle, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
                    image.Freeze();
                    return image;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }

        public event PropertyChangedEventHandler PropertyChanged;
        void Changed(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    // An entry of a settings list: the text shown and the value behind it.
    public sealed class Option
    {
        public string Text { get; }
        public object Value { get; }
        public Option(string text, object value) { Text = text; Value = value; }
    }
}
