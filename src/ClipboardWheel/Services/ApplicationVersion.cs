using System.Reflection;

namespace ClipboardWheel.Services;

public static class ApplicationVersion
{
    public static string CurrentText
    {
        get
        {
            var informational = Assembly.GetEntryAssembly()?
                .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
                .InformationalVersion;

            return string.IsNullOrWhiteSpace(informational)
                ? (Assembly.GetEntryAssembly()?.GetName().Version?.ToString(3) ?? "0.0.0")
                : informational.Split('+', 2)[0];
        }
    }

    public static Version Current
    {
        get
        {
            return Version.TryParse(CurrentText, out var version)
                ? version
                : new Version(0, 0, 0);
        }
    }
}
