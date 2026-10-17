using System;
using System.Globalization;
using System.Windows;

namespace Dxup.Shared
{
    // Texts in German or English, as Windows' display language: one
    // ResourceDictionary per language (Strings.de.xaml, Strings.en.xaml) in
    // the application, used as {DynamicResource Key} and Loc.T("Key").
    public static class Loc
    {
        public static bool German => CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "de";

        public static void Apply(Application app)
        {
            string name = German ? "Strings.de.xaml" : "Strings.en.xaml";
            app.Resources.MergedDictionaries.Add(new ResourceDictionary { Source = new Uri(name, UriKind.Relative) });
        }

        public static string T(string key) => Application.Current?.TryFindResource(key) as string ?? key;

        public static string F(string key, params object[] args) => string.Format(T(key), args);
    }
}
