using System.Globalization;

namespace VideoPlayer.Helpers;

public static class TimeFormatter
{
    public static string Format(long milliseconds)
    {
        var totalSeconds = Math.Max(0, milliseconds / 1_000);
        var hours = totalSeconds / 3_600;
        var minutes = totalSeconds % 3_600 / 60;
        var seconds = totalSeconds % 60;

        return hours > 0
            ? string.Create(
                CultureInfo.InvariantCulture,
                $"{hours}:{minutes:00}:{seconds:00}")
            : string.Create(
                CultureInfo.InvariantCulture,
                $"{minutes:00}:{seconds:00}");
    }
}
