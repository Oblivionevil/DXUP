using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using Dxup.Shared;

namespace Dxup.Manager
{
    // Autofind: installed Steam and Battle.net games to add; the likeliest
    // executable of each is preselected.
    public partial class FinderWindow : Window
    {
        public sealed class Row
        {
            public FoundGame Game;
            public string Name => Game.Name;
            public string Store => Game.Store;
            public bool Available { get; set; }
            public bool Selected { get; set; }
            public string Note { get; set; }
            public List<Option> Executables { get; set; }
            public Option Executable { get; set; }
        }

        readonly List<GameEntry> m_known;
        public List<(FoundGame game, string exe)> Chosen { get; } = new List<(FoundGame, string)>();

        public FinderWindow(List<GameEntry> known)
        {
            InitializeComponent();
            Theme.Attach(this);
            m_known = known;
            Loaded += async (s, e) => await Load();
        }

        async Task Load()
        {
            var games = await Task.Run(() => GameFinder.All());
            var rows = new List<Row>();
            foreach (var game in games)
            {
                var options = game.Executables.Select(exe => new Option(RelativePath(game.Dir, exe), exe)).ToList();
                bool known = m_known.Any(entry => GameFinder.IsInside(entry.Dir, game.Dir)
                    || (entry.AppId.Length != 0 && entry.AppId == game.Id));
                // DXUP copied in by hand: adding the game brings it up to date.
                Option ownCopy = known ? null : options.FirstOrDefault(option =>
                    DxupInstall.IsDxupFile(Path.Combine(Path.GetDirectoryName((string)option.Value), "d3d11.dll")));
                rows.Add(new Row
                {
                    Game = game,
                    Available = !known,
                    Selected = ownCopy != null,
                    Note = known ? Loc.T("FindAlready") : ownCopy != null ? Loc.T("FindOwnCopy") : game.Dir,
                    Executables = options,
                    Executable = ownCopy ?? options.FirstOrDefault(),
                });
            }
            GameList.ItemsSource = rows;
            InfoText.Text = Loc.T("FindNone");
            InfoText.Visibility = rows.Count == 0 ? Visibility.Visible : Visibility.Collapsed;
            AddButton.IsEnabled = rows.Count != 0;
        }

        static string RelativePath(string dir, string path) =>
            path.StartsWith(dir, StringComparison.OrdinalIgnoreCase) ? path.Substring(dir.Length).TrimStart('\\') : path;

        void OnAdd(object sender, RoutedEventArgs e)
        {
            foreach (Row row in GameList.ItemsSource ?? new List<Row>())
            {
                if (row.Selected && row.Available && row.Executable?.Value is string exe && File.Exists(exe))
                    Chosen.Add((row.Game, exe));
            }
            DialogResult = true;
        }
    }
}
