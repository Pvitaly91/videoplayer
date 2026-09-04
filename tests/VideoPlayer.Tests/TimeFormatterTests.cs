using VideoPlayer.Helpers;

namespace VideoPlayer.Tests;

public sealed class TimeFormatterTests
{
    [Theory]
    [InlineData(0, "00:00")]
    [InlineData(5_000, "00:05")]
    [InlineData(60_000, "01:00")]
    [InlineData(3_599_999, "59:59")]
    [InlineData(-1, "00:00")]
    public void Format_ForDurationsShorterThanOneHour_UsesMinutesAndSeconds(
        long milliseconds,
        string expected)
    {
        Assert.Equal(expected, TimeFormatter.Format(milliseconds));
    }

    [Theory]
    [InlineData(3_600_000, "1:00:00")]
    [InlineData(45_296_000, "12:34:56")]
    [InlineData(360_000_000, "100:00:00")]
    public void Format_ForDurationsOfAtLeastOneHour_UsesHoursMinutesAndSeconds(
        long milliseconds,
        string expected)
    {
        Assert.Equal(expected, TimeFormatter.Format(milliseconds));
    }
}
