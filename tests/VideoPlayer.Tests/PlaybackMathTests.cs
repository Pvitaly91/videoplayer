using VideoPlayer.Helpers;

namespace VideoPlayer.Tests;

public sealed class PlaybackMathTests
{
    [Theory]
    [InlineData(-1, 100_000, 0)]
    [InlineData(45_000, 100_000, 45_000)]
    [InlineData(100_001, 100_000, 100_000)]
    [InlineData(1, 0, 0)]
    [InlineData(1, -1, 0)]
    public void ClampPosition_ConstrainsPositionToDuration(
        long positionMilliseconds,
        long durationMilliseconds,
        long expected)
    {
        Assert.Equal(
            expected,
            PlaybackMath.ClampPosition(positionMilliseconds, durationMilliseconds));
    }

    [Theory]
    [InlineData(10_000, 5, 60_000, 15_000)]
    [InlineData(10_000, -5, 60_000, 5_000)]
    [InlineData(10_000, 30, 60_000, 40_000)]
    [InlineData(40_000, -30, 60_000, 10_000)]
    public void CalculateSeekTarget_AppliesRequestedFiveOrThirtySecondOffset(
        long currentMilliseconds,
        int offsetSeconds,
        long durationMilliseconds,
        long expected)
    {
        Assert.Equal(
            expected,
            PlaybackMath.CalculateSeekTarget(
                currentMilliseconds,
                offsetSeconds,
                durationMilliseconds));
    }

    [Theory]
    [InlineData(2_000, -5, 60_000, 0)]
    [InlineData(58_000, 5, 60_000, 60_000)]
    [InlineData(10_000, 30, 20_000, 20_000)]
    [InlineData(10_000, -30, 20_000, 0)]
    [InlineData(10_000, 5, 0, 0)]
    public void CalculateSeekTarget_ConstrainsResultToDuration(
        long currentMilliseconds,
        int offsetSeconds,
        long durationMilliseconds,
        long expected)
    {
        Assert.Equal(
            expected,
            PlaybackMath.CalculateSeekTarget(
                currentMilliseconds,
                offsetSeconds,
                durationMilliseconds));
    }

    [Theory]
    [InlineData(-1, 0)]
    [InlineData(0, 0)]
    [InlineData(55, 55)]
    [InlineData(100, 100)]
    [InlineData(101, 100)]
    public void ClampVolume_ConstrainsVolumeToSupportedRange(int volume, int expected)
    {
        Assert.Equal(expected, PlaybackMath.ClampVolume(volume));
    }
}
